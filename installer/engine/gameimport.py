"""Game-folder validation and import for the SheikahVision installer engine (standard library only).

The user's own Breath of the Wild (Wii U) files are checked and copied into the installer's work directory.
Only already decrypted game folders (code/, content/, meta/) are accepted; nothing here downloads or ships game data.

Validation works on "layers": an ordered list of folders, base first and update last. A file is looked up in the
last layer that has it, which is exactly how an update is merged over the base. A merged single folder is one layer.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

V208_RPX_SHA256 = "ba58da5b95ce929e005d058ceb08b9b2788d1ab2bbc8a6c189bbadca0bb34d30"
TITLE_VERSION = 208

# low 32 bits of the title id -> region
REGION_BY_LOW = {"101C9300": "JPN", "101C9400": "USA", "101C9500": "EUR"}
LOW_BY_REGION = {v: k for k, v in REGION_BY_LOW.items()}
LANGS = {
    "JPN": ["JPja"],
    "USA": ["USen", "USfr", "USes"],
    "EUR": ["EUen", "EUde", "EUfr", "EUes", "EUit", "EUnl", "EUru"],
}
# high 32 bits: what the folder is
KIND_BY_HIGH = {"00050000": "base", "0005000E": "update", "0005000C": "dlc"}

REQUIRED = ["code/U-King.rpx", "code/app.xml", "code/cos.xml", "content", "meta/meta.xml"]
GAME_DIRS = ("code", "content", "meta")
JUNK_NAMES = {".DS_Store", "__MACOSX", ".Spotlight-V100", ".fseventsd", ".Trashes"}


class GameError(Exception):
    """A problem the user can fix. `code` is stable (the wizard maps it to a screen); `message` is for people."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


def is_junk(name):
    return name in JUNK_NAMES or name.startswith("._")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


# ---- reading meta.xml / app.xml ----

def read_xml_fields(path):
    """title_id (16 upper-case hex digits) and title_version (int) from a Wii U meta.xml / app.xml; missing -> None."""
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError):
        return {"title_id": None, "title_version": None}
    out = {"title_id": None, "title_version": None}
    for el in root.iter():
        t = (el.text or "").strip()
        if el.tag == "title_id" and re.fullmatch(r"[0-9A-Fa-f]{16}", t):
            out["title_id"] = t.upper()
        elif el.tag == "title_version":
            try:
                out["title_version"] = int(t, 0)
            except ValueError:
                pass
    return out


def title_info(layer):
    """Fields of a folder: meta.xml first, app.xml when meta has no title id."""
    meta = read_xml_fields(os.path.join(layer, "meta", "meta.xml"))
    app = read_xml_fields(os.path.join(layer, "code", "app.xml"))
    return {"title_id": meta["title_id"] or app["title_id"],
            "title_version": meta["title_version"] if meta["title_version"] is not None else app["title_version"]}


# ---- finding and classifying game folders ----

def is_game_root(d):
    return (os.path.isfile(os.path.join(d, "meta", "meta.xml")) or os.path.isfile(os.path.join(d, "code", "app.xml"))
            or os.path.isfile(os.path.join(d, "code", "U-King.rpx")))


def find_roots(path, max_depth=3):
    """Game roots at or below `path` (a folder holding code/ and/or meta/), at most `max_depth` levels down."""
    path = os.path.abspath(path)
    if is_game_root(path):
        return [path]
    found = []

    def walk(d, depth):
        if depth > max_depth:
            return
        try:
            names = sorted(os.listdir(d))
        except OSError:
            return
        for n in names:
            p = os.path.join(d, n)
            if is_junk(n) or n.startswith(".") or os.path.islink(p) or not os.path.isdir(p):
                continue
            if is_game_root(p):
                found.append(p)
            else:
                walk(p, depth + 1)

    walk(path, 1)
    return found


def has_aoc(d):
    try:
        return any(n.lower().startswith("aoc") and os.path.isdir(os.path.join(d, n)) for n in os.listdir(d))
    except OSError:
        return False


