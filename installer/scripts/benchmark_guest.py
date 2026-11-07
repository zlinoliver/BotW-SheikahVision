#!/usr/bin/env python3
"""Serial cold default/O1 builds. Compiler ownership follows parent PIDs, not job process groups."""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import statistics
import subprocess
import threading
import time


def descendants(rows, root):
    owned = {root} if root else set()
    while True:
        more = {pid for pid, parent, _ in rows if parent in owned}
        if more <= owned:
            return owned
        owned |= more


def process_rows():
    data = subprocess.check_output(['ps', '-A', '-o', 'pid=,ppid=,comm='], text=True)
    return [(int(p), int(parent), name) for line in data.splitlines() if line.strip()
            for p, parent, name in [line.strip().split(None, 2)]]


def external_compilers(rows, root=None):
    owned = descendants(rows, root)
    names = {'clang', 'clang++', 'clang-21', 'swift-frontend', 'swiftc', 'ninja', 'cmake', 'metal'}
    return [f'{pid} {name}' for pid, _, name in rows if pid not in owned and Path(name).name in names]


def observe_compilers(root=None):
    try:
        return external_compilers(process_rows(), root)
    except (OSError, subprocess.SubprocessError):
        return ['process inventory unavailable']


def stop_build(child):
    try:
        owned = descendants(process_rows(), child.pid)
    except (OSError, subprocess.SubprocessError):
        owned = {child.pid}
    for pid in owned:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    try:
        child.wait(timeout=5)
    except subprocess.TimeoutExpired:
        for pid in owned:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        child.wait()


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--generated', required=True, help='private, locally generated sources')
    p.add_argument('--output', default='build-installer/benchmark-review')
    a = p.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = Path(a.output).resolve(); out.mkdir(parents=True, exist_ok=True)
    results = []
    for trial in range(3):
        for opt in ('default', 'O1'):
            if observe_compilers():
                raise SystemExit('Other compilers or unavailable process inventory: 未能干净测量.')
            build = out / opt
            subprocess.run(['cmake', '-S', str(root/'ios'), '-B', str(build), '-G', 'Ninja',
                            '-DCMAKE_SYSTEM_NAME=visionOS', '-DCMAKE_OSX_SYSROOT=xros',
                            '-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0',
                            '-DBOTW_GENERATED_DIR='+str(Path(a.generated).resolve()),
                            '-DBOTW_GUEST_OPT='+('' if opt=='default' else opt)], check=True)
            shutil.rmtree(build/'CMakeFiles/guest.dir', ignore_errors=True)
            dirty = []; stop = threading.Event()
            with (out/f'{trial+1}-{opt}.log').open('w') as f:
                start = time.monotonic()
                child = subprocess.Popen(['cmake', '--build', str(build), '--target', 'guest', '-j', str(os.cpu_count())],
                                         stdout=f, stderr=subprocess.STDOUT, start_new_session=True)
                def observe():
                    while not stop.wait(1):
                        dirty.extend(observe_compilers(child.pid))
                monitor = threading.Thread(target=observe); monitor.start()
                try:
                    code = child.wait()
                except BaseException:
                    stop_build(child)
                    raise
                finally:
                    stop.set(); monitor.join()
                elapsed = time.monotonic()-start
            results.append(dict(trial=trial+1, optimization=opt, seconds=elapsed, exit=code,
                                other_compilers=sorted(set(dirty))))
            (out/'results.json').write_text(json.dumps(results, indent=2))
            if code:
                raise SystemExit(code)
            if dirty:
                raise SystemExit('External compilers observed: 未能干净测量. Raw results saved; no comparison.')
    for opt in ('default', 'O1'):
        values = [v['seconds'] for v in results if v['optimization']==opt]
        print(opt, 'median', statistics.median(values), 'range', min(values), max(values))
    print('Also confirm the machine was otherwise idle. No frame-rate conclusion.')


if __name__ == '__main__':
    def interrupted(_signum, _frame):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, interrupted)
    main()
