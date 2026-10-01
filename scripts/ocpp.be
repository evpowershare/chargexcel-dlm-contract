# ChargeXcel on-device script: DLM for an OCPP 1.6J charging station.
# Point the station at ws://<unit>/ocpp/ (it appends its id), Basic auth,
# password = the secret "ws_password" (no secret, no connection). It keeps
# one TxDefaultProfile (amps, connector 0) on the station:
#   load rises        -> lower at once, 1 A under the line
#   room steady 2 min -> raise, to the smallest value seen meanwhile
#   room under 6 A    -> 0 A (pause); resume after 2 min of >= 8 A
#   net-zero cut      -> lower after 1 min, raise after 2, max 6 changes/h
#   every 15 min and on (re)connect -> send it again
# If the station ignores it, ChargeXcel's relay still protects.
interval = 30
import json

var mx = 48       # the station's own maximum amps; lower it to suit yours
var peer = nil    # id of the connected station
var tk = 0        # tick clock (30 s each)
var mid = 0       # our last OCPP message id
var pend = nil    # id of our unanswered SetChargingProfile
var pa = 0        # amps in that request
var pt = 0        # tick it was sent
var sa = -1       # amps the station accepted; 0 = paused; -1 = send now
var st = 0        # tick of the last accepted profile (for the 15 min re-send)
var bo = 0        # back off until this tick after a rejection
var ust = 0       # tick since which a raise has been waiting
var wmin = 99     # smallest target seen while waiting to raise
var lh = 0        # tick since which a solar lowering has been waiting
var okt = 0       # tick since which headroom has allowed a resume
var sol = []      # ticks of solar-following changes in the last hour
var wsol = false  # the pending request follows the sun (counts toward 6/h)
var wu = false    # the station refused amps: send watts (amps x line volts)

# Ask the station to hold `a` amps (0 = pause). One request in flight.
def setp(a)
  mid += 1
  pend = str(mid)
  pa = a
  pt = tk
  # Built as text: a map literal costs arena memory per key.
  if !dlm.ws_send(peer, '[2,"' + pend + '","SetChargingProfile",{"connectorId":0,"csChargingProfiles":'
      '{"chargingProfileId":1,"stackLevel":0,"chargingProfilePurpose":"TxDefaultProfile",'
      '"chargingProfileKind":"Relative","chargingSchedule":{"chargingRateUnit":"' + (wu ? "W" : "A") +
      '","chargingSchedulePeriod":[{"startPeriod":0,"limit":' +
      str(wu ? a * (dlm.telemetry()['topology'] == 2 ? 208 : 240) : a) + '}]}}}]')
    pend = nil
  end
end

# Room for one more solar-following change this hour?
def solok()
  while size(sol) > 0 && tk - sol[0] >= 120
    sol.remove(0)
  end
  return size(sol) < 6
end

def on_ws(p, k, x)
  if k == "open"
    peer = p
    pend = nil
    sa = -1
    wu = false
    dlm.log(p + " connected")
    return
  end
  if k == "close"
    if p == peer
      peer = nil
      pend = nil
      dlm.log(p + " disconnected")
    end
    return
  end
  var m = json.load(x)
  if !isinstance(m, list) || size(m) < 3
    return
  end
  if m[0] == 2                       # a request from the station
    var a = m[2]
    var r = nil
    var ok = '{"idTagInfo":{"status":"Accepted"}}'
    if a == "BootNotification"
      r = '{"status":"Accepted","interval":60,"currentTime":"' + dlm.telemetry()['time_utc'] + '"}'
      sa = -1                        # a rebooted station forgets its profile
    elif a == "Heartbeat"
      r = '{"currentTime":"' + dlm.telemetry()['time_utc'] + '"}'
    elif a == "Authorize"
      r = ok
    elif a == "StartTransaction"
      mid += 1                       # any unique number will do
      r = '{"transactionId":' + str(mid) + ',' + ok[1..]
    elif a == "StatusNotification" || a == "MeterValues" || a == "StopTransaction"
      r = '{}'
    end
    dlm.ws_send(p, r == nil ? '[4,' + json.dump(m[1]) + ',"NotImplemented","",{}]'
                            : '[3,' + json.dump(m[1]) + ',' + r + ']')
  elif (m[0] == 3 || m[0] == 4) && m[1] == pend   # the answer to our profile
    var ok = m[0] == 3 && isinstance(m[2], map) && m[2].find("status") == "Accepted"
    if ok
      if wsol && sa >= 0 && pa != sa
        sol.push(tk)
      end
      sa = pa
      st = tk
      pend = nil
    elif !wu                         # some stations take watts only
      wu = true
      setp(pa)
    else
      dlm.log(peer + " refused the profile")
      wu = false
      bo = tk + 10                   # 5 minutes
      sa = -1
      pend = nil
    end
  end
