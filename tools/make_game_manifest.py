#!/usr/bin/env python3
"""Generate a content-free regional manifest from a legally owned, decrypted v208 game.

Usage: python3 tools/make_game_manifest.py /path/to/game
US/EU owners can submit the resulting JSON in ios/assets/game-manifest/ for review.
The manifest contains only relative paths, sizes and SHA-256 hashes; never include game files.
"""
import hashlib
import json
from pathlib import Path
import sys
import xml.etree.ElementTree as ET


def main():
    root = Path(sys.argv[1]).resolve()
    meta = ET.parse(root / 'meta/meta.xml').getroot()
    title = meta.findtext('title_id', '').strip().upper()
    if title not in ('00050000101C9300', '00050000101C9400', '00050000101C9500'):
        raise SystemExit('Not a supported Wii U Breath of the Wild title')
    if meta.findtext('title_version', '').strip() != '208':
        raise SystemExit('Update to v208 first')
    files = []
    for folder in ('code', 'content', 'meta'):
        for path in sorted((root / folder).rglob('*')):
            rel = path.relative_to(root)
            if path.is_symlink():
                raise SystemExit(f'Symlink rejected: {rel}')
            if not path.is_file() or any(p.startswith('.') or p in ('__MACOSX', 'aoc') for p in rel.parts):
                continue
            if any(p in ('', '.', '..') or '\\' in p or ':' in p for p in rel.parts):
                raise SystemExit(f'Unsafe path: {rel}')
            with path.open('rb') as stream:
                digest = hashlib.file_digest(stream, 'sha256').hexdigest()
            files.append(dict(path=rel.as_posix(), size=path.stat().st_size, sha256=digest))
    rpx = next(f['sha256'] for f in files if f['path'] == 'code/U-King.rpx')
    if rpx != 'ba58da5b95ce929e005d058ceb08b9b2788d1ab2bbc8a6c189bbadca0bb34d30':
        raise SystemExit('U-King.rpx is not the supported v208 executable')
    output = Path(__file__).resolve().parents[1] / 'ios/assets/game-manifest' / f'{title}.json'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(dict(titleID=title, files=files), ensure_ascii=False, separators=(',', ':')) + '\n')
    print(f'{title}: {len(files)} files, {sum(f["size"] for f in files)} bytes → {output}')


if __name__ == '__main__':
    main()
