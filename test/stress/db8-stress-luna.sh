#!/bin/sh
# Copyright (c) 2026 LG Electronics, Inc. and webOS-ports project
# SPDX-License-Identifier: Apache-2.0
#
# db8-stress-luna.sh: on-device stress harness for the running db8 service.
# Drives com.palm.db over the luna bus with concurrent put/find/del batches
# while sampling the daemon's memory, then verifies that every object this
# script created was deleted again and that memory did not grow unbounded.
#
# Usage: db8-stress-luna.sh [ITERATIONS] [JOBS]
#   ITERATIONS  batches per job    (default 50)
#   JOBS        concurrent jobs    (default 4)

ITERATIONS=${1:-50}
JOBS=${2:-4}
SERVICE=com.palm.db
KIND="com.webos.db8stress:1"
LUNA_SEND=$(command -v luna-send || echo /usr/bin/luna-send)
TMPDIR=$(mktemp -d /tmp/db8-stress-luna.XXXXXX) || exit 2
trap 'rm -rf "$TMPDIR"' EXIT
FAIL=0

call() {
    # call <payload-json> ; echoes response, returns non-zero unless returnValue:true
    resp=$("$LUNA_SEND" -n 1 -a com.webos.db8stress "luna://$SERVICE/$1" "$2" 2>&1)
    echo "$resp"
    case "$resp" in
        *'"returnValue":true'*) return 0 ;;
        *) return 1 ;;
    esac
}

db8_pid() {
    pidof db8 || pidof mojodb-luna
}

rss_kb() {
    pid=$(db8_pid | awk '{print $1}')
    [ -n "$pid" ] && awk '/VmRSS/{print $2}' "/proc/$pid/status"
}

echo "[db8-stress-luna] registering kind $KIND"
call putKind "{\"id\":\"$KIND\",\"owner\":\"com.webos.db8stress\",\"indexes\":[{\"name\":\"batch\",\"props\":[{\"name\":\"batch\"}]}]}" > "$TMPDIR/putkind.out" || {
    echo "[db8-stress-luna] putKind failed:"; cat "$TMPDIR/putkind.out"; exit 1;
}

RSS_START=$(rss_kb)
echo "[db8-stress-luna] start RSS: ${RSS_START:-unknown} kB; running $JOBS jobs x $ITERATIONS iterations"

runjob() {
    job=$1
    i=0
    while [ "$i" -lt "$ITERATIONS" ]; do
        batch="j${job}b${i}"
        objs="{\"_kind\":\"$KIND\",\"batch\":\"$batch\",\"n\":1},{\"_kind\":\"$KIND\",\"batch\":\"$batch\",\"n\":2},{\"_kind\":\"$KIND\",\"batch\":\"$batch\",\"n\":3}"
        call put "{\"objects\":[$objs]}" > "$TMPDIR/put.$job" || { echo "put failed (job $job iter $i)"; cat "$TMPDIR/put.$job"; return 1; }
        call find "{\"query\":{\"from\":\"$KIND\",\"where\":[{\"prop\":\"batch\",\"op\":\"=\",\"val\":\"$batch\"}]}}" > "$TMPDIR/find.$job" || { echo "find failed (job $job iter $i)"; return 1; }
        # every batch we just put must be findable
        case "$(cat "$TMPDIR/find.$job")" in
            *"$batch"*) : ;;
            *) echo "find returned no results for $batch"; return 1 ;;
        esac
        call del "{\"query\":{\"from\":\"$KIND\",\"where\":[{\"prop\":\"batch\",\"op\":\"=\",\"val\":\"$batch\"}]}}" > "$TMPDIR/del.$job" || { echo "del failed (job $job iter $i)"; return 1; }
        i=$((i + 1))
    done
    return 0
}

j=0
while [ "$j" -lt "$JOBS" ]; do
    ( runjob "$j" ) &
    eval "PID_$j=$!"
    j=$((j + 1))
done

j=0
while [ "$j" -lt "$JOBS" ]; do
    eval "wait \$PID_$j" || FAIL=1
    j=$((j + 1))
done

# all our objects should be gone now
LEFT=$(call find "{\"query\":{\"from\":\"$KIND\"},\"count\":true}")
case "$LEFT" in
    *'"count":0'*) : ;;
    *) echo "[db8-stress-luna] LEFTOVER OBJECTS: $LEFT"; FAIL=1 ;;
esac

call delKind "{\"id\":\"$KIND\"}" > /dev/null 2>&1

RSS_END=$(rss_kb)
echo "[db8-stress-luna] end RSS: ${RSS_END:-unknown} kB (start ${RSS_START:-unknown} kB)"
if [ -n "$RSS_START" ] && [ -n "$RSS_END" ]; then
    GROWTH=$((RSS_END - RSS_START))
    echo "[db8-stress-luna] RSS growth: ${GROWTH} kB"
    # flag runaway growth (> 50 MB) as failure
    [ "$GROWTH" -gt 51200 ] && { echo "[db8-stress-luna] EXCESSIVE MEMORY GROWTH"; FAIL=1; }
fi

if [ "$FAIL" -eq 0 ]; then
    echo "[db8-stress-luna] PASSED"
else
    echo "[db8-stress-luna] FAILED"
fi
exit "$FAIL"
