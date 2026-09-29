#include "DlmScriptVm.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "berry.h"

// be_gc.h is an internal header without extern "C" guards; this is the one
// non-API entry point used, declared here rather than pulling that header in.
extern "C" void be_gc_collect(bvm* vm);

// print() inside a script -> the host's log. The Berry port layer (be_port.c)
// declares this and routes be_writebuffer() to it.
extern "C" void dlm_script_write(const char* buffer, size_t length)
{
    DlmScriptVm* const active = DlmScriptVm::s_activeForWrite();
    if (!active)
        return;
    char line[121] = {};
    const size_t n = length < sizeof(line) - 1 ? length : sizeof(line) - 1;
    std::memcpy(line, buffer, n);
    // A trailing newline is print()'s own; the log adds its own framing.
    if (n > 0 && line[n - 1] == '\n')
        line[n - 1] = '\0';
    if (line[0] != '\0')
        active->hostLog(line);
}

DlmScriptVm* DlmScriptVm::s_active = nullptr;

DlmScriptVm* DlmScriptVm::s_activeForWrite()
{
    return s_active;
}

void DlmScriptVm::hostLog(const char* text)
{
    if (m_host.log)
        m_host.log(m_host.context, text);
}

DlmScriptVm::~DlmScriptVm()
{
    unload();
}

void DlmScriptVm::unload()
{
    if (m_vm)
    {
        be_vm_delete(m_vm);
        m_vm = nullptr;
    }
    if (s_active == this)
        s_active = nullptr;
    m_intervalSeconds = INTERVAL_DEFAULT_S;
}

namespace
{
// be_pcall()/be_loadbuffer() leave [exception type, message] on the stack
// for a script exception or a syntax error; fold both into one line for the
// /dlm page. An arena exhaustion (BE_MALLOC_FAIL) is thrown from inside the
// allocator with nothing pushed, so it gets its own words -- and it is the
// one a script author most needs to recognise.
void captureError(bvm* vm, int code, char* error, size_t capacity)
{
    if (!error || capacity == 0)
        return;
    if (code == BE_MALLOC_FAIL)
    {
        std::snprintf(error, capacity, "out of memory: the script arena is exhausted");
        return;
    }
    if (code == BE_EXCEPTION || code == BE_SYNTAX_ERROR)
    {
        const char* type = be_top(vm) >= 2 ? be_tostring(vm, -2) : "error";
        const char* message = be_top(vm) >= 1 ? be_tostring(vm, -1) : "";
        std::snprintf(error, capacity, "%s: %s", type ? type : "error", message ? message : "");
        return;
    }
    std::snprintf(error, capacity, "vm error %d", code);
}
}

bool DlmScriptVm::runProtected(int argc, char* error, size_t errorCapacity)
{
    m_heartbeats = 0;
    const int result = be_pcall(m_vm, argc);
    if (result != 0)
    {
        captureError(m_vm, result, error, errorCapacity);
        be_pop(m_vm, be_top(m_vm));
        return false;
    }
    be_pop(m_vm, be_top(m_vm));
    return true;
}

bool DlmScriptVm::load(
    const char* source, size_t length, const DlmScriptHost& host, char* error, size_t errorCapacity)
{
    if (error && errorCapacity)
        error[0] = '\0';
    unload();
    if (!source || length == 0 || length > SOURCE_MAX)
    {
        if (error)
            std::snprintf(error, errorCapacity, "script is empty or over %u bytes", static_cast<unsigned>(SOURCE_MAX));
        return false;
    }
    if (s_active && s_active != this)
    {
        if (error)
            std::snprintf(error, errorCapacity, "another script VM is already loaded");
        return false;
    }

    m_host = host;
    m_vm = be_vm_new();
    if (!m_vm)
    {
        if (error)
            std::snprintf(error, errorCapacity, "arena too small to create a VM");
        return false;
    }
    s_active = this;
    be_set_obs_hook(m_vm, obsHook);
    registerModule();

    // Top-level statements run now, under the same budget as a tick -- that
    // is where `interval = 60` and the imports happen.
    m_httpCalls = 0;
    m_reportedThisTick = false;
    const int compiled = be_loadbuffer(m_vm, "script", source, length);
    if (compiled != 0)
    {
        captureError(m_vm, compiled, error, errorCapacity);
        unload();
        return false;
    }
    if (!runProtected(0, error, errorCapacity))
    {
        unload();
        return false;
    }
    readInterval();
    return true;
}

void DlmScriptVm::readInterval()
{
    m_intervalSeconds = INTERVAL_DEFAULT_S;
    if (be_getglobal(m_vm, "interval"))
    {
        if (be_isint(m_vm, -1))
        {
            const bint value = be_toint(m_vm, -1);
            if (value < static_cast<bint>(INTERVAL_MIN_S))
                m_intervalSeconds = INTERVAL_MIN_S;
            else if (value > static_cast<bint>(INTERVAL_MAX_S))
                m_intervalSeconds = INTERVAL_MAX_S;
            else
                m_intervalSeconds = static_cast<uint32_t>(value);
        }
    }
    be_pop(m_vm, 1);
}

