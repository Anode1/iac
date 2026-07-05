#!/bin/sh
# bench.sh - reproducible send->recv wake latency for iac.
#
# Measures the TRANSPORT's contribution to a message round-trip: the time from
# a sender's append to a parked recv returning. This is not the agent's think
# time (a model turn is seconds); it isolates what iac itself costs.
#
# Linux only: the sub-millisecond path is an inotify wake, and this script uses
# `date +%s.%N` (GNU date). On macOS recv falls back to a ~100ms poll and %N is
# unavailable; run this on Linux. Token cost is MODELED, not measured - see
# doc/dev/RECEIVE_MODEL.md and doc/dev/BENCHMARKS.md.
#
# Usage: scripts/bench.sh [iterations]   (default 7)

set -eu

here=$(dirname "$0")
iac="$here/../iac"
n=${1:-7}

[ -x "$iac" ] || { echo "build first: make (no $iac)" >&2; exit 1; }
case "$(date +%N)" in
    *N*|'') echo "need GNU date with %N (run on Linux)" >&2; exit 1 ;;
esac

# One warm-log round-trip: log already exists, so the inotify watch attaches.
# Returns the send->wake gap in milliseconds on stdout.
one_warm() {
    r=$(mktemp -d)
    "$iac" join "$r" bob >/dev/null 2>&1
    echo warmup | "$iac" send "$r" alice >/dev/null 2>&1     # creates the log (stdin body; no "-" arg)
    ( "$iac" recv "$r" bob 10 >/dev/null 2>&1; date +%s.%N >"$r/.woke" ) &
    rp=$!
    sleep 0.3                                                # let bob block on inotify
    ts=$(date +%s.%N)
    echo ping | "$iac" send "$r" bob >/dev/null 2>&1
    wait "$rp"
    t1=$(cat "$r/.woke")
    rm -rf "$r"
    awk -v a="$ts" -v b="$t1" 'BEGIN{ printf "%.3f\n", (b-a)*1000 }'
}

# One cold-room round-trip: no log yet, so the first message rides the poll tick.
one_cold() {
    r=$(mktemp -d)
    "$iac" join "$r" bob >/dev/null 2>&1
    ( "$iac" recv "$r" bob 10 >/dev/null 2>&1; date +%s.%N >"$r/.woke" ) &
    rp=$!
    sleep 0.3
    ts=$(date +%s.%N)
    echo ping | "$iac" send "$r" bob >/dev/null 2>&1
    wait "$rp"
    t1=$(cat "$r/.woke")
    rm -rf "$r"
    awk -v a="$ts" -v b="$t1" 'BEGIN{ printf "%.3f\n", (b-a)*1000 }'
}

summary() {   # reads ms values on stdin, prints min / median / max
    sort -n | awk '
        { v[NR]=$1 }
        END{
            m = (NR%2) ? v[(NR+1)/2] : (v[NR/2]+v[NR/2+1])/2
            printf "  n=%d   min %.2f ms   median %.2f ms   max %.2f ms\n", NR, v[1], m, v[NR]
        }'
}

echo "iac send->recv wake latency ($n iterations each)"
echo
echo "warm log (inotify path - a live board):"
i=0; while [ "$i" -lt "$n" ]; do one_warm; i=$((i+1)); done | summary
echo
echo "cold room (poll fallback - first message into a new room):"
i=0; while [ "$i" -lt "$n" ]; do one_cold; i=$((i+1)); done | summary
echo
echo "The wake is a fraction of this; two process spin-ups dominate."
echo "An agent's own turn is seconds - thousands of times larger. The transport is never the bottleneck."
