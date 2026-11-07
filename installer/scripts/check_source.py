"""Release prebuilds must use a clean source checkout and an isolated runtime-only cache."""
from pathlib import Path
import subprocess
import sys

root, build = map(Path, sys.argv[1:])
status = subprocess.check_output(['git','-C',str(root),'status','--porcelain','--untracked-files=all','--',
                                 'CMakeLists.txt','runtime','ios','cafe','tools','installer','.gitignore','pyproject.toml','uv.lock'], text=True)
if status:
    raise SystemExit('Commit or remove changed/untracked source before prebuild:\n' + status)
cache = build/'CMakeCache.txt'
if cache.exists() and 'BOTW_RUNTIME_ONLY:BOOL=ON' not in cache.read_text():
    raise SystemExit('Use a separate runtime-only build directory; refusing to change an ordinary build cache.')