DlmScriptVm::TickResult DlmScriptVm::tick()
{
    TickResult result;
    if (!m_vm)
    {
        std::snprintf(result.error, sizeof(result.error), "no script loaded");
        return result;
    }
    m_httpCalls = 0;
    m_reported = DlmAdvisory{};
    m_reportedThisTick = false;

    if (!be_getglobal(m_vm, "tick") || !be_isfunction(m_vm, -1))
    {
        be_pop(m_vm, be_top(m_vm));
        std::snprintf(result.error, sizeof(result.error), "script defines no tick() function");
        return result;
    }
    result.ok = runProtected(0, result.error, sizeof(result.error));
    // Everything a tick built is garbage now; collect it so the arena's
    // resident figure (and the next tick's headroom) is real, not deferred.
    be_gc_collect(m_vm);
    result.heartbeats = m_heartbeats;
    result.httpCalls = m_httpCalls;
    if (result.ok)
        result.advisory = m_reported; // present=false if the script never reported
    else
    {
        result.advisory.present = false;
        std::snprintf(result.advisory.action, sizeof(result.advisory.action), "error: %.24s", result.error);
    }
    return result;
}

// ---------------------------------------------------------------------------
// The heartbeat: the whole execution bound.
// ---------------------------------------------------------------------------

void DlmScriptVm::obsHook(bvm* vm, int event, ...)
{
    if (event != BE_OBS_VM_HEARTBEAT || !s_active)
        return;
    DlmScriptVm& self = *s_active;
    ++self.m_heartbeats;
    if (self.m_heartbeats > HEARTBEAT_BUDGET)
    {
        // Longjmps to the enclosing be_pcall(); runProtected() reports it.
        be_raise(vm, "timeout_error", "tick exceeded its instruction budget");
    }
    if (self.m_host.yield && (self.m_heartbeats % YIELD_EVERY_HEARTBEATS) == 0)
        self.m_host.yield(self.m_host.context);
}

// ---------------------------------------------------------------------------
// The `dlm` module
// ---------------------------------------------------------------------------

void DlmScriptVm::registerModule()
{
    be_newmodule(m_vm);
    be_setname(m_vm, -1, "dlm");
    struct Entry
    {
        const char* name;
        bntvfunc function;
    };
    static const Entry ENTRIES[] = {
        {"telemetry", nativeTelemetry},
        {"http", nativeHttp},
        {"report", nativeReport},
        {"log", nativeLog},
        {"secret", nativeSecret},
    };
    for (const Entry& entry : ENTRIES)
    {
        be_pushntvfunction(m_vm, entry.function);
        be_setmember(m_vm, -2, entry.name);
        be_pop(m_vm, 1);
    }
    be_setglobal(m_vm, "dlm");
    be_pop(m_vm, 1);
}

namespace
{
void mapInsertReal(bvm* vm, const char* key, float value)
{
    be_pushstring(vm, key);
    be_pushreal(vm, static_cast<breal>(value));
    be_data_insert(vm, -3);
    be_pop(vm, 2);
}

void mapInsertBool(bvm* vm, const char* key, bool value)
{
    be_pushstring(vm, key);
    be_pushbool(vm, value ? 1 : 0);
    be_data_insert(vm, -3);
    be_pop(vm, 2);
}

void mapInsertInt(bvm* vm, const char* key, bint value)
{
    be_pushstring(vm, key);
    be_pushint(vm, value);
    be_data_insert(vm, -3);
    be_pop(vm, 2);
}

void mapInsertString(bvm* vm, const char* key, const char* value)
{
    be_pushstring(vm, key);
    be_pushstring(vm, value);
    be_data_insert(vm, -3);
    be_pop(vm, 2);
}
}

