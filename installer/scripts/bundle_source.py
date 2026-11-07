"""Copy only tracked public build inputs into a local installer; fail closed on game/secret files."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def bundle(root, dest):
    root, dest = Path(root), Path(dest)
    names = subprocess.check_output(['git', '-C', str(root), 'ls-files', '-z', 'CMakeLists.txt', 'runtime', 'ios',
                                     'cafe', 'tools', 'pyproject.toml', 'uv.lock', 'LICENSE', 'installer/engine',
                                     'installer/cmake', 'installer/scripts', 'installer/README.md', 'VERSION', 'installer/THIRD_PARTY_NOTICES.md', 'installer/Glovebox-NOTICE.md']).split(b'\0')
    banned = {'.rpx', '.rpl', '.wud', '.wux', '.key', '.tik', '.tmd', '.h3', '.sarc', '.pack', '.sbfres', '.bfres', '.sav'}
    for raw in names:
        if not raw:
            continue
        p = Path(os.fsdecode(raw))
        # the optional experience save is the author's own completed save, published on purpose (2026-11-17)
        experience = p.parts[:3] == ('ios', 'assets', 'experience-save')
        if (p.suffix.lower() in banned and not experience) or p.name in {'keys.txt', 'local.env'} or 'generated' in p.parts:
            raise ValueError('refusing to bundle a game or secret file: ' + str(p))
        if (root / p).is_symlink():
            raise ValueError('refusing to bundle a symlink: ' + str(p))
    if dest.exists():
        shutil.rmtree(dest)
    h = hashlib.sha256()
    for raw in names:
        if not raw:
            continue
        rel = os.fsdecode(raw)
        src = root / rel
        if not src.is_file():
            raise ValueError('missing tracked source: ' + rel)
        data = src.read_bytes()
        h.update(raw + b'\0' + data)
        target = dest / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, target)
    (dest / 'source-version').write_text(h.hexdigest())
    # the commit, for the app's build line (the bundled copy has no .git); release_installer.sh requires a clean tree
    commit = subprocess.run(['git', '-C', str(root), 'rev-parse', '--short=8', 'HEAD'], capture_output=True, text=True).stdout.strip()
    (dest / 'source-commit').write_text(commit or 'unknown')
    (dest / 'source-files.json').write_text(json.dumps([os.fsdecode(n) for n in names if n]))
    print('Public source copied; no generated sources, game files or keys.')


if __name__ == '__main__':
    bundle(*sys.argv[1:])