def classify(root):
    info = title_info(root)
    tid = info["title_id"]
    kind = KIND_BY_HIGH.get(tid[:8]) if tid else None
    if kind is None:
        kind = "dlc" if has_aoc(root) else "unknown"
    return {"path": root, "title_id": tid, "title_version": info["title_version"], "kind": kind}


def plan_sources(paths, notices):
    """Turn what the user passed into ordered layers (base first, update last), ignoring DLC.

    Returns (layers, dlc_ignored). Raises GameError when nothing usable or the folders belong to different regions."""
    roots = []
    for p in paths:
        if not os.path.exists(p):
            raise GameError("E_NO_GAME", f"{p} does not exist")
        if not os.path.isdir(p):
            if p.lower().endswith(".wua"):
                raise GameError("E_NOT_FOLDER", f"{os.path.basename(p)} is a Cemu .wua archive, which the installer cannot read; choose the "
                                                "game as a folder with code, content and meta (Cemu has no built-in way to unpack a .wua)")
            if p.lower().endswith((".wud", ".wux")):
                raise GameError("E_NOT_FOLDER", f"{os.path.basename(p)} is a disc image, which is not supported; choose the decrypted "
                                                "game folder that contains code, content and meta")
            raise GameError("E_NOT_FOLDER", f"{p} is not a folder: choose the decrypted game folder that contains code, content and meta")
        found = find_roots(p)
        if not found:
            raise GameError("E_NO_GAME", f"no game folder (code/, content/, meta/) found in {p} or up to 3 levels below it")
        roots.extend(r for r in found if r not in roots)
    infos = [classify(r) for r in roots]
    # A Cemu mlc01/usr/title folder also holds other games: keep Breath of the Wild's titles, mention the rest
    botw = [i for i in infos if i["title_id"] and i["title_id"][8:].upper() in REGION_BY_LOW]
    others = [i for i in infos if i["title_id"] and i not in botw]
    if botw and others:
        notices.append(f"ignored {len(others)} folder(s) of other games (only Breath of the Wild is imported)")
        infos = [i for i in infos if i not in others]
    dlc = [i for i in infos if i["kind"] == "dlc"]
    for i in dlc:
        notices.append(f"DLC found at {i['path']}: DLC is not supported yet, it is ignored (the base game is imported as usual)")
    usable = [i for i in infos if i["kind"] != "dlc"]
    if not usable:
        raise GameError("E_NO_GAME", "only DLC was found; the base game (code/, content/, meta/) is needed")
    # base before update; otherwise keep the order the user gave
    usable.sort(key=lambda i: 1 if i["kind"] == "update" else 0)
    lows = {i["title_id"][8:] for i in usable if i["title_id"]}
    if len(lows) > 1:
        raise GameError("E_REGION_MIX", "the folders are from different regions (" + ", ".join(
            sorted(REGION_BY_LOW.get(x, x) for x in lows)) + "); import one region at a time")
    bases = [i for i in usable if i["kind"] in ("base", "unknown")]
    if len(bases) > 1:
        notices.append(f"{len(bases)} folders look like base games; they are merged in the order given (later over earlier)")
    return [i["path"] for i in usable], [i["path"] for i in dlc]


# ---- validation ----

def layer_path(layers, rel):
    """The path of `rel` in the last layer that has it, or None."""
    for layer in reversed(layers):
        p = os.path.join(layer, rel)
        if os.path.exists(p):
            return p
    return None


def pack_languages(layers):
    langs = set()
    for layer in layers:
        d = os.path.join(layer, "content", "Pack")
        try:
            names = os.listdir(d)
        except OSError:
            continue
        for n in names:
            m = re.fullmatch(r"Bootup_([A-Z]{2}[a-z]{2})\.pack", n)
            if m:
                langs.add(m.group(1))
    return langs


