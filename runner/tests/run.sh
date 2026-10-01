#!/bin/sh
# Builds the runner, runs every example script against its canned answers,
# and proves the unit's limits still hold in the copied VM.
set -u
cd "$(dirname "$0")/.."
make -s || exit 1
fail=0

expect() { # name, expected exit code, text the output must contain, command...
    name=$1; code=$2; text=$3; shift 3
    out=$("$@" 2>&1); got=$?
    if [ "$got" -eq "$code" ] && printf '%s' "$out" | grep -qF -- "$text"; then
        echo "ok    $name"
    else
        echo "FAIL  $name (exit $got, wanted $code and \"$text\")"; printf '%s\n' "$out"; fail=1
    fi
}

for s in ../scripts/*.be; do
    n=$(basename "$s" .be)
    [ "$n" = ocpp ] && continue # no HTTP; it is tested against a pretend station below
    expect "$n runs clean" 0 "report: active" \
        ./cxl-run "$s" --ticks 3 --responses "../scripts/replay/$n.txt" --secrets "../scripts/replay/$n.secrets"
done
expect "smartcar stops the car on low headroom" 0 "stopped: low headroom" \
    ./cxl-run ../scripts/smartcar.be --set allowed_amps=3 \
    --responses ../scripts/replay/smartcar.txt --secrets ../scripts/replay/smartcar.secrets
expect "a looping tick is aborted" 1 "timeout_error" ./cxl-run tests/loop.be
expect "a memory hog runs out of arena" 1 "out of memory" ./cxl-run tests/hog.be
expect "there is no os module" 1 "module 'os' not found" ./cxl-run tests/no_os.be
expect "ocpp waits for a station" 0 "no OCPP station connected" ./cxl-run ../scripts/ocpp.be --ticks 2
python3 tests/ocpp_station.py || fail=1
exit $fail
