#!/bin/zsh
# usage: tools/vr_bench.sh <label> [runs] [runtime flags...]
# Repeatable VR performance numbers: loads the test save (tools/test_save.sh), plays 100 s with BOTW_VR=1 at 720p,
# and averages the [gpu] log lines of the last 30 s (the player stays where the save puts them). Prints one line per
# run: fps, draws per frame, and the main thread's wait on the GPU between the two eye passes (GX2WaitTimeStamp,
# ms per second). BOTW_VR=0 measures the same place without VR.
set -eu
cd "$(dirname "$0")/.."
label=$1 runs=${2:-2}
shift $(( $# >= 2 ? 2 : 1 ))
root=${TMPDIR:-/tmp}/botw-bench
for k in $(seq 1 $runs); do
  out="$root/$label-$k"
  BOTW_VR=${BOTW_VR:-1} tools/test_save.sh "$out" 100 --scale 1 "$@" > /dev/null 2>&1 || true
  awk '/\[gpu\]/ { t = substr($2, 1, length($2) - 1) + 0; if (t >= 70) {
         n++; fps += $4; draws += $6
         if (match($0, /GX2WaitTimeStamp [0-9]+/)) wait += substr($0, RSTART + 17, RLENGTH - 17)
       } }
       END { if (n) printf "%s run %d: %.1f fps, %.0f draws/frame, inter-pass wait %.0f ms/s (%d samples)\n", \
                    lbl, k, fps / n, draws / n, wait / n, n
             else printf "%s run %d: no samples\n", lbl, k }' lbl="$label" k="$k" "$out/runtime.log"
done
