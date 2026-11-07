#!/usr/bin/env python3
"""Summarise a headset run's runtime.log for the long, cold-start comparisons.

usage: tools/vr_run_report.py <runtime.log> [more logs...] [--slice SECONDS]

For each slice of the run: frame rate, GPU time and share, draws, the wait between the eye passes, how many refreshes each
picture showed, picture latency, the view pass's time (the same work every frame, a proxy for the GPU's clock) and the
headset loop. Above them, when the thermal state changed and when the loop first fell below 60 Hz.
"""
import argparse
import re
from pathlib import Path

NUM = r"([\d.]+)"
TIME = rf"^\[\s*{NUM}\]"
GPU = re.compile(rf"{TIME} \[gpu\] {NUM} fps, (\d+) draws/frame .*?gpu {NUM} ms/frame \({NUM}% busy\)(?:.*?GX2WaitTimeStamp (\d+))?")
TIMING = re.compile(
    rf"{TIME} vr timing: late (\d+)/(\d+), gpu queue avg {NUM} ms max {NUM} ms, gpu run avg {NUM} ms, "
    rf"picture latency {NUM} frames \({NUM} ms\).*?shown 1x (\d+) 2x (\d+) 3x (\d+) 4\+x (\d+)")
LOOP = re.compile(rf"{TIME} vr loop: {NUM} Hz, thermal (\w+)")
LOAD = re.compile(rf"{TIME} vr load: view pass {NUM} ms each, {NUM} ms of GPU a second")
FPS = re.compile(rf"{TIME} fps: game {NUM} frames/s.*?over 30 ms (\d+)")
SKIP_BEFORE = 20.0  # the title screen and loading


def mean(values):
    return sum(values) / len(values) if values else float("nan")


def read(path):
    runs = {"gpu": [], "timing": [], "loop": [], "load": [], "over": []}
    for line in Path(path).read_text(errors="ignore").splitlines():
        if m := GPU.match(line):
            runs["gpu"].append((float(m[1]), float(m[2]), int(m[3]), float(m[4]), float(m[5]), int(m[6] or 0)))
        elif m := TIMING.match(line):
            runs["timing"].append((float(m[1]), int(m[2]), float(m[8]), *[int(m[i]) for i in (9, 10, 11, 12)], float(m[6])))
        elif m := LOOP.match(line):
            runs["loop"].append((float(m[1]), float(m[2]), m[3]))
        elif m := LOAD.match(line):
            runs["load"].append((float(m[1]), float(m[2]), float(m[3])))
        elif m := FPS.match(line):
            runs["over"].append((float(m[1]), int(m[3])))
    return runs


def report(path, width):
    r = read(path)
    print(f"== {path}")
    if not r["gpu"]:
        print("no [gpu] lines")
        return
    end = max(x[0] for x in r["gpu"])
    states, last = [], None
    for t, _, state in r["loop"]:
        if state != last:
            states.append(f"{t:.0f}s {state}")
            last = state
    print("thermal:", ", ".join(states) or "no data")
    slow = [t for t, hz, _ in r["loop"] if hz < 60]
    print(f"loop: first below 60 Hz at {slow[0]:.0f}s" if slow else "loop: never below 60 Hz", f"(run {end:.0f}s)")
    print(" slice      fps   min  gpu ms busy draws wait ms/s  over30 | 2x%  latency  late | view ms  90Hz thermal")
    start = SKIP_BEFORE
    while start < end:
        stop = start + width
        inside = lambda rows: [x for x in rows if start <= x[0] < stop]
        gpu = [x for x in inside(r["gpu"]) if x[2] >= 1000]  # not the loading screens
        timing, loop, load = inside(r["timing"]), inside(r["loop"]), inside(r["load"])
        if gpu:
            shown = [sum(x[i] for x in timing) for i in (3, 4, 5, 6)]
            two = shown[1] / max(sum(shown), 1) * 100
            view = mean([x[1] for x in load]) if load else mean([x[7] for x in timing])
            fast = sum(1 for x in loop if x[1] > 80)
            hot = max((x[2] for x in loop), key=["nominal", "fair", "serious", "critical"].index, default="?")
            print(f"{start:4.0f}-{stop:<4.0f} {mean([x[1] for x in gpu]):5.1f} {min(x[1] for x in gpu):5.1f} "
                  f"{mean([x[3] for x in gpu]):7.1f} {mean([x[4] for x in gpu]):4.0f} {mean([x[2] for x in gpu]):5.0f} "
                  f"{mean([x[5] for x in gpu]):9.0f} {sum(x[1] for x in inside(r['over'])):7d} | {two:3.0f} "
                  f"{mean([x[2] for x in timing]):6.0f}ms {sum(x[1] for x in timing):5d} | {view:7.2f} {fast}/{len(loop)} {hot}")
        start = stop


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("logs", nargs="+")
    parser.add_argument("--slice", type=float, default=60.0)
    args = parser.parse_args()
    for path in args.logs:
        report(path, args.slice)


if __name__ == "__main__":
    main()
