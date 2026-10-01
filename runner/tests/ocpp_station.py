#!/usr/bin/env python3
"""A pretend OCPP 1.6J charging station, standard library only.

Starts `cxl-run scripts/ocpp.be --ws-listen`, connects to it the way a real
station would (Basic auth, subprotocol ocpp1.6), boots, starts a
transaction, and accepts every charging profile it is sent. Exits 0 when
the script asked for the expected limits, in order, as the headroom moved.
"""
import base64, json, os, socket, struct, subprocess, sys, time

PORT = 19000 + os.getpid() % 1000
HERE = os.path.dirname(os.path.abspath(__file__))
RUNNER = os.path.join(HERE, "..", "cxl-run")
SCRIPT = os.path.join(HERE, "..", "..", "scripts", "ocpp.be")

# Telemetry changes by tick, and the limit each change must produce.
TIMELINE = "1 allowed_amps=24 continuous_capacity_amps=32\n4 allowed_amps=18\n6 allowed_amps=4\n"
EXPECTED = [23, 17, 0]


def frame(text):
    data = text.encode()
    mask = os.urandom(4)
    head = bytes([0x81])
    head += bytes([0x80 | len(data)]) if len(data) < 126 else bytes([0x80 | 126]) + struct.pack(">H", len(data))
    return head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data))


def read_exact(sock, n):
    out = b""
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise EOFError("connection closed")
        out += chunk
    return out


def read_text(sock):
    b0, b1 = read_exact(sock, 2)
    n = b1 & 0x7F
    if n == 126:
        n = struct.unpack(">H", read_exact(sock, 2))[0]
    return read_exact(sock, n).decode()


def connect(password):
    sock = socket.create_connection(("127.0.0.1", PORT), timeout=10)
    auth = base64.b64encode(("CP1:" + password).encode()).decode()
    sock.sendall((
        "GET /ocpp/CP1 HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: ocpp1.6\r\nAuthorization: Basic " + auth + "\r\n\r\n").encode())
    reply = b""
    while b"\r\n\r\n" not in reply:
        chunk = sock.recv(1024)
        if not chunk:
            break
        reply += chunk
    return sock, reply.decode(errors="replace")


def main():
    timeline = os.path.join(HERE, "ocpp_timeline.tmp")
    with open(timeline, "w") as f:
        f.write(TIMELINE)
    runner = subprocess.Popen(
        [RUNNER, SCRIPT, "--ws-listen", str(PORT), "--tick-every", "0.5", "--ticks", "12",
         "--secret", "ws_password=demo-pass", "--timeline", timeline],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        for _ in range(50):
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
                break
            except OSError:
                time.sleep(0.1)
        bad, reply = connect("wrong")
        bad.close()
        assert reply.startswith("HTTP/1.1 401"), "a wrong password must be refused: " + reply
        sock, reply = connect("demo-pass")
        assert reply.startswith("HTTP/1.1 101") and "ocpp1.6" in reply, reply
        sock.sendall(frame('[2,"b1","BootNotification",{"chargePointModel":"M","chargePointVendor":"V"}]'))
        boot = json.loads(read_text(sock))
        assert boot[0] == 3 and boot[1] == "b1" and boot[2]["status"] == "Accepted", boot
        sock.sendall(frame('[2,"s1","StartTransaction",{"connectorId":1,"idTag":"T","meterStart":0,"timestamp":"x"}]'))
        start = json.loads(read_text(sock))
        assert start[2]["idTagInfo"]["status"] == "Accepted" and isinstance(start[2]["transactionId"], int), start
        limits = []
        while len(limits) < len(EXPECTED):
            msg = json.loads(read_text(sock))
            if msg[0] == 2 and msg[2] == "SetChargingProfile":
                schedule = msg[3]["csChargingProfiles"]["chargingSchedule"]
                assert schedule["chargingRateUnit"] == "A", schedule
                limits.append(schedule["chargingSchedulePeriod"][0]["limit"])
                sock.sendall(frame(json.dumps([3, msg[1], {"status": "Accepted"}])))
        assert limits == EXPECTED, "limits %s, expected %s" % (limits, EXPECTED)
        print("ok    ocpp station: boot, transaction, limits %s" % limits)
        sock.close()
        return 0
    except Exception as e:  # noqa: BLE001 -- report anything, with the runner's log
        runner.kill()
        print("FAIL  ocpp station: %s\n%s" % (e, runner.communicate()[0]))
        return 1
    finally:
        if runner.poll() is None:
            runner.kill()
        os.remove(timeline)


if __name__ == "__main__":
    sys.exit(main())