int DlmScriptVm::nativeTelemetry(bvm* vm)
{
    DlmScriptVm& self = *s_active;
    const DlmTelemetry t = self.m_host.telemetry ? self.m_host.telemetry(self.m_host.context) : DlmTelemetry{};

    // be_newobject pushes the map instance and its raw `.p` map; inserts go
    // to `.p`, then it is popped to leave the instance as the return value.
    be_newobject(vm, "map");
    mapInsertInt(vm, "epoch", static_cast<bint>(t.epochSeconds));
    mapInsertBool(vm, "time_trusted", t.timeTrusted);
    mapInsertBool(vm, "ct_valid", t.ctReadingsValid);
    mapInsertReal(vm, "service_leg_a_amps", t.serviceLegAAmps);
    mapInsertReal(vm, "service_leg_b_amps", t.serviceLegBAmps);
    mapInsertReal(vm, "evse_branch_amps", t.evseBranchAmps);
    mapInsertReal(vm, "allowed_amps", t.allowedAmps);
    mapInsertBool(vm, "allowed_amps_valid", t.allowedAmpsValid);
    mapInsertBool(vm, "relay_permitted", t.relayPermitted);
    mapInsertBool(vm, "relay_closed", t.relayClosed);
    mapInsertString(vm, "safety_state", toString(t.safetyState));
    mapInsertReal(vm, "service_rating_amps", t.serviceRatingAmps);
    mapInsertReal(vm, "evse_breaker_rating_amps", t.evseBreakerRatingAmps);
    mapInsertReal(vm, "continuous_capacity_amps", t.continuousCapacityAmps);
    mapInsertInt(vm, "topology", static_cast<bint>(t.topology));
    mapInsertBool(vm, "solar_installed", t.solarInstalled);
    mapInsertReal(vm, "safety_allowed_amps", t.safetyAllowedAmps);
    mapInsertString(vm, "netzero", NetZero::toString(t.netzeroPhase));
    mapInsertString(vm, "netzero_mode", NetZero::toString(t.netzeroMode));
    mapInsertString(vm, "netzero_leg_a", NetZero::toString(t.netzeroLegA));
    mapInsertString(vm, "netzero_leg_b", NetZero::toString(t.netzeroLegB));
    be_pop(vm, 1);
    be_return(vm);
}

// dlm.http(method, url, headers_map_or_nil, body_or_nil) -> [status, body]
namespace
{
bool authorityChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
        c == ':';
}

char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool sameIgnoringCase(const char* a, size_t aLen, const char* b, size_t bLen)
{
    if (aLen != bLen)
        return false;
    for (size_t i = 0; i < aLen; ++i)
        if (lower(a[i]) != lower(b[i]))
            return false;
    return true;
}

// host[:port] with at most one colon, a non-empty host and, if present, a
// non-empty all-digit port. Writes where the host part ends.
bool splitAuthority(const char* text, size_t length, size_t& hostLength)
{
    hostLength = length;
    bool colon = false;
    for (size_t i = 0; i < length; ++i)
    {
        if (!authorityChar(text[i]))
            return false;
        if (text[i] == ':')
        {
            if (colon)
                return false;
            colon = true;
            hostLength = i;
        }
        else if (colon && (text[i] < '0' || text[i] > '9'))
            return false;
    }
    return hostLength > 0 && (!colon || hostLength + 1 < length);
}
}  // namespace

bool DlmScriptVm::urlHostAllowed(const char* url, const AllowHost* hosts, size_t count)
{
    if (!url || !hosts)
        return false;
    const char* authority = nullptr;
    if (std::strncmp(url, "http://", 7) == 0)
        authority = url + 7;
    else if (std::strncmp(url, "https://", 8) == 0)
        authority = url + 8;
    else
        return false;
    const size_t length = std::strcspn(authority, "/?#");
    size_t hostLength = 0;
    if (length == 0 || length > HTTP_ALLOW_HOST_MAX || !splitAuthority(authority, length, hostLength))
        return false;
    for (size_t i = 0; i < count; ++i)
    {
        const size_t entryLength = std::strlen(hosts[i]);
        if (entryLength == 0)
            continue;
        const bool entryHasPort = std::strchr(hosts[i], ':') != nullptr;
        if (entryHasPort ? sameIgnoringCase(authority, length, hosts[i], entryLength)
                         : sameIgnoringCase(authority, hostLength, hosts[i], entryLength))
            return true;
    }
    return false;
}

bool DlmScriptVm::normalizeAllowHost(const char* line, size_t length, char* out, size_t capacity)
{
    if (!line || !out || capacity == 0)
        return false;
    out[0] = '\0';
    while (length > 0 && (*line == ' ' || *line == '\t'))
        ++line, --length;
    while (length > 0 && (line[length - 1] == ' ' || line[length - 1] == '\t' || line[length - 1] == '\r'))
        --length;
    // A pasted URL: keep only what sits between the scheme and the path.
    for (size_t i = 0; i + 2 < length; ++i)
    {
        if (line[i] == ':' && line[i + 1] == '/' && line[i + 2] == '/')
        {
            line += i + 3;
            length -= i + 3;
            break;
        }
    }
    for (size_t i = 0; i < length; ++i)
    {
        if (line[i] == '/' || line[i] == '?' || line[i] == '#')
        {
            length = i;
            break;
        }
    }
    size_t hostLength = 0;
    if (length == 0 || length > HTTP_ALLOW_HOST_MAX || length + 1 > capacity ||
        !splitAuthority(line, length, hostLength))
        return false;
    for (size_t i = 0; i < length; ++i)
        out[i] = lower(line[i]);
    out[length] = '\0';
    return true;
}

