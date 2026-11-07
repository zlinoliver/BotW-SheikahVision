#!/bin/zsh
# usage: tools/test_save.sh <out dir> <seconds> [runtime flags...]
# A test run that starts from a save instead of a new game: <out dir> becomes the user folder (BOTW_USER_ROOT), with
# a copy of the save (BOTW_TEST_SAVE, default: the fully unlocked save of the Cemu bundle next to the repo) as the
# player's save; the original is never touched. A presses at the title and the menu continue the latest slot.
# BOTW_VR=1 etc. pass through. Captures (--capture N) land in <out dir>/captures, the log in <out dir>/runtime.log.
set -eu
cd "$(dirname "$0")/.."
out=$1 seconds=$2
shift 2
save=${BOTW_TEST_SAVE:?set BOTW_TEST_SAVE to a folder with your own save (option.sav and slot folders)}
[ -f "$save/option.sav" ] || { echo "no save at $save (set BOTW_TEST_SAVE)" >&2; exit 1; }
rm -rf "$out"
mkdir -p "$out/save/2147483649"
cp -R "$save/." "$out/save/2147483649/"
# title -> "Continue" -> load: A every 2 s for the first 40 s (presses during loading are ignored)
script=""
for f in $(seq 300 120 2400); do script="$script$f:key:a,"; done
BOTW_USER_ROOT="$out" exec ./build/SheikahVision --game "${BOTW_GAME:-../game}" --seconds "$seconds" --mute \
  --script "${script%,}" "$@"