# Known text patches, by the SHA-256 of the language pack they replace. The community Simplified Chinese patch for the
# Japanese copy puts its text into Bootup_JPja.pack (and a font); U-King.rpx stays the v208 original.
KNOWN_TEXT = {"54ac183afd9e1626b51456623761e13688881e014e70502c674d0141f6bf1668": "zh-Hans"}


def validate(layers):
    """Check the (merged) game. Returns {region, title_id, meta_title_id, title_version, rpx_sha256, languages}.
    Raises GameError with a specific code and a message that says what to do."""
    missing = [r for r in REQUIRED if layer_path(layers, r) is None]
    if missing:
        raise GameError("E_STRUCTURE", "not a complete game folder, missing: " + ", ".join(missing))
    meta = None
    for layer in reversed(layers):  # the update's meta.xml wins
        i = title_info(layer)
        if i["title_id"]:
            meta = i
            break
    if meta is None or not meta["title_id"]:
        raise GameError("E_NOT_BOTW", "meta/meta.xml has no title id: this is not Breath of the Wild for Wii U")
    tid = meta["title_id"]
    region = REGION_BY_LOW.get(tid[8:])
    if tid[:8] not in KIND_BY_HIGH or region is None:
        raise GameError("E_NOT_BOTW", f"title id {tid} is not Breath of the Wild for Wii U "
                                      "(expected 101C9300 Japan, 101C9400 USA or 101C9500 Europe)")
    # the highest version in any layer's meta/app.xml (an update folder says 208, the base says less)
    versions = [title_info(l)["title_version"] for l in layers]
    version = max([v for v in versions if v is not None], default=None)
    if version != TITLE_VERSION:
        raise GameError("E_VERSION", f"title_version is {version}, not {TITLE_VERSION} (v208 = update 1.5.0, the last Wii U update): "
                                     "select the v208 update folder together with the base game")
    rpx = layer_path(layers, "code/U-King.rpx")
    digest = sha256_file(rpx)
    if digest != V208_RPX_SHA256:
        raise GameError("E_RPX_HASH", f"code/U-King.rpx has SHA-256 {digest}, expected {V208_RPX_SHA256}: the file is "
                                      "modified, damaged or not the v208 executable; dump it again")
    found = pack_languages(layers)
    ours = [l for l in LANGS[region] if l in found]
    if not found:
        raise GameError("E_LANG", "no language pack (content/Pack/Bootup_<lang>.pack) found")
    if not ours:
        raise GameError("E_LANG", f"language packs {sorted(found)} do not belong to the {region} title id {tid}")
    text = None
    for lang in ours:
        pack = layer_path(layers, f"content/Pack/Bootup_{lang}.pack")
        text = text or KNOWN_TEXT.get(sha256_file(pack))
    return {"region": region, "title_id": "00050000" + tid[8:], "meta_title_id": tid, "title_version": version,
            "rpx_sha256": digest, "languages": ours, "text_patch": text,
            "other_languages": sorted(found - set(ours))}


# ---- copying ----

def copy_tree(src, dst, clone=True):
    """Copy the contents of src over dst (dst/ keeps files src lacks; src files replace). On APFS, `cp -c` clones,
    so a 12 GB game costs almost no extra space; falls back to a plain copy across volumes or off macOS."""
    os.makedirs(dst, exist_ok=True)
    if clone and sys.platform == "darwin" and shutil.which("cp"):
        r = subprocess.run(["cp", "-cR", src.rstrip("/") + "/.", dst], capture_output=True, text=True)
        if r.returncode == 0:
            return "clone"
    shutil.copytree(src, dst, dirs_exist_ok=True, symlinks=True)  # links stay links; clean_tree removes them
    return "copy"


def clean_tree(root):
    """Remove Finder junk and symlinks; returns (file_count, total_bytes, removed)."""
    files = size = 0
    removed = []
    for dirpath, dirnames, filenames in os.walk(root):
        for n in list(dirnames):
            p = os.path.join(dirpath, n)
            if os.path.islink(p):
                os.unlink(p)
                removed.append(os.path.relpath(p, root))
                dirnames.remove(n)
            elif is_junk(n):
                shutil.rmtree(p)
                removed.append(os.path.relpath(p, root))
                dirnames.remove(n)
        for n in filenames:
            p = os.path.join(dirpath, n)
            if os.path.islink(p) or is_junk(n):
                os.unlink(p)
                removed.append(os.path.relpath(p, root))
                continue
            files += 1
            size += os.lstat(p).st_size
    return files, size, removed