int DlmScriptVm::nativeHttp(bvm* vm)
{
    DlmScriptVm& self = *s_active;
    int status = HTTP_BAD_ARGS;
    const char* body = "";
    size_t bodyLen = 0;

    const int argc = be_top(vm);
    if (argc >= 2 && be_isstring(vm, 1) && be_isstring(vm, 2) && self.m_host.http)
    {
        const char* method = be_tostring(vm, 1);
        const char* url = be_tostring(vm, 2);
        const bool methodOk = std::strcmp(method, "GET") == 0 || std::strcmp(method, "POST") == 0 ||
            std::strcmp(method, "PATCH") == 0 || std::strcmp(method, "PUT") == 0 ||
            std::strcmp(method, "DELETE") == 0;
        const bool urlOk = std::strncmp(url, "http://", 7) == 0 || std::strncmp(url, "https://", 8) == 0;

        // Headers: a map instance (or nil) flattened to "Name: value\n" lines
        // in a bounded buffer; an oversized set is a bad-args failure, not a
        // silent truncation.
        char headers[HTTP_HEADERS_MAX] = {};
        bool headersOk = true;
        if (argc >= 3 && be_isinstance(vm, 3))
        {
            size_t used = 0;
            be_getmember(vm, 3, ".p");
            if (be_ismap(vm, -1))
            {
                be_pushiter(vm, -1);
                while (be_iter_hasnext(vm, -2))
                {
                    be_iter_next(vm, -2);
                    const char* key = be_isstring(vm, -2) ? be_tostring(vm, -2) : nullptr;
                    const char* value = be_isstring(vm, -1) ? be_tostring(vm, -1) : nullptr;
                    if (key && value)
                    {
                        const int written = std::snprintf(
                            headers + used, sizeof(headers) - used, "%s: %s\n", key, value);
                        if (written < 0 || used + static_cast<size_t>(written) >= sizeof(headers))
                            headersOk = false;
                        else
                            used += static_cast<size_t>(written);
                    }
                    be_pop(vm, 2);
                }
                be_pop(vm, 1);
            }
            be_pop(vm, 1);
        }

        const char* requestBody = (argc >= 4 && be_isstring(vm, 4)) ? be_tostring(vm, 4) : nullptr;

        if (!methodOk || !urlOk || !headersOk)
            status = HTTP_BAD_ARGS;
        else if (self.m_host.httpAllowed && !self.m_host.httpAllowed(self.m_host.context, url))
            status = HTTP_HOST_BLOCKED;
        else if (self.m_httpCalls >= MAX_HTTP_PER_TICK)
            status = HTTP_TOO_MANY;
        else
        {
            ++self.m_httpCalls;
            const char* out = nullptr;
            size_t outLen = 0;
            status = self.m_host.http(self.m_host.context, method, url, headers, requestBody, &out, &outLen);
            if (out && outLen > 0)
            {
                body = out;
                bodyLen = outLen > HTTP_BODY_MAX ? HTTP_BODY_MAX : outLen;
            }
        }
    }

    be_newobject(vm, "list");
    be_pushint(vm, status);
    be_data_push(vm, -2);
    be_pop(vm, 1);
    be_pushnstring(vm, body, bodyLen);
    be_data_push(vm, -2);
    be_pop(vm, 1);
    be_pop(vm, 1);
    be_return(vm);
}

// dlm.report(present, action)
int DlmScriptVm::nativeReport(bvm* vm)
{
    DlmScriptVm& self = *s_active;
    const int argc = be_top(vm);
    self.m_reported = DlmAdvisory{};
    self.m_reported.present = argc >= 1 && be_isbool(vm, 1) && be_tobool(vm, 1);
    if (argc >= 2 && be_isstring(vm, 2))
        DlmLogic::sanitiseAction(be_tostring(vm, 2), self.m_reported.action, sizeof(self.m_reported.action));
    self.m_reportedThisTick = true;
    be_return_nil(vm);
}

int DlmScriptVm::nativeLog(bvm* vm)
{
    DlmScriptVm& self = *s_active;
    if (be_top(vm) >= 1 && be_isstring(vm, 1))
    {
        char line[121] = {};
        std::snprintf(line, sizeof(line), "%s", be_tostring(vm, 1));
        self.hostLog(line);
    }
    be_return_nil(vm);
}

int DlmScriptVm::nativeSecret(bvm* vm)
{
    DlmScriptVm& self = *s_active;
    const char* value = nullptr;
    if (be_top(vm) >= 1 && be_isstring(vm, 1) && self.m_host.secret)
        value = self.m_host.secret(self.m_host.context, be_tostring(vm, 1));
    if (value)
        be_pushstring(vm, value);
    else
        be_pushnil(vm);
    be_return(vm);
}