end

def tick()
  tk += 1
  if peer == nil
    dlm.report(false, "no OCPP station connected")
    return
  end
  var t = dlm.telemetry()
  var al = t['allowed_amps']
  var sf = t['safety_allowed_amps']
  var a = t['evse_branch_amps']
  if !t['relay_closed']              # ChargeXcel shed; nothing for us to do
    dlm.report(true, peer + " relay open")
    return
  end
  if !t['allowed_amps_valid']        # the relay protects
    dlm.report(true, peer + " no headroom figure")
    return
  end
  if pend != nil
    if tk - pt < 2
      dlm.report(true, peer + " sending " + str(pa) + "A")
      return
    end
    pend = nil                       # no answer in a minute: try again
    sa = -1
  end
  if tk < bo
    dlm.report(true, peer + " refused; retry " + str((bo - tk + 1) / 2) + "m")
    return
  end
  if t['netzero'] == "resolving" && a <= sf + 0.5
    dlm.report(true, peer + " reading the sun")
    return
  end

  var cc = t['continuous_capacity_amps']
  var s = al < sf - 0.5              # the sun, not the service, is the cut
  var tg = al >= cc - 0.01 ? int(cc) : (s ? int(al + 0.5) : int(al - 1))
  if tg > mx tg = mx end
  if tg < 0 tg = 0 end
  var bd = s ? 1 : 2
  wsol = s

  if sa == 0                         # paused
    if tg < (s ? 6 : 8)
      okt = 0
      dlm.report(true, peer + " paused: low headroom")
      return
    end
    if okt == 0 okt = tk end
    if tk - okt < 4 || (s && !solok())
      dlm.report(true, peer + " resume in " + str((5 - (tk - okt)) / 2) + "m")
      return
    end
    okt = 0
    setp(tg)
  elif tg < 6
    if sa != 0 && (!s || solok() || sa < 0)
      setp(0)
    end
  elif sa < 0 || a > sf + 0.5 || (!s && tg <= sa - bd)
    if tg != sa setp(tg) end         # a safety cut goes at once
    ust = tk
    wmin = 99
    lh = 0
  elif s && tg <= sa - bd            # the sun dimmed: lower after 1 min
    if lh == 0 lh = tk end
    if tk - lh >= 2 && solok()
      setp(tg)
      lh = 0
    end
    ust = tk
    wmin = 99
  elif tg >= sa + bd                 # room to raise: wait 2 min
    if tg < wmin wmin = tg end
    if tk - ust >= 4 && (!s || solok())
      setp(wmin)
      ust = tk
      wmin = 99
    end
    lh = 0
  else
    ust = tk
    wmin = 99
    lh = 0
    if tk - st >= 30 setp(sa) end    # re-send every 15 min
  end
  if pend != nil
    dlm.report(true, peer + " sending " + str(pa) + "A")
  else
    dlm.report(true, peer + (sa == 0 ? " paused <6A" : " " + str(sa) + "A"))
  end
end
