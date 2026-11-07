"""Public runtime kit metadata. No generated sources or game objects may enter a kit."""
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def compiler_version(output=None):
    import sv
    output = sv.run('xcrun', 'clang++', '--version')[1] if output is None else output
    return next((line.strip() for line in output.splitlines() if line.startswith('Apple clang version')), output.splitlines()[0] if output else '')


def export(build, output):
    import sv
    output = os.path.abspath(output)
    build = os.path.abspath(build)
    if output == build or build.startswith(output + os.sep):
        raise sv.StepError('runtime output must be separate from its build directory')
    if os.path.isdir(output):
        if os.listdir(output) and not os.path.isfile(os.path.join(output, 'runtime.json')):
            raise sv.StepError('refusing to replace an unrelated non-empty output directory')
        shutil.rmtree(output)
    os.makedirs(output)
    names = ['libsheikah_runtime.a', 'libbotw_immersive.a']
    for name in names:
        shutil.copy2(os.path.join(build, name), os.path.join(output, name))
    cache = Path(build, 'CMakeCache.txt').read_text()
    deps_root = next(line.split('=', 1)[1] for line in cache.splitlines() if line.startswith('BOTW_IOS_DEPS:PATH='))
    deps = os.path.join(deps_root, 'prefix-xros')
    shutil.copytree(deps, os.path.join(output, 'prefix-xros'), dirs_exist_ok=True)
    shutil.rmtree(os.path.join(output, 'prefix-xros/lib/pkgconfig'), ignore_errors=True)
    # SDL's documentation example is not a developer path; keep it portable as well.
    header = Path(output, 'prefix-xros/include/SDL3/SDL_filesystem.h')
    if header.exists():
        header.write_text(header.read_text().replace('/Users/bob/', '~/'))
    files = {}
    for folder, _, entries in os.walk(output):
        for name in entries:
            path = os.path.join(folder, name)
            rel = os.path.relpath(path, output)
            if rel != 'runtime.json':
                files[rel] = digest(path)
    manifest = dict(source=sv.source_version(), compiler=compiler_version(),
                    sdk=sv.run('xcrun', '--sdk', 'xros', '--show-sdk-version')[1].strip(),
                    architecture='arm64', minimum_os=next(line.split('=', 1)[1] for line in cache.splitlines() if line.startswith('CMAKE_OSX_DEPLOYMENT_TARGET:')), files=files)
    # no path of this Mac (the kit ships inside the public installer). The bare user name is not checked: the public
    # GitHub owner in the launcher's About page and in the installer's releases link is intended.
    local = {b'/Users/', build.encode(), sv.ROOT.encode(), os.path.expanduser('~').encode()}
    for rel in files:
        with open(os.path.join(output, rel), 'rb') as f:
            data = f.read()
        if any(x in data for x in local):
            raise sv.StepError('runtime kit contains a local path: ' + rel)
    sv.save_json(os.path.join(output, 'runtime.json'), manifest)


def verify(path):
    import sv
    manifest = sv.load_json(os.path.join(path, 'runtime.json'))
    if (manifest.get('source') != sv.source_version() or
        compiler_version(manifest.get('compiler', '')) != compiler_version() or
        manifest.get('sdk') != sv.run('xcrun', '--sdk', 'xros', '--show-sdk-version')[1].strip() or
        manifest.get('architecture') != 'arm64' or manifest.get('minimum_os') != '26.0'):
        raise sv.StepError('the prebuilt runtime is for a different source, Xcode or SDK version; rebuild the runtime kit')
    files = manifest.get('files', {})
    if not all(n in files for n in ('libsheikah_runtime.a', 'libbotw_immersive.a')):
        raise sv.StepError('the runtime kit is incomplete')
    actual = {os.path.relpath(os.path.join(folder, name), path) for folder, _, names in os.walk(path) for name in names}
    if actual != set(files) | {'runtime.json'}:
        raise sv.StepError('the runtime kit has unlisted or missing files')
    for rel, expected in files.items():
        if os.path.isabs(rel) or '..' in rel.split('/') or os.path.islink(os.path.join(path, rel)) or not os.path.realpath(os.path.join(path, rel)).startswith(os.path.realpath(path) + os.sep) or not os.path.isfile(os.path.join(path, rel)):
            raise sv.StepError('the runtime kit has an unsafe or missing file')
        if digest(os.path.join(path, rel)) != expected:
            raise sv.StepError('the runtime kit is damaged; rebuild it')
    return manifest


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == 'export':
        export(sys.argv[2], sys.argv[3])
    else:
        raise SystemExit('usage: runtimekit.py export <runtime-only build> <output>')