def tree_size(root):
    total = 0
    for dirpath, _d, filenames in os.walk(root):
        for n in filenames:
            try:
                total += os.lstat(os.path.join(dirpath, n)).st_size
            except OSError:
                pass
    return total


# ---- the import itself ----

def import_game(paths, work, emit, validate_only=False, installer_version="dev", clone=True):
    """Validate, then copy (APFS clones) into <work>/game and write <work>/game.json. Sources are only read.
    Returns the summary dict. Raises GameError."""
    notices = []
    emit("step", step="import", status="running", detail="looking for the game folders")
    layers, dlc = plan_sources(paths, notices)
    for n in notices:
        emit("notice", step="import", message=n)
    emit("step", step="import", status="running",
         detail="checking: " + " + ".join(os.path.basename(l.rstrip("/")) or l for l in layers))
    info = validate(layers)
    emit("step", step="import", status="ok",
         detail=f"{info['region']} v{info['title_version']}, languages {', '.join(info['languages'])}")
    summary = dict(info, dlc_ignored=[os.path.basename(d) for d in dlc], validated_only=validate_only)
    if validate_only:
        return summary

    os.makedirs(work, exist_ok=True)
    game = os.path.join(work, "game")
    staging = os.path.join(work, "game.staging")
    old = os.path.join(work, "game.old")
    shutil.rmtree(staging, ignore_errors=True)
    shutil.rmtree(old, ignore_errors=True)
    # space: a clone costs ~nothing on the same APFS volume; across volumes the full size is needed
    need = 1 << 30
    if any(os.stat(l).st_dev != os.stat(work).st_dev for l in layers) or not clone:
        need += sum(tree_size(os.path.join(l, d)) for l in layers for d in GAME_DIRS if os.path.isdir(os.path.join(l, d)))
    free = shutil.disk_usage(work).free
    if free < need:
        raise GameError("E_SPACE", f"need {need / 1e9:.1f} GB free in {work}, have {free / 1e9:.1f} GB")
    modes = set()
    for l in layers:
        for d in GAME_DIRS:
            s = os.path.join(l, d)
            if os.path.isdir(s):
                emit("progress", step="import", line=f"copying {os.path.basename(l.rstrip('/'))}/{d}")
                modes.add(copy_tree(s, os.path.join(staging, d), clone=clone))
    files, size, removed = clean_tree(staging)
    if removed:
        emit("notice", step="import", message=f"removed {len(removed)} junk or symlink entries from the copy")
    again = validate([staging])  # the copy itself, not just the sources
    if again["rpx_sha256"] != info["rpx_sha256"]:
        raise GameError("E_COPY", "the copied U-King.rpx differs from the source")
    # commit: the old game stays until the new one is in place
    if os.path.isdir(game):
        os.rename(game, old)
    os.rename(staging, game)
    shutil.rmtree(old, ignore_errors=True)
    summary.update(file_count=files, size_bytes=size, game_dir=game, copy_mode="clone" if modes == {"clone"} else "copy",
                   sources=[os.path.basename(l.rstrip("/")) for l in layers],
                   imported_at=time.strftime("%Y-%m-%dT%H:%M:%S%z"), installer_version=installer_version)
    summary.pop("validated_only", None)
    tmp = os.path.join(work, "game.json.tmp")
    with open(tmp, "w") as f:
        json.dump(summary, f, indent=2, sort_keys=True)
    os.replace(tmp, os.path.join(work, "game.json"))
    emit("step", step="import", status="ok",
         detail=f"{files} files, {size / 1e9:.2f} GB ({summary['copy_mode']}) in {game}")
    return summary
