#!/usr/bin/env python3
"""sv: the SheikahVision installer engine. Builds the Vision Pro app from the player's own copy of the game, on
their own Mac, and puts it on their headset. Standard library only; run it with `xcrun python3 sv.py <command>`.

  sv doctor                      what this Mac and headset still need, each check with the fix
  sv import <path> [<update>]    validate the game (v208, 3 regions) and copy it into the work directory
  sv build                       recompile (cached), build the visionOS app, sign it for your team
  sv install                     install the built app on the headset (never uninstalls first)
  sv push-game                   store-only zip of the game -> Documents/Import/ in the app's container
  sv renew                       re-sign and reinstall the existing build (free teams expire after 7 days)
  sv collect                     import-log.txt and runtime logs from the headset

Common options: --json (one JSON event per line on stdout), --work DIR, --dry-run (print the commands, run nothing
that changes anything), --device UDID, --team TEAMID, --bundle-id ID.

Adapted from Glovebox (MIT license): its doctor checks, team discovery, signing warm-up, JSON event contract and
devicectl retry logic.
"""
import argparse
import datetime
import hashlib
import json
import multiprocessing
import signal
import threading
import zipfile
import uuid
import os
import platform
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import runtimekit
import gameimport as gi  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))  # the repository root
def _installer_version():
    try:  # the repository's VERSION: one version for the headset app, the Mac installer and the build record
        with open(os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "VERSION")) as f:
            return f.read().strip() or "0.0.0"
    except OSError:
        return "0.0.0"


VERSION = _installer_version()
MIN_XCODE = 26
MIN_MACOS = (15, 6)  # what Xcode 26 runs on
SDK = "xros"
APP_REL = os.path.join("build-ios", SDK, "SheikahVision.app")  # build.sh's output (the bundle is named SheikahVision)
BUNDLE_BASE = "com.botw.sheikahvision"
# Initial estimate from serial M5 Air observations; Ninja progress refines it on other Macs.
CPU_SECONDS_PER_TU = 9.0
DEFAULT_TUS = 1100
RETRIES = 5
RETRY_PAUSE = 10
FORBIDDEN_FLAGS = ("--remove-existing-content", "-r", "uninstall", "remove", "delete", "erase", "reset")  # never in a devicectl command (checked below)


class Out:
    json = False


_emit_lock = threading.Lock()


def emit(event, **kw):
    """One JSON object per line with --json, a readable line otherwise. Event names follow Glovebox:
    check / step / progress / notice / command / error / done."""
    if event == "error":
        kw.setdefault("code", "E_STEP")
        kw.setdefault("next", "retry")
    if event == "done" and kw.get("ok"):
        kw.setdefault("next", {"import": "pair", "build": "install", "install": "open-app", "push-game": "import-on-headset",
                               "renew": "done", "collect": "export"}.get(kw.get("step"), "continue"))
    if Out.json:
        with _emit_lock:  # the transfer's progress poller emits from a thread of its own
            print(json.dumps(dict(event=event, **kw), sort_keys=False), flush=True)
        return
    if event == "check":
        mark = {"pass": "[pass]", "warn": "[warn]", "fail": "[FAIL]"}.get(kw.get("status"), "[ ?? ]")
        line = f"{mark} {kw.get('title')}: {kw.get('detail', '')}"
        print(line, flush=True)
        if kw.get("status") != "pass" and kw.get("fix"):
            print(f"       fix: {kw['fix']}", flush=True)
    elif event == "progress":
        if "percent" in kw:
            eta = f", about {int(kw['eta_seconds'] // 60)} min left" if kw.get("eta_seconds") is not None else ""
            print(f"  {kw.get('step')}: {kw['percent']:.0f}% ({kw.get('done')}/{kw.get('total')}){eta}", flush=True)
        else:
            print(f"  {kw.get('line', '')}", flush=True)
    elif event == "command":
        env = " ".join(f"{k}={v}" for k, v in (kw.get("env") or {}).items())
        print(f"  $ {env + ' ' if env else ''}{' '.join(shq(c) for c in kw['cmd'])}"
              + (f"   (in {kw['cwd']})" if kw.get("cwd") else ""), flush=True)
    elif event == "error":
        print(f"error: {kw.get('message')}", file=sys.stderr, flush=True)
    elif event == "notice":
        print(f"note: {kw.get('message')}", flush=True)
    elif event == "step":
        print(f"{kw.get('step')}: {kw.get('status', '')} {kw.get('detail', '')}".rstrip(), flush=True)
    elif event == "done":
        print(f"done: {kw.get('step')} {'ok' if kw.get('ok') else 'FAILED'}", flush=True)
    else:
        print(f"{event}: {kw}", flush=True)


def shq(s):
    return s if re.fullmatch(r"[A-Za-z0-9_@%+=:,./-]+", s) else "'" + s.replace("'", "'\\''") + "'"


class StepError(Exception):
    def __init__(self, message, code="E_STEP"):
        super().__init__(message)
        self.code = code


# ---- environment helpers ----

def fix_path():
    """GUI apps start with a short PATH: add where Homebrew, uv and cargo put tools."""
    extra = ["/opt/homebrew/bin", "/usr/local/bin", os.path.expanduser("~/.local/bin"), os.path.expanduser("~/.cargo/bin")]
    cur = os.environ.get("PATH", "").split(":")
    os.environ["PATH"] = ":".join([p for p in extra if os.path.isdir(p) and p not in cur] + cur)


def run(*cmd, timeout=60):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except (OSError, subprocess.TimeoutExpired) as e:
        return 127, "", str(e)


def default_work():
    return os.environ.get("SV_WORK") or os.path.expanduser("~/Library/Application Support/SheikahVision")


def use_xcode():
    """The Xcode every step uses: DEVELOPER_DIR if set, else xcode-select's when it is a full Xcode, else the newest
    release in /Applications. Set in the environment so nothing needs xcode-select changed (that needs admin rights)."""
    d = os.environ.get("DEVELOPER_DIR", "")
    if not d:
        rc, out, _ = run("xcode-select", "-p")
        d = out.strip() if rc == 0 else ""
    if d.endswith(".app/Contents/Developer") and os.path.isdir(d):
        os.environ["DEVELOPER_DIR"] = d
        return d[: -len("/Contents/Developer")]
    try:
        apps = sorted(os.path.join("/Applications", a) for a in os.listdir("/Applications")
                      if a.startswith("Xcode") and a.endswith(".app"))
    except OSError:
        apps = []

    def version(app):
        try:
            v = plistlib.load(open(os.path.join(app, "Contents", "Info.plist"), "rb"))["CFBundleShortVersionString"]
            return tuple(int(x) for x in v.split(".") if x.isdigit())
        except (OSError, KeyError, ValueError):
            return ()
    apps.sort(key=lambda a: ("beta" not in a.lower(), version(a)))
    if not apps:
        return None
    os.environ["DEVELOPER_DIR"] = apps[-1] + "/Contents/Developer"
    return apps[-1]


# ---- doctor: every check returns (status, detail, fix); status is pass / warn / fail ----

def check_mac():
    ver = platform.mac_ver()[0]
    if platform.machine() != "arm64":
        return "fail", f"{platform.machine()} Mac: an Apple silicon Mac is needed", "use a Mac with an M-series chip"
    try:
        nums = tuple(int(x) for x in ver.split(".")[:2])
    except ValueError:
        nums = ()
    if nums and nums < MIN_MACOS:
        return "fail", f"Apple silicon, macOS {ver}: Xcode {MIN_XCODE} needs macOS {'.'.join(map(str, MIN_MACOS))} or later", \
            "update macOS in System Settings > General > Software Update"
    return "pass", f"Apple silicon, macOS {ver}", None


def check_disk(work=None):
    probe = work if work and os.path.isdir(work) else os.path.expanduser("~")
    free = shutil.disk_usage(probe).free / 1e9
    why = "about 5 GB for the build, 13 GB for the game zip, up to 13 GB more if the game cannot be cloned"
    if free < 25:
        return "fail", f"{free:.0f} GB free, 25 GB needed ({why})", "free up disk space"
    if free < 40:
        return "warn", f"{free:.0f} GB free; enough, but tight ({why})", None
    return "pass", f"{free:.0f} GB free", None


def check_xcode():
    app = use_xcode()
    if not app:
        return "fail", "Xcode is not installed", "install Xcode from the App Store (version %d or later)" % MIN_XCODE
    rc, out, _ = run("xcodebuild", "-version")
    ver = out.split()[1] if rc == 0 and len(out.split()) > 1 else "?"
    try:
        major = int(ver.split(".")[0])
    except ValueError:
        major = 0
    if rc == 0 and major < MIN_XCODE:
        return "fail", f"Xcode {ver} is too old (needs {MIN_XCODE} or later)", "update Xcode"
    rc_l, _, _ = run("xcodebuild", "-license", "check")
    rc_f, _, _ = run("xcodebuild", "-checkFirstLaunchStatus")
    if rc != 0 or rc_l != 0 or rc_f != 0:
        return "fail", "Xcode needs to finish setting up (license, extra components)", \
            "open Xcode once and accept its prompts, or run: sudo xcodebuild -runFirstLaunch"
    rc, sel, _ = run("xcode-select", "-p")
    if rc == 0 and "CommandLineTools" in sel:
        return "warn", f"Xcode {ver} found, but xcode-select points at the Command Line Tools ({sel.strip()}); " \
                       "this engine uses Xcode via DEVELOPER_DIR, but other tools in your shell will not", \
            f"sudo xcode-select -s {app}/Contents/Developer"
    return "pass", f"Xcode {ver} (xcode-select: {sel.strip()})", None


def check_visionos_sdk():
    rc, out, _ = run("xcrun", "--sdk", SDK, "--show-sdk-version")
    if rc != 0:
        return "fail", "the visionOS platform/SDK is not installed", "xcodebuild -downloadPlatform visionOS"
    return "pass", f"visionOS SDK {out.strip()}", None


def runtime_headers():
    paths = {}
    for name in ("boost", "glm"):
        prefix = run("brew", "--prefix", name)[1].strip() if shutil.which("brew") else ""
        candidates = [os.path.join(prefix, "include")] if prefix else []
        candidates += [f"{base}/opt/{name}/include" for base in ("/opt/homebrew", "/usr/local")]
        paths[name] = next((p for p in candidates if os.path.isdir(os.path.join(p, name))), "")
    return paths


def resolve_runtime(args):
    kit = getattr(args, "runtime", None) or os.environ.get("SV_RUNTIME_KIT")
    if kit:
        kit = os.path.abspath(os.path.expanduser(kit))
        try:
            runtimekit.verify(kit)
            return kit
        except (StepError, OSError, ValueError) as e:
            emit("notice", step="runtime", message=f"Prebuilt runtime unavailable ({e}); falling back to source. Install headers if needed: brew install boost glm")
    return None


def check_tools(args=None):
    missing = [t for t in ("cmake", "ninja", "uv") if not shutil.which(t)]
    if missing:
        return "fail", "missing: " + ", ".join(missing), "brew install " + " ".join(missing) + \
            "   (uv: https://docs.astral.sh/uv/ if you do not use Homebrew)"
    kit = resolve_runtime(args)
    if not kit:
        missing_headers = [name for name, path in runtime_headers().items() if not path]
        if missing_headers:
            return "fail", "missing runtime build headers: " + ", ".join(missing_headers), "brew install " + " ".join(missing_headers)
    vers = []
    for t in ("cmake", "ninja", "uv"):
        rc, out, _ = run(t, "--version")
        m = re.search(r"\d+(?:\.\d+)+", out or "")
        vers.append(f"{t} {m.group(0) if m else '?'}")
    return "pass", ", ".join(vers), None


def xcode_prefs():
    rc, out, _ = run("defaults", "export", "com.apple.dt.Xcode", "-")
    if rc != 0:
        return {}
    try:
        return plistlib.loads(out.encode())
    except Exception:
        return {}


def _is_account(entry):
    if isinstance(entry, str):
        return bool(entry.strip())
    if isinstance(entry, dict):
        return any(map(_is_account, entry.values()))
    if isinstance(entry, list):
        return any(map(_is_account, entry))
    return not isinstance(entry, (bool, int, float, datetime.datetime))


def xcode_teams(prefs=None):
    """Teams Xcode's account settings know: [(team id, name, paid)]. Empty when signed out."""
    prefs = xcode_prefs() if prefs is None else prefs
    lists = prefs.get("DVTDeveloperAccountManagerAppleIDLists")
    if isinstance(lists, dict) and not any(isinstance(v, list) and any(map(_is_account, v)) for v in lists.values()):
        return []  # signed out: the cached teams are stale
    teams = []
    for _acct, lst in (prefs.get("IDEProvisioningTeamByIdentifier") or {}).items():
        for t in lst or []:
            if t.get("teamID"):
                teams.append((t.get("teamID"), t.get("teamName"), not t.get("isFreeProvisioningTeam", True)))
    return teams


def keychain_teams():
    """Team IDs that have an 'Apple Development' signing identity here (public certificates only: no keychain prompt)."""
    pems = run("security", "find-certificate", "-a", "-c", "Apple Development", "-p")[1]
    valid = run("security", "find-identity", "-v", "-p", "codesigning")[1]
    teams = set()
    for pem in re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", pems, re.S):
        r = subprocess.run(["openssl", "x509", "-noout", "-subject", "-nameopt", "RFC2253"], input=pem,
                           capture_output=True, text=True).stdout
        m = re.search(r"OU=([A-Z0-9]{10})", r)
        if m:
            teams.add(m.group(1))
    return teams if valid.strip() and "0 valid" not in valid.splitlines()[-1] else set()


def check_team():
    teams = xcode_teams()
    paid = [t for t in teams if t[2]]
    free = [t for t in teams if not t[2]]
    certs = keychain_teams()
    if paid:
        names = ", ".join(f"{t[1]} ({t[0]})" for t in paid)
        extra = f"; also a free team: {free[0][1]} ({free[0][0]})" if free else ""
        return "pass", f"paid Apple Developer Program team: {names}{extra}", None
    if free:
        return "pass", f"free Personal Team {free[0][1]} ({free[0][0]}): works, but the app must be renewed every " \
                       "7 days (sv renew)", None
    if certs:
        return "warn", "no team in Xcode's account list, but a signing certificate exists for " + ", ".join(sorted(certs)) + \
            "; pass --team", "open Xcode > Settings > Accounts so it loads your team"
    return "fail", "no Apple account in Xcode", \
        "open Xcode > Settings > Accounts, sign in with your Apple ID (a free one is enough); no password is given to this tool"


def headsets(probe=False):
    """Paired visionOS devices from `devicectl list devices` (read-only). probe=True also connects to refresh
    Developer Mode state (device info details); off by default."""
    fd, path = tempfile.mkstemp(suffix=".json")
    os.close(fd)
    try:
        rc, _, _ = run("xcrun", "devicectl", "list", "devices", "--json-output", path, timeout=40)
        if rc != 0:
            return None
        devs = load_json(path).get("result", {}).get("devices", [])
    except (OSError, ValueError):
        return None
    finally:
        os.unlink(path)
    out = []
    for d in devs:
        hp, dp, cp = d.get("hardwareProperties", {}), d.get("deviceProperties", {}), d.get("connectionProperties", {})
        if hp.get("platform") != "visionOS" or cp.get("transportType") == "sameMachine":
            continue
        availability = {"connected": "connected", "disconnected": "available"}.get(cp.get("tunnelState"), "offline")
        h = {"availability": availability, "name": dp.get("name"), "os": dp.get("osVersionNumber"), "udid": hp.get("udid"),
             "paired": cp.get("pairingState") == "paired", "developerMode": dp.get("developerModeStatus"),
             "reachable": cp.get("tunnelState") == "connected", "ddi": dp.get("ddiServicesAvailable") is True,
             "id": d.get("identifier"), "transport": cp.get("transportType")}
        if probe and h["paired"] and availability != "offline":
            fd, p2 = tempfile.mkstemp(suffix=".json")
            os.close(fd)
            try:
                rc, _, _ = run("xcrun", "devicectl", "device", "info", "details", "--device", h["id"] or h["udid"], "--timeout", "10",
                               "--json-output", p2, timeout=30)
                response = load_json(p2) if rc == 0 else {}
                r = response.get("result", {}) if response.get("info", {}).get("outcome") == "success" else {}
            except (OSError, ValueError):
                r = {}
            finally:
                os.unlink(p2)
            if r:
                h["developerMode"] = r.get("deviceProperties", {}).get("developerModeStatus", h["developerMode"])
                h["reachable"] = True  # a successful read confirms the idle device can establish a connection
                h["ddi"] = r.get("deviceProperties", {}).get("ddiServicesAvailable") is True
            else:
                h["reachable"] = False
                h["probe_error"] = "Device details unavailable; keep it awake and nearby."
        out.append(h)
    return out


def check_headset(probe=False):
    hs = headsets(probe)
    if hs is None:
        return "warn", "headset status unavailable; pairing can be completed before build", "open Xcode, unlock Vision Pro and pair it before building"
    if not hs:
        return "warn", "no Vision Pro paired with this Mac; pair it before building, installing or transferring", \
            "same Wi-Fi as this Mac; on the headset open Settings > General > Remote Devices, then in Xcode's Device Hub " \
            "+ > Pair Nearby Device (older Xcode: Window > Devices and Simulators) and enter the code"
    h = next((x for x in hs if x["paired"]), None)
    if not h:
        return "warn", f"{hs[0]['name']} is visible but not paired", "pair it in Xcode's Device Hub: + > Pair Nearby Device (older Xcode: Window > Devices and Simulators)"
    if h["developerMode"] != "enabled":
        return "warn", f"{h['name']}: Developer Mode is {h['developerMode'] or 'unknown'} (the list can be stale)", \
            "on the headset: Settings > Privacy & Security > Developer Mode > On (it appears after pairing), restart, then tap Turn On; re-run with --probe"
    if not h["reachable"] or not h["ddi"]:
        return "warn", f"{h['name']} is paired but asleep or unreachable", "put the headset on and unlock it, same Wi-Fi as this Mac"
    extra = f" ({len(hs)} found: pass --device)" if len(hs) > 1 else ""
    return "pass", f"{h['name']} (visionOS {h['os']}, udid {h['udid']}){extra}", None


def doctor(args):
    work = os.path.abspath(os.path.expanduser(args.work))
    checks = [("mac", "This Mac", check_mac), ("xcode", "Xcode", check_xcode), ("visionos", "visionOS SDK", check_visionos_sdk),
              ("tools", "cmake, ninja, uv", lambda: check_tools(args)), ("disk", "Disk space", lambda: check_disk(work)),
              ("team", "Apple account / team", check_team), ("headset", "Vision Pro", lambda: check_headset(args.probe))]
    fails = 0
    results = {}
    for key, title, fn in checks:
        status, detail, fix = fn()
        results[key] = status
        emit("check", check=key, title=title, status=status, detail=detail, fix=fix)
        fails += status == "fail"
        if status == "fail" and key in ("mac", "xcode"):
            break  # the later checks need these
    emit("teams", teams=[dict(id=t[0], name=t[1], paid=t[2]) for t in xcode_teams()])
    emit("done", step="doctor", ok=not fails, results=results, next="account")
    return 1 if fails else 0


# ---- running commands ----

class Runner:
    dry = False


def guard(cmd):
    """No devicectl command of ours may delete anything on the headset: refuse before running."""
    if any("devicectl" in c for c in cmd):
        for c in cmd:
            if c.split("=", 1)[0] in FORBIDDEN_FLAGS or c.startswith("--remove-existing-content"):
                raise StepError(f"refusing to run a devicectl command with {c}: this tool never deletes on the headset", "E_UNSAFE_COMMAND")


_COREDEVICE_IDS = {}


def coredevice_target(device):
    """devicectl addresses a headset more reliably by its CoreDevice identifier than by its hardware UDID (seen
    2026-11-12: `device install app --device <UDID>` failed with CoreDeviceError 4016 while `--device <identifier>`
    worked). Xcode provisioning still uses the UDID; only devicectl --device values go through here."""
    if Runner.dry or not device or re.fullmatch(r"[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}", device):
        return device
    if device not in _COREDEVICE_IDS:
        for h in headsets() or []:
            if h.get("udid") and h.get("id"):
                _COREDEVICE_IDS[h["udid"]] = h["id"]
    return _COREDEVICE_IDS.get(device, device)


def with_coredevice_ids(cmd):
    if not any("devicectl" in c for c in cmd) or "--device" not in cmd:
        return cmd
    cmd = list(cmd)
    i = cmd.index("--device") + 1
    if i < len(cmd):
        cmd[i] = coredevice_target(cmd[i])
    return cmd


def sh(cmd, step, cwd=None, env=None, log=None, on_line=None, capture=False):
    """Run (or, with --dry-run, only print) a command, streaming each output line as a progress event."""
    cmd = with_coredevice_ids(cmd)
    guard(cmd)
    extra = {k: v for k, v in (env or {}).items()}
    shown_env = {k: ("<redacted>" if "KEY" in k or "TOKEN" in k or "PASSWORD" in k else v)
                 for k, v in extra.items() if k.startswith("BOTW_") or k == "DEVELOPER_DIR"}
    emit("command", step=step, cmd=cmd, cwd=cwd, env=shown_env, dry_run=Runner.dry)
    if Runner.dry:
        return ""
    full = dict(os.environ, **extra)
    f = open(log, "a") if log else None
    tail = []
    out = []
    p = None
    try:
        p = subprocess.Popen(cmd, cwd=cwd, env=full, start_new_session=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        for line in p.stdout:
            line = line.rstrip("\n")
            if f:
                f.write(line + "\n")
            tail = (tail + [line])[-12:]
            if capture:
                out.append(line)
            if on_line:
                on_line(line)
            else:
                emit("progress", step=step, line=line)
        p.stdout.close()
        rc = p.wait()
    except BaseException:
        if p is not None and p.poll() is None:
            os.killpg(p.pid, signal.SIGTERM)
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
        raise
    finally:
        if p is not None and p.stdout is not None:
            p.stdout.close()
        if f:
            f.close()
    if rc != 0:
        raise StepError(f"{os.path.basename(cmd[0])} failed ({rc}): " + " | ".join(l for l in tail[-4:] if l.strip()), "E_COMMAND_FAILED")
    return "\n".join(out)


# ---- state kept between commands ----

def state_path(work):
    return os.path.join(work, "build.json")


def load_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_json(path, data):
    path = os.fspath(path)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(data, f, indent=2, sort_keys=True)
    os.replace(tmp, path)


def bundle_id_for(team):
    if not re.fullmatch(r"[A-Z0-9]{10}", team):
        raise StepError("choose a valid 10-character Apple team ID from Xcode > Settings > Accounts")
    return f"{BUNDLE_BASE}.t{team.lower()}"


# ---- import ----

def cmd_import(args):
    work = os.path.abspath(os.path.expanduser(args.work))
    if args.dry_run:
        emit("command", step="import", cmd=["sv", "import"] + args.paths, dry_run=True)
        emit("done", step="import", ok=True, dry_run=True, next="pair")
        return 0
    try:
        s = gi.import_game([os.path.expanduser(p) for p in args.paths], work, emit,
                           validate_only=args.validate_only, installer_version=VERSION)
    except gi.GameError as e:
        emit("error", code=e.code, message=e.message)
        emit("done", step="import", ok=False, code=e.code)
        return 1
    emit("done", step="import", ok=True, game=s)
    return 0


# ---- build ----

def recompiler_version():
    """A hash of everything that decides what recompile.py writes: the script, the analyser/codegen (cafe/*.py), and the
    BOTW_HOOK / BOTW_REPLACE / BOTW_SITE markers in runtime/src/game/*.cpp (the recompiler reads those). Content-based,
    so it works on a dirty tree. The label also carries `git hash-object tools/recompile.py`."""
    h = hashlib.sha256()
    files = [os.path.join(ROOT, "tools", "recompile.py")] + sorted(
        os.path.join(ROOT, "cafe", n) for n in os.listdir(os.path.join(ROOT, "cafe")) if n.endswith(".py"))
    for p in files:
        with open(p, "rb") as fh:
            h.update(os.path.relpath(p, ROOT).encode() + b"\0" + fh.read())
    gdir = os.path.join(ROOT, "runtime", "src", "game")
    marks = []
    if os.path.isdir(gdir):
        for n in sorted(os.listdir(gdir)):
            if n.endswith(".cpp"):
                with open(os.path.join(gdir, n), errors="replace") as fh:
                    for line in fh:
                        if re.match(r"BOTW_(HOOK|REPLACE|SITE)\(0x[0-9a-fA-F]+\)", line):
                            marks.append(f"{n}:{line.strip()}")
    h.update("\n".join(marks).encode())
    h.update(os.environ.get("BOTW_PROBES", "").encode())
    git = run("git", "-C", ROOT, "hash-object", os.path.join(ROOT, "tools", "recompile.py"))[1].strip()[:8]
    return h.hexdigest()[:12], git


def cache_dir(work, rpx_sha):
    ver, git = recompiler_version()
    return os.path.join(work, "cache", f"generated-{rpx_sha[:12]}-{ver}"), ver, git


def count_tus(d):
    try:
        return sum(1 for n in os.listdir(d) if n.endswith(".cpp") and n.startswith(("fn_", "big_", "huge_")))
    except OSError:
        return 0


def estimate_seconds(tus, jobs):
    return tus * CPU_SECONDS_PER_TU / max(1, jobs)


def tool_env(team, bundle_id, paid, work, device=None):
    env = {"BOTW_TEAM": team, "BOTW_BUNDLE_ID": bundle_id, "BOTW_IOS_DEPS": os.path.join(work, "ios-deps"),
           "BOTW_IOS_JOBS": str(multiprocessing.cpu_count())}
    env["BOTW_PAID"] = "1" if paid else "0"
    if device:
        env["BOTW_DEVICE_UDID"] = device
    return env


def resolve_team(args):
    """Use an explicit team, or the sole available team. Multiple teams require a choice."""
    teams = xcode_teams()
    team = args.team or os.environ.get("SV_TEAM")
    if not team:
        pool = teams
        if not pool:
            raise StepError("no Apple team found in Xcode: sign in under Xcode > Settings > Accounts, or pass --team")
        if len(pool) != 1:
            raise StepError("more than one Apple team is available: select a team explicitly with --team")
        team = pool[0][0]
    bundle_id_for(team)
    is_paid = False  # free-compatible capabilities by default, including paid teams
    if args.paid:
        is_paid = True
    if args.free:
        is_paid = False
    return team, is_paid


def latest_identity(team):
    pems = run("security", "find-certificate", "-a", "-c", "Apple Development", "-Z", "-p")[1]
    valid = set(re.findall(r"\b([0-9A-F]{40})\b", run("security", "find-identity", "-v", "-p", "codesigning")[1]))
    identities = []
    for sha, pem in re.findall(r"SHA-1 hash: ([0-9A-F]{40}).*?(-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----)", pems, re.S):
        if sha not in valid:
            continue
        info = subprocess.run(["openssl", "x509", "-noout", "-subject", "-startdate", "-nameopt", "RFC2253"], input=pem, capture_output=True, text=True).stdout
        if not re.search(r"OU=" + re.escape(team) + r"(?:,|$)", info, re.M):
            continue
        date = re.search(r"notBefore=(.*)", info)
        try:
            made = datetime.datetime.strptime(date[1], "%b %d %H:%M:%S %Y %Z") if date else datetime.datetime.min
        except ValueError:
            made = datetime.datetime.min
        identities.append((made, sha))
    return max(identities)[1] if identities else None


def decode_profile(path):
    try:
        with open(path, "rb") as f:
            raw = f.read()
        if not raw.startswith((b"<?xml", b"bplist")):
            raw = subprocess.run(["security", "cms", "-D", "-i", os.fspath(path)], capture_output=True).stdout
        return plistlib.loads(raw)
    except (OSError, ValueError, plistlib.InvalidFileException):
        return {}


def profile_identity(profile, team, bid, wanted, device, identities):
    grants = profile.get("Entitlements", {})
    try:
        expiry = profile["ExpirationDate"].replace(tzinfo=datetime.timezone.utc)
        if (team not in profile.get("TeamIdentifier", []) or
            grants.get("application-identifier") not in (team + "." + bid, team + ".*") or
            expiry <= datetime.datetime.now(datetime.timezone.utc) or
            not all(grants.get(k) == v for k, v in wanted.items()) or
            not (profile.get("ProvisionedDevices") or profile.get("ProvisionsAllDevices")) or
            (device and device not in profile.get("ProvisionedDevices", []) and not profile.get("ProvisionsAllDevices"))):
            return None
        return next((hashlib.sha1(c).hexdigest().upper() for c in profile.get("DeveloperCertificates", [])
                     if hashlib.sha1(c).hexdigest().upper() in identities), None)
    except (KeyError, TypeError, AttributeError):
        return None


def warm_up_signing(team, work, identity=None):
    """Sign one scratch file alone before the real signing, so a macOS keychain prompt ("codesign wants to use your key",
    answer Always Allow) appears once, here, with an explanation. See Glovebox docs/SIGNING-PROMPTS.md."""
    if Runner.dry:
        emit("command", step="sign", cmd=["codesign", "--sign", identity or team, "<warmup-file>"], dry_run=True)
        return None
    chosen = identity or latest_identity(team)
    ids = [chosen] if chosen else []
    if not ids:
        emit("progress", step="sign", line=f"no signing key for team {team} yet: in Xcode Settings > Accounts > Manage Certificates, add Apple Development, then warm up again")
        return
    cmd = ["codesign", "--force", "--sign", ids[0], "--timestamp=none", os.path.join(work, "warmup", "signing-check")]
    emit("command", step="sign", cmd=cmd, dry_run=Runner.dry)
    if Runner.dry:
        return None
    d = os.path.join(work, "warmup")
    os.makedirs(d, exist_ok=True)
    with open(cmd[-1], "w") as f:
        f.write("SheikahVision\n")
    emit("notice", step="sign", message="macOS may ask once for your Mac password to let codesign use your signing key: "
                                        "click Always Allow (with the mouse; Enter picks Allow once)")
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        raise StepError("macOS is waiting for permission to use your signing key: answer its window with your password and Always Allow")
    if r.returncode != 0:
        raise StepError("macOS did not let this Mac sign with your key (" + (r.stderr.strip().splitlines() or ["?"])[-1] + ")")

    return ids[0]


def profile_expiry(app):
    p = os.path.join(app, "embedded.mobileprovision")
    if not os.path.exists(p):
        return None
    raw = subprocess.run(["security", "cms", "-D", "-i", p], capture_output=True).stdout
    try:
        return plistlib.loads(raw)["ExpirationDate"].strftime("%Y-%m-%d")
    except Exception:
        return None


def progress_parser(step, started):
    """ninja prints [done/total]; turn it into percent + estimate, at most one event per 2 s."""
    last = [0.0, -1]

    def on_line(line):
        m = re.match(r"\[(\d+)/(\d+)\]", line)
        if not m:
            if line.strip() and not line.startswith(" "):
                emit("progress", step=step, line=line[:200])
            return
        done, total = int(m.group(1)), int(m.group(2))
        now = time.time()
        if now - last[0] < 2 and done != total:
            return
        last[0], last[1] = now, done
        el = now - started
        eta = el * (total - done) / done if done > 20 else None
        emit("progress", step=step, done=done, total=total, percent=100.0 * done / total, elapsed_seconds=int(el),
             eta_seconds=None if eta is None else int(eta))
    return on_line


def source_commit():
    """The commit the source comes from: the bundled source records it (source-commit, written when the installer is
    packaged from a clean checkout); a checkout asks git and marks local source edits with '+'."""
    try:
        with open(os.path.join(ROOT, "source-commit")) as f:
            value = f.read().strip()
            if value:
                return value
    except OSError:
        pass
    rc, out, _ = run("git", "-C", ROOT, "rev-parse", "--short=8", "HEAD")
    if rc != 0 or not out.strip():
        return "unknown"
    rc2, dirty, _ = run("git", "-C", ROOT, "status", "--porcelain", "--", "runtime", "ios", "cafe", "tools", "CMakeLists.txt", "VERSION")
    return out.strip() + ("+" if rc2 == 0 and dirty.strip() else "")


def source_version():
    """Hash tracked source contents, including local edits; exclude docs and installer UI."""
    prefixes = ("runtime", "ios", "CMakeLists.txt", "cafe", "tools/recompile.py", "installer/cmake")
    rc, output, _ = run("git", "-C", ROOT, "ls-files", *prefixes)
    if rc == 0 and output.strip():
        names = output.splitlines()
    else:
        listed = load_json(os.path.join(ROOT, "source-files.json"))
        names = sorted(n for n in listed if any(n == p or n.startswith(p + "/") for p in prefixes))
    if not names:
        raise StepError("the public source manifest is missing: choose a complete source checkout or reinstall the installer")
    h = hashlib.sha256()
    for name in names:
        path = os.path.join(ROOT, name)
        h.update(name.encode() + b"\0")
        if os.path.isfile(path):
            with open(path, "rb") as f:
                h.update(f.read())
    return h.hexdigest()


def guest_key(rpx_sha):
    headers = hashlib.sha256()
    root = os.path.join(ROOT, "runtime/include")
    paths = sorted(os.path.relpath(os.path.join(folder, name), root) for folder, _, names in os.walk(root) for name in names)
    for relative in paths:
        with open(os.path.join(root, relative), "rb") as f:
            headers.update(relative.encode() + b"\0" + f.read())
    config = "O2;big=O1;huge=O0;apple-a14;fp-contract=off;wrapv;no-strict-aliasing;xros26.0"
    return hashlib.sha256((rpx_sha + recompiler_version()[0] + headers.hexdigest() + config +
                           runtimekit.compiler_version() +
                           run("xcrun", "--sdk", SDK, "--show-sdk-version")[1]).encode()).hexdigest()[:24]


def compiled_key(rpx_sha):
    compiler = runtimekit.compiler_version()
    sdk = run("xcrun", "--sdk", SDK, "--show-sdk-version")[1]
    return hashlib.sha256((rpx_sha + recompiler_version()[0] + compiler + sdk + source_version()).encode()).hexdigest()[:24]


def device_json(arguments, step="preflight"):
    """Device information is exclusively read from devicectl's versioned JSON output. The step "progress" (the transfer's
    size samples, every few seconds) runs quietly: no command or output lines in the log."""
    with tempfile.TemporaryDirectory(prefix="sv-device-") as d:
        path = os.path.join(d, "result.json")
        if step == "progress":
            cmd = with_coredevice_ids(["xcrun", "devicectl"] + arguments + ["--json-output", path])
            guard(cmd)
            if Runner.dry or subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30).returncode:
                return {}
            return load_json(path).get("result") or {}
        sh(["xcrun", "devicectl"] + arguments + ["--json-output", path], step)
        if Runner.dry:
            return {}
        data = load_json(path)
        if data.get("info", {}).get("outcome") != "success" or not isinstance(data.get("result"), dict):
            raise StepError("Xcode did not return usable device information. Unlock and reconnect Vision Pro, then try again.", "E_DEVICE_JSON")
        return data["result"]


def validate_bundle_id(bid):
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9-]*(?:\.[A-Za-z0-9][A-Za-z0-9-]*){2,}", bid):
        raise StepError("use a bundle ID such as com.yourname.sheikahvision (letters, numbers, dots and hyphens only)")
    return bid


def forget_moved_cmake_cache(build, source):
    """CMake refuses a build folder configured from another source folder (the installer's source copy moved, or
    Advanced chose another checkout). Drop only CMake's own cache then; the next configure starts that folder fresh."""
    cache = os.path.join(build, "CMakeCache.txt")
    if Runner.dry or not os.path.isfile(cache):
        return
    with open(cache, errors="replace") as f:
        home = next((l.split("=", 1)[1].strip() for l in f if l.startswith("CMAKE_HOME_DIRECTORY:")), None)
    if home and os.path.realpath(home) != os.path.realpath(source):
        emit("notice", message="The source folder changed since this build folder was configured; configuring it again.")
        os.remove(cache)
        shutil.rmtree(os.path.join(build, "CMakeFiles"), ignore_errors=True)


def provision(team, bid, paid, work, device=None):
    bundle_id_for(team)
    validate_bundle_id(bid)
    ent = os.path.join(ROOT, "ios", "SheikahVision-paid.entitlements" if paid else "SheikahVision.entitlements")
    dest = os.path.join(work, "provision", team, bid)
    forget_moved_cmake_cache(dest, os.path.join(ROOT, "ios/provision"))
    sh(["cmake", "-S", os.path.join(ROOT, "ios/provision"), "-B", dest, "-G", "Xcode",
        "-DCMAKE_SYSTEM_NAME=visionOS", "-DCMAKE_OSX_SYSROOT=xros", "-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0",
        "-DCMAKE_OSX_ARCHITECTURES=arm64", "-DBOTW_TEAM=" + team,
        "-DBOTW_BUNDLE_ID=" + bid, "-DBOTW_ENTITLEMENTS=" + ent], "provision")
    warmed = warm_up_signing(team, work)
    cmd = ["xcodebuild", "-project", os.path.join(dest, "BotWProvision.xcodeproj"), "-scheme", "BotWProvision",
           "-destination", "platform=visionOS,id=" + device if device else "generic/platform=visionOS",
           "-allowProvisioningUpdates", "-allowProvisioningDeviceRegistration", "build"]
    try:
        try:
            sh(cmd, "provision", log=os.path.join(work, "provision.log") if not Runner.dry else None)
        except StepError as e:
            history = str(e)
            if os.path.isfile(os.path.join(work, "provision.log")):
                with open(os.path.join(work, "provision.log")) as f:
                    history += f.read()[-65536:]
            if not device or not re.search(r"Unable to find a destination|is not available|Unable to find a device", history, re.I):
                raise
            emit("notice", step="provision", message="Headset is offline; retrying the generic visionOS destination. Installation will check registration again.")
            cmd[cmd.index("-destination") + 1] = "generic/platform=visionOS"
            cmd.remove("-allowProvisioningDeviceRegistration")
            sh(cmd, "provision", log=os.path.join(work, "provision.log") if not Runner.dry else None)
    except StepError as e:
        history = str(e)
        if os.path.isfile(os.path.join(work, "provision.log")):
            with open(os.path.join(work, "provision.log")) as f:
                history += f.read()[-65536:]
        if re.search(r"no devices|no registered|doesn.t include|register.*device|requires.*device", history, re.I):
            raise StepError("Connect and unlock Vision Pro, enable Developer Mode and pair it in Xcode first. Personal Teams need a registered headset before signing.", "E_DEVICE_REGISTRATION") from e
        raise
    return warmed


def select_profile(team, bid, paid, device=None):
    wanted_path = os.path.join(ROOT, "ios", "SheikahVision-paid.entitlements" if paid else "SheikahVision.entitlements")
    with open(wanted_path, "rb") as f:
        wanted = plistlib.load(f)
    identities = run("security", "find-identity", "-v", "-p", "codesigning")[1]
    profiles = []
    for folder in ("~/Library/Developer/Xcode/UserData/Provisioning Profiles", "~/Library/MobileDevice/Provisioning Profiles"):
        from pathlib import Path
        for p in Path(os.path.expanduser(folder)).glob("*.mobileprovision"):
            profile = decode_profile(p)
            identity = profile_identity(profile, team, bid, wanted, device, identities)
            if identity:
                profiles.append((profile["ExpirationDate"], p, profile["Entitlements"], identity))
    if not profiles:
        raise StepError("no valid profile for the selected team, capabilities and headset: connect and pair Vision Pro in Xcode, then build again", "E_PROFILE")
    return max(profiles, key=lambda x: x[0]), wanted


def sign_app(app, team, bid, paid, work, device=None, compile_assets=True, warmed=None):
    """Select only a valid profile and identity belonging to the explicitly chosen team."""
    if Runner.dry:
        emit("command", step="sign", cmd=["codesign", "--force", "--sign", team, app], dry_run=True)
        return
    (_, path, grants, identity), wanted = select_profile(team, bid, paid, device)
    if identity != warmed:
        warm_up_signing(team, work, identity=identity)
    shutil.copyfile(path, os.path.join(app, "embedded.mobileprovision"))
    ent = dict(wanted, **{"application-identifier": team + "." + bid, "com.apple.developer.team-identifier": team,
                          "get-task-allow": bool(grants.get("get-task-allow"))})
    ent_path = os.path.join(work, "signing-entitlements.plist")
    with open(ent_path, "wb") as f:
        plistlib.dump(ent, f)
    # Compile only public assets, matching ios/scripts/sign.sh.
    assets = os.path.join(ROOT, "ios/assets/Vision.xcassets")
    if compile_assets and os.path.isdir(assets):
        partial = os.path.join(work, "icon-info.plist")
        sh(["xcrun", "actool", assets, "--compile", app, "--platform", "xros", "--minimum-deployment-target", "26.0",
            "--app-icon", "AppIcon", "--target-device", "vision", "--output-partial-info-plist", partial], "assets")
        with open(os.path.join(app, "Info.plist"), "rb") as f:
            info = plistlib.load(f)
        with open(partial, "rb") as f:
            info.update(plistlib.load(f))
        with open(os.path.join(app, "Info.plist"), "wb") as f:
            plistlib.dump(info, f)
    sh(["codesign", "--force", "--sign", identity, "--timestamp=none", "--entitlements", ent_path, app], "sign")
    sh(["codesign", "--verify", "--strict", app], "sign")


def cmd_status(args):
    work = os.path.abspath(os.path.expanduser(args.work))
    game, game_missing = {}, False
    if load_json(os.path.join(work, "game.json")):
        # the player may delete the Mac copy to free space once the game is on the headset: renewing still works,
        # only a rebuild or a new transfer needs the game folder chosen again
        try:
            game = gi.validate([os.path.join(work, "game")])
            receipt = load_json(os.path.join(work, "game.json"))
            game.update({key: receipt[key] for key in ("file_count", "size_bytes", "dlc_ignored") if key in receipt})
        except gi.GameError as e:
            game, game_missing = {}, True
            emit("notice", step="status", message=f"The game copy on this Mac is missing or incomplete ({e.code}); choose the game folder again before rebuilding.")
    state = load_json(state_path(work))
    built = bool(state.get("app") and os.path.isfile(os.path.join(state["app"], "Info.plist")))
    emit("done", step="status", ok=True, game=game or None, game_missing=game_missing, built=built,
         installed_devices=state.get("installed_devices", {}), team=state.get("team"), expires=state.get("expires"),
         installer_version=VERSION, built_version=state.get("installer_version"),
         built_commit=state.get("source_commit"), source_commit=source_commit(), source_changed=source_changed(state),
         estimated_build_minutes=round(estimate_seconds(DEFAULT_TUS, multiprocessing.cpu_count()) / 60))
    return 0


def source_changed(state):
    """The installed app was built from other source than this installer carries (same VERSION or not)."""
    try:
        return bool(state.get("source_version")) and state["source_version"] != source_version()
    except StepError:
        return False


def cmd_devices(args):
    devices = ([] if Runner.dry else headsets(probe=getattr(args, "probe", False))) or []
    emit("devices", devices=devices)
    emit("done", step="devices", ok=True)
    return 0


def device_is_reachable(device):
    if Runner.dry:
        return True
    return any(h.get("paired") and h.get("reachable") and device in (h.get("udid"), h.get("id"))
               for h in (headsets(probe=True) or []))


def cmd_launch(args):
    """Request launch once, distinguishing a lost connection from a trust/launch failure."""
    work, state = load_build_state(args, need_app=False)
    device = resolve_device(args)
    try:
        device_json(["device", "process", "launch", "--device", device, state["bundle_id"]], "launch")
    except StepError as error:
        if not device_is_reachable(device):
            raise StepError("Vision Pro disconnected. Wake it up and reconnect, then try again.", "E_DEVICE_UNREACHABLE") from error
        raise
    emit("done", step="launch", ok=True, dry_run=Runner.dry)
    return 0


def expected_package_size(work):
    """The game zip's size: the zip on this Mac while it is kept (a transfer not finished yet), else what the last
    finished transfer recorded (push.json); None when neither is known. Either counts only when it was made from the
    game imported now (rpx_sha256 against game.json): after another dump is imported, an old zip of the same size on
    the headset must not pass for the new one."""
    current = load_json(os.path.join(work, "game.json")).get("rpx_sha256")
    def same_game(record):
        return not current or not record.get("rpx_sha256") or record.get("rpx_sha256") == current
    zp = os.path.join(work, "SheikahVision-game.zip")
    if os.path.isfile(zp) and same_game(load_json(zp + ".json")):
        return os.path.getsize(zp)
    pushed = load_json(os.path.join(work, "push.json"))
    size = pushed.get("size")
    return int(size) if isinstance(size, int) and size > 0 and same_game(pushed) else None


def waiting_package_complete(work, listing):
    """Whether Documents/Import holds the whole game zip. A transfer cut off or cancelled leaves part of it there; that
    part was taken for a package already sent, the transfer was skipped and the headset was left with a broken zip
    (user 2026-11-18: 3.89 of 13.0 GB). A zip of another size is sent again (the copy overwrites it)."""
    files = [f for f in (listing or {}).get("files", []) if f.get("name") == "SheikahVision-game.zip"]
    if not files:
        return False
    on_headset = int((files[0].get("metadata") or {}).get("size") or 0)
    expected = expected_package_size(work)
    if expected is None:
        # nothing to compare with: with the game still on this Mac, send it again rather than trust a zip that may be
        # cut off; without it (deleted after an earlier install) the app imports what is there, as before
        return not os.path.isfile(os.path.join(work, "game.json"))
    if on_headset != expected:
        emit("notice", step="documents", message=f"The game package on the headset is incomplete ({on_headset / 1e9:.2f} of "
                                                  f"{expected / 1e9:.2f} GB); it will be sent again.")
        return False
    return True


def cmd_documents(args):
    """Read-only readiness probe. An ambiguous failure is never presented as 'open the app'."""
    work, state = load_build_state(args, need_app=False)
    device = resolve_device(args)
    ready, reason, has_game, pending, app_installed = False, "unreachable", False, False, None
    if device_is_reachable(device):
        try:
            listing = device_json(["device", "info", "files", "--device", device, "--domain-type", "appDataContainer",
                                   "--domain-identifier", state["bundle_id"], "--subdirectory", "Documents", "--no-recurse"], "documents")
            ready, reason, app_installed = True, None, True
            names = {f.get("name") for f in (listing or {}).get("files", [])}
            # the headset app writes Documents/game-receipt.json after a verified import
            has_game = "game-receipt.json" in names
            # An import under way (the .game-import work folder): the app imports by itself on launch, so sending the
            # zip again would overwrite a file it may be reading. Exact names: .game-import-released.json (an import
            # cancelled after it freed part of the zip, which the app then refuses) and .game-import-trash-* are not
            # imports under way; a released zip is sent again (a new file the app accepts)
            pending = ".game-import" in names
            released = ".game-import-released.json" in names
            if released and not has_game and not pending:
                emit("notice", step="documents", message="An earlier import on the headset was stopped part-way; the game package will be sent again.")
            if not has_game and not pending and not released:
                try:
                    waiting = device_json(["device", "info", "files", "--device", device, "--domain-type", "appDataContainer",
                                           "--domain-identifier", state["bundle_id"], "--subdirectory", "Documents/Import", "--no-recurse"], "documents")
                    pending = waiting_package_complete(work, waiting)
                except StepError:
                    pending = False
        except StepError as error:
            emit("notice", step="documents", message=str(error))
            missing = re.search(r"no such (?:file|directory)|(?:documents|container|directory|path|file).{0,120}(?:does not exist|not found)|(?:could not|unable to) find.{0,120}(?:container|documents)|(?:no|missing) (?:app data )?container",
                                str(error), re.I)
            if missing:
                emit("notice", step="documents", message="The app container or Documents folder is not available yet.")
            if device_is_reachable(device):
                reason = "not-opened"
                # whether the app is there at all (read-only): a listing can fail for other reasons (a locked headset,
                # a dropped connection), and only an app known to be gone may send the installer back to install
                try:
                    apps = device_json(["device", "info", "apps", "--device", device, "--bundle-id", state["bundle_id"]], "documents")
                    app_installed = any(a.get("bundleIdentifier") == state["bundle_id"] for a in apps.get("apps", []))
                except StepError:
                    app_installed = None
    emit("done", step="documents", ok=True, ready=ready, reason=reason, has_game=has_game, game_pending=pending,
         app_installed=app_installed, dry_run=Runner.dry)
    return 0


def cmd_account(args):
    emit("teams", teams=[dict(id=t[0], name=t[1], paid=t[2]) for t in xcode_teams()])
    warmed = None
    if args.team and not getattr(args, "list_only", False):
        team, _ = resolve_team(args)
        if not Runner.dry:
            os.makedirs(args.work, exist_ok=True)
        warmed = warm_up_signing(team, args.work)
    emit("done", step="account", ok=True, warmed=bool(warmed), team=args.team, next="import")
    return 0


def publish_app(app, destination):
    temporary = destination + ".staging-" + uuid.uuid4().hex
    old = destination + ".previous"
    if os.path.isdir(old) and not os.path.exists(destination):
        os.rename(old, destination)
    try:
        sh(["ditto", app, temporary], "package")
        if os.path.exists(old):
            shutil.rmtree(old)
        if os.path.exists(destination):
            os.rename(destination, old)
        try:
            os.rename(temporary, destination)
        except BaseException:
            if os.path.exists(old) and not os.path.exists(destination):
                os.rename(old, destination)
            raise
        shutil.rmtree(old, ignore_errors=True)
    finally:
        if os.path.isdir(old) and not os.path.exists(destination):
            os.rename(old, destination)
        shutil.rmtree(temporary, ignore_errors=True)


def prune_build_caches(work, current):
    cache = os.path.join(work, "cache")
    if not os.path.isdir(cache):
        return
    directories = sorted((os.path.join(cache, n) for n in os.listdir(cache) if re.fullmatch(r"build-[a-f0-9]{24}", n)
                          and os.path.isdir(os.path.join(cache, n)) and not os.path.islink(os.path.join(cache, n))),
                         key=os.path.getmtime, reverse=True)
    for path in directories[3:]:
        if path != current and time.time() - os.path.getmtime(path) > 30 * 86400:
            shutil.rmtree(path)
            emit("notice", step="cache", message="Removed unused build cache older than 30 days: " + os.path.basename(path))


def refresh_host_cache(build_dir, source, kit, previous=None):
    inputs = dict(source_version=source, kit_manifest_sha=runtimekit.digest(os.path.join(kit, "runtime.json")) if kit else None)
    marker = os.path.join(build_dir, "host-inputs.json")
    if not Runner.dry:
        if load_json(marker) != inputs or (previous is not None and any(previous.get(k) != v for k, v in inputs.items())):
            for name in ("runtime_host", "cemu_gpu", "botw_immersive"):
                shutil.rmtree(os.path.join(build_dir, "CMakeFiles", name + ".dir"), ignore_errors=True)
            for name in ("SheikahVision.app", "libsheikah_runtime.a", "libbotw_immersive.a"):
                path = os.path.join(build_dir, name)
                if os.path.isdir(path):
                    shutil.rmtree(path)
                elif os.path.exists(path):
                    os.unlink(path)
        os.makedirs(build_dir, exist_ok=True)
        save_json(marker, inputs)
    return inputs


def cmd_build(args):
    kit = resolve_runtime(args)
    if not kit and not Runner.dry:
        missing = [name for name, path in runtime_headers().items() if not path]
        if missing:
            raise StepError("source build needs headers: brew install " + " ".join(missing), "E_TOOLS")
    work = os.path.abspath(os.path.expanduser(args.work))
    gj = load_json(os.path.join(work, "game.json"))
    rpx = os.path.join(work, "game", "code", "U-King.rpx")
    if not gj or (not os.path.isfile(rpx) and not args.dry_run):
        raise StepError("no imported game: run 'sv import <path>' first", "E_NO_GAME")
    validated = gi.validate([os.path.join(work, "game")]) if not Runner.dry else gj
    rpx_sha = validated.get("rpx_sha256") or gi.V208_RPX_SHA256
    cache, ver, git = cache_dir(work, rpx_sha)
    marker = os.path.join(cache, ".complete")
    try:
        team, paid = resolve_team(args)
    except StepError as e:
        if not args.dry_run:
            raise StepError(str(e), "E_TEAM") from e
        team, paid = "DRYRUN0000", bool(args.paid)
        emit("notice", step="build", message=f"{e}; the dry run continues with a placeholder team")
    bundle_id = args.bundle_id or bundle_id_for(team)
    device = resolve_device(args)
    jobs = multiprocessing.cpu_count()
    record = load_json(marker)
    cached = (record.get("rpx_sha256") == rpx_sha and record.get("recompiler_version") == ver
              and count_tus(cache) > 0)
    tus = count_tus(cache) if cached else DEFAULT_TUS
    emit("step", step="plan", status="ok", detail=json.dumps({
        "team": team, "paid": paid, "bundle_id": bundle_id, "entitlements": "paid (+extended-virtual-addressing)" if paid
        else "free team (increased-memory-limit only)", "recompile_cache": cache, "recompile_cache_valid": cached,
        "recompiler_version": ver, "recompiler_git": git, "rpx_sha256": rpx_sha,
        "translation_units": tus, "tus_are_estimate": not cached, "jobs": jobs,
        "estimated_build_minutes": round(estimate_seconds(tus, jobs) / 60)}))
    log = os.path.join(work, "build.log")
    try:
        if not Runner.dry:
            os.makedirs(os.path.join(work, "cache"), exist_ok=True)
        # 1. recompile (cached)
        if cached:
            emit("step", step="recompile", status="ok", detail=f"cache hit: {os.path.basename(cache)}")
        else:
            part = cache + ".partial"
            emit("step", step="recompile", status="running", detail="U-King.rpx -> C++ (a few minutes)")
            sh(["uv", "run", "python", "-I", os.path.join("tools", "recompile.py"), rpx, part], "recompile", cwd=ROOT, log=log)
            if not Runner.dry:
                if os.path.isdir(cache):
                    shutil.rmtree(cache)
                os.rename(part, cache)
                save_json(marker, {"rpx_sha256": rpx_sha, "recompiler_version": ver, "recompiler_git": git,
                                   "translation_units": count_tus(cache), "created": time.strftime("%Y-%m-%dT%H:%M:%S")})
                tus = count_tus(cache)
                emit("step", step="recompile", status="ok", detail=f"{tus} translation units")
        # CMake receives the cache path; never modify a developer's generated/ directory.
        # Keep provisioning and signed products private to this installer work folder.
        t0 = time.monotonic()
        build_key = compiled_key(rpx_sha)
        build_dir = os.path.join(work, "cache", "build-" + guest_key(rpx_sha))
        build_number, commit = time.strftime("%Y%m%d%H%M"), source_commit()
        inputs = refresh_host_cache(build_dir, source_version(), kit, load_json(state_path(work)))
        app = os.path.join(build_dir, "SheikahVision.app")
        warmed = provision(team, bundle_id, paid, work, device)
        if not Runner.dry:
            (_, _, _, profile_key), _ = select_profile(team, bundle_id, paid, device)
            if profile_key != warmed:
                warmed = warm_up_signing(team, work, identity=profile_key)
        deps = kit or os.path.join(ROOT, "ios-deps")
        if not os.path.isdir(os.path.join(deps, "prefix-xros")):
            sh([os.path.join(ROOT, "ios/scripts/build_deps.sh"), SDK], "dependencies", cwd=ROOT,
               env={"BOTW_IOS_DEPS": deps})
        forget_moved_cmake_cache(build_dir, os.path.join(ROOT, "ios"))
        sh(["cmake", "-S", os.path.join(ROOT, "ios"), "-B", build_dir, "-G", "Ninja",
            "-DCMAKE_SYSTEM_NAME=visionOS", "-DCMAKE_OSX_SYSROOT=xros", "-DCMAKE_OSX_ARCHITECTURES=arm64",
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0", "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DBOTW_GUEST_OPT=", "-DBOTW_BUNDLE_ID=" + bundle_id,
            "-DBOTW_IOS_DEPS=" + deps, "-DBOTW_GENERATED_DIR=" + cache,
            "-DBOTW_PREBUILT_RUNTIME=" + (kit or ""), "-DBOTW_RUNTIME_ONLY=OFF",
            # CFBundleVersion rises with every build (same VERSION, still tells builds apart); the launcher shows the commit
            "-DBOTW_BUILD_NUMBER=" + build_number, "-DBOTW_SOURCE_COMMIT=" + commit,
            *(["-DBOTW_SOURCE_VERSION=" + inputs["source_version"],
               "-DBOTW_BOOST_INCLUDE=" + runtime_headers()["boost"],
               "-DBOTW_GLM_INCLUDE=" + runtime_headers()["glm"]] if not kit else [])], "configure", cwd=ROOT, log=log)
        sh(["cmake", "--build", build_dir, "-j", str(jobs)], "build", cwd=ROOT, log=log,
           on_line=progress_parser("build", time.time()))
        sign_app(app, team, bundle_id, paid, work, device, warmed=warmed)
        elapsed = round(time.monotonic() - t0, 3)
        out_app = os.path.join(work, "app", "SheikahVision.app")
        if not Runner.dry:
            os.makedirs(os.path.dirname(out_app), exist_ok=True)
            publish_app(app, out_app)
            st = {"team": team, "paid": paid, "bundle_id": bundle_id, "app": out_app,
                  "built_at": time.strftime("%Y-%m-%dT%H:%M:%S"), "expires": profile_expiry(out_app),
                  "rpx_sha256": rpx_sha, "recompiler_version": ver, "build_key": build_key,
                  "elapsed_seconds": elapsed, "installer_version": VERSION, "build_number": build_number,
                  "source_commit": commit, **inputs}
            save_json(state_path(work), st)
            prune_build_caches(work, build_dir)
        emit("step", step="build", status="ok", elapsed_seconds=elapsed, cache_key=build_key)
    except StepError as e:
        emit("error", code=getattr(e, "code", "E_STEP"), message=str(e))
        emit("done", step="build", ok=False)
        return 1
    emit("done", step="build", ok=True, dry_run=Runner.dry, next="install", expires=profile_expiry(out_app) if not Runner.dry else None)
    return 0


# ---- device commands ----

def resolve_device(args):
    """--device, else the single paired Vision Pro. With --dry-run and nothing found: a placeholder."""
    if args.device:
        return args.device
    if Runner.dry:
        return "<device-udid>"
    hs = headsets()
    paired = [h for h in (hs or []) if h["paired"]]
    if len(paired) == 1:
        return paired[0]["udid"]
    if Runner.dry:
        emit("notice", message="no single paired Vision Pro found; the dry run uses <device-udid>")
        return "<device-udid>"
    if not paired:
        raise StepError("no paired Vision Pro: pair it in Xcode first, or pass --device <udid>", "E_DEVICE")
    raise StepError("more than one Vision Pro is paired: pass --device <udid> (" + ", ".join(
        f"{h['name']}={h['udid']}" for h in paired) + ")")


def load_build_state(args, need_app=True):
    work = os.path.abspath(os.path.expanduser(args.work))
    st = load_json(state_path(work))
    if args.bundle_id:
        st["bundle_id"] = args.bundle_id
    if not st.get("bundle_id"):
        if args.team:
            st["bundle_id"] = bundle_id_for(args.team)
        elif Runner.dry:
            st["bundle_id"] = bundle_id_for("DRYRUN0000")
            emit("notice", message="no build recorded yet; the dry run uses a placeholder bundle id")
        else:
            raise StepError("nothing built yet: run 'sv build' first (or pass --bundle-id)")
    if need_app and not st.get("app"):
        if Runner.dry:
            st["app"] = os.path.join(work, "app", "SheikahVision.app")
        else:
            raise StepError("nothing built yet: run 'sv build' first")
    validate_bundle_id(st["bundle_id"])
    if need_app and not Runner.dry and os.path.isfile(os.path.join(st["app"], "Info.plist")):
        with open(os.path.join(st["app"], "Info.plist"), "rb") as f:
            info = plistlib.load(f)
        if info.get("CFBundleIdentifier") != st["bundle_id"]:
            raise StepError("the app bundle ID differs from the recorded installation: build with the desired --bundle-id first")
    return work, st


def install_app(app, device, step="install"):
    """devicectl install over the existing app (never uninstall: that would delete the saves). Retries when the
    headset drops the connection."""
    cmd = ["xcrun", "devicectl", "device", "install", "app", "--device", device, app]
    for attempt in range(3):
        try:
            device_json(cmd[2:], step)
            return
        except StepError as e:
            if attempt == 2 or not re.search(r"unexpectedly closed|connection.*closed", str(e), re.I):
                raise
            emit("progress", step=step, line="the Vision Pro dropped the connection; trying again")
            time.sleep(5)


PROFILE_FOLDER = "~/Library/Developer/Xcode/UserData/Provisioning Profiles"


def cached_profiles(team, bid):
    from pathlib import Path
    out = []
    for p in Path(os.path.expanduser(PROFILE_FOLDER)).glob("*.mobileprovision"):
        profile = decode_profile(p)
        if team in profile.get("TeamIdentifier", []) and profile.get("Entitlements", {}).get("application-identifier", "") == f"{team}.{bid}":
            out.append(p)
    return out


def retire_cached_profiles(team, bid, work):
    """A free Personal Team's profile lasts 7 days, and while one is cached Xcode keeps using it: renewing re-signed the
    app with the same profile and the expiry did not move (user 2026-11-18: still 1 day left after renewing). Before a
    free team's renewal, this app's cached profiles are moved (not deleted) into the work folder, so Xcode fetches a
    new one; restore_profiles() puts them back if that fails. Returns the moved (original, kept) paths."""
    keep = os.path.join(work, "retired-profiles", time.strftime("%Y%m%d-%H%M%S"))
    moved = []
    for p in cached_profiles(team, bid):
        os.makedirs(keep, exist_ok=True)
        target = os.path.join(keep, p.name)
        shutil.move(os.fspath(p), target)
        moved.append((os.fspath(p), target))
    if moved:
        emit("notice", step="provision", message=f"Asking Apple for a fresh 7-day profile ({len(moved)} cached profile(s) set aside).")
    return moved


def restore_profiles(moved):
    for original, kept in moved:
        if not os.path.exists(original) and os.path.exists(kept):
            shutil.move(kept, original)


def resign_existing(st, work, device=None, fresh_profile=False):
    moved = retire_cached_profiles(st["team"], st["bundle_id"], work) if fresh_profile and not st.get("paid") and not Runner.dry else []
    signed = False
    try:
        warmed = provision(st["team"], st["bundle_id"], st.get("paid", False), work, device)
        if moved and not cached_profiles(st["team"], st["bundle_id"]):
            restore_profiles(moved)  # Xcode fetched nothing: sign with what there was
        sign_app(st["app"], st["team"], st["bundle_id"], st.get("paid", False), work, device,
                 compile_assets=False, warmed=warmed)
        signed = True
    finally:
        # any failure or cancel before the app was signed (provisioning, signing, the keychain, Ctrl-C) puts the
        # set-aside profiles back, so a still-valid one is never left only in the work folder
        if not signed:
            restore_profiles(moved)


def ensure_device_profile(st, work, device):
    if Runner.dry:
        return
    team = st.get("team")
    if not team:
        raise StepError("No signing team recorded: build with your selected team first.")
    with open(os.path.join(ROOT, "ios", "SheikahVision-paid.entitlements" if st.get("paid") else "SheikahVision.entitlements"), "rb") as f:
        wanted = plistlib.load(f)
    identities = run("security", "find-identity", "-v", "-p", "codesigning")[1]
    profile = decode_profile(os.path.join(st["app"], "embedded.mobileprovision"))
    if not profile_identity(profile, team, st["bundle_id"], wanted, device, identities):
        emit("notice", step="sign", message="Registering the selected headset and re-signing the existing app; no game recompilation.")
        resign_existing(st, work, device)


def cmd_install(args):
    try:
        work, st = load_build_state(args)
        if not Runner.dry and not os.path.isdir(st["app"]):
            raise StepError(f"{st['app']} is missing: run 'sv build'")
        device = resolve_device(args)
        ensure_device_profile(st, work, device)
        install_app(st["app"], device)
        if not Runner.dry:
            st["selected_device"] = device
            st.setdefault("installed_devices", {})[device] = time.time()
            save_json(state_path(work), st)
    except StepError as e:
        emit("error", code=getattr(e, "code", "E_STEP"), message=str(e))
        emit("done", step="install", ok=False)
        return 1
    emit("step", step="install", status="ok", detail="installed; open the app once on the headset before push-game")
    emit("done", step="install", ok=True, dry_run=Runner.dry)
    return 0


def make_zip(work, gj):
    """Store-only zip (zip -0) of game/ beside it; reused when it matches the current import."""
    zp = os.path.join(work, "SheikahVision-game.zip")
    side = zp + ".json"
    snapshot = hashlib.sha256()
    for folder, dirs, names in os.walk(os.path.join(work, "game")):
        dirs.sort()
        for name in sorted(names):
            path = os.path.join(folder, name)
            st = os.lstat(path)
            snapshot.update(f"{os.path.relpath(path, work)}:{st.st_size}:{st.st_mtime_ns}".encode())
    want = {"imported_at": gj.get("imported_at"), "rpx_sha256": gj.get("rpx_sha256"),
            "file_count": gj.get("file_count"), "snapshot": snapshot.hexdigest()}
    if os.path.isfile(zp) and load_json(side) == want and os.path.getsize(zp) > 0:
        emit("step", step="zip", status="ok", detail=f"reusing the up-to-date {zp} ({os.path.getsize(zp) / 1e9:.2f} GB)")
        return zp
    tmp = zp + ".part"
    if Runner.dry:
        emit("command", step="zip", cmd=["sv", "store-zip", os.path.join(work, "game"), zp], dry_run=True)
        return zp
    info = gi.validate([os.path.join(work, "game")])
    if info["rpx_sha256"] != gj.get("rpx_sha256"):
        raise StepError("the imported game changed: import it again before transferring")
    total = gj.get("file_count") or 1
    count = 0
    started = time.monotonic()
    with zipfile.ZipFile(tmp, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as z:
        for top in gi.GAME_DIRS:
            for folder, dirs, names in os.walk(os.path.join(work, "game", top)):
                dirs.sort()
                for name in sorted(names):
                    path = os.path.join(folder, name)
                    if os.path.islink(path):
                        raise StepError("the imported game contains links: import it again")
                    z.write(path, os.path.relpath(path, os.path.join(work, "game")))
                    count += 1
                    if count % 128 == 0:
                        emit("progress", step="zip", done=count, total=total, percent=100 * count / total)
    os.replace(tmp, zp)
    save_json(side, want)
    emit("step", step="zip", status="ok", elapsed_seconds=round(time.monotonic()-started, 3))
    return zp


class TransferProgress:
    """devicectl copies the 13 GB zip in one call and reports nothing until it ends: the installer's bar sat still for
    minutes and looked stuck (user 2026-11-18). While it runs, the zip's size on the headset is read every few seconds
    (a read-only devicectl call) and sent as progress: percent, bytes, total_bytes, bytes_per_second, eta_seconds."""
    INTERVAL = 4.0

    def __init__(self, device, bid, zp, started):
        self.arguments = ["device", "info", "files", "--device", device, "--domain-type", "appDataContainer",
                          "--domain-identifier", bid, "--subdirectory", "Documents/Import", "--no-recurse"]
        self.name = os.path.basename(zp)
        self.total = os.path.getsize(zp) if os.path.isfile(zp) else 0
        self.started = started
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def __enter__(self):
        if not Runner.dry and self.total:
            self.thread.start()
        return self

    def __exit__(self, *exc):
        self.stop.set()
        if self.thread.is_alive():
            self.thread.join(timeout=10)
        return False

    def size_on_headset(self):
        for f in device_json(self.arguments, "progress").get("files", []):
            if f.get("name") == self.name:
                return int((f.get("metadata") or {}).get("size") or 0)
        return None

    def run(self):
        while not self.stop.wait(self.INTERVAL):
            try:
                done = self.size_on_headset()
            except Exception:  # a slow or failed read only skips this sample; the copy itself is untouched
                continue
            if done is None or self.stop.is_set():
                continue
            done = min(done, self.total)
            elapsed = max(time.monotonic() - self.started, .001)
            speed = done / elapsed
            event = dict(step="push", percent=round(done * 100 / self.total, 1), bytes=done, total_bytes=self.total,
                         bytes_per_second=speed)
            if speed > 0:
                event["eta_seconds"] = int((self.total - done) / speed)
            emit("progress", **event)


def cmd_push_game(args):
    try:
        work, st = load_build_state(args, need_app=False)
        gj = load_json(os.path.join(work, "game.json"))
        if not gj and not Runner.dry:
            raise StepError("no imported game: run 'sv import <path>' first")
        device = resolve_device(args)
        bid = st["bundle_id"]
        # read-only preflight: the app must be installed and have been opened once (its Documents folder exists)
        apps = device_json(["device", "info", "apps", "--device", device, "--bundle-id", bid])
        if not Runner.dry and not any(a.get("bundleIdentifier") == bid for a in apps.get("apps", [])):
            raise StepError(f"{bid} is not installed on the headset: run 'sv install' first")
        try:
            device_json(["device", "info", "files", "--device", device, "--domain-type", "appDataContainer",
                         "--domain-identifier", bid, "--subdirectory", "Documents", "--no-recurse"])
        except StepError:
            raise StepError("the app has no accessible Documents folder: unlock and connect the headset, open SheikahVision once, then try again")
        zp = make_zip(work, gj) if (gj or not Runner.dry) else os.path.join(work, "SheikahVision-game.zip")
        cmd = ["xcrun", "devicectl", "device", "copy", "to", "--device", device, "--domain-type", "appDataContainer",
               "--domain-identifier", bid, "--source", zp, "--destination", "Documents/Import/" + os.path.basename(zp)]
        for attempt in range(1, RETRIES + 1):
            try:
                emit("step", step="push", status="running", detail=f"attempt {attempt}/{RETRIES}")
                started = time.monotonic()
                with TransferProgress(device, bid, zp, started):
                    device_json(cmd[2:], "push")
                elapsed = max(time.monotonic() - started, .001)
                emit("progress", step="push", percent=100, bytes=os.path.getsize(zp) if not Runner.dry else 0,
                     bytes_per_second=os.path.getsize(zp) / elapsed if not Runner.dry else 0)
                break
            except StepError as e:
                if attempt == RETRIES:
                    raise StepError(f"copy failed {RETRIES} times; run push-game again to retransmit the entire zip (no byte-level resume): {e}")
                emit("progress", step="push", line=f"failed ({e}); retrying in {RETRY_PAUSE} s")
                time.sleep(RETRY_PAUSE)
    except StepError as e:
        emit("error", code=getattr(e, "code", "E_STEP"), message=str(e))
        emit("done", step="push-game", ok=False)
        return 1
    emit("step", step="push", status="ok", detail="copied; open the app on the headset and start the import")
    # the size that arrived, for the next readiness probe to tell a whole package from a cut-off one once the zip on
    # this Mac is removed below (waiting_package_complete)
    if not Runner.dry:
        try:
            with open(os.path.join(work, "push.json"), "w") as f:
                json.dump({"size": os.path.getsize(zp), "rpx_sha256": (gj or {}).get("rpx_sha256")}, f)
        except OSError:
            pass
    # the zip is only a transfer package (about 13 GB of real disk, not a clone): drop it once it is on the headset.
    # A failed run keeps it so a retry reuses it; a later transfer packs game/ again.
    if not Runner.dry:
        for path in (zp, zp + ".json"):
            try:
                os.remove(path)
            except OSError:
                pass
        emit("notice", step="push", message="Removed the transfer zip from this Mac; the game folder stays.")
    # The app was opened before the transfer (its Documents folder had to exist), so it saw an empty Import folder.
    # Restart it once: on launch it imports a package waiting in Documents/Import by itself. Best effort; the player
    # can always open the app or use Prepare Game > Check and import.
    relaunched = False
    try:
        device_json(["device", "process", "launch", "--terminate-existing", "--device", device, bid], "launch")
        relaunched = True
        emit("step", step="push", status="ok", detail="restarted SheikahVision on the headset; it imports the game by itself")
    except StepError as e:
        emit("notice", step="push", message=f"Could not restart the app on the headset ({str(e)[-120:]}); open SheikahVision there to start the import.")
    emit("done", step="push-game", ok=True, relaunched=relaunched, dry_run=Runner.dry)
    return 0


def cmd_renew(args):
    try:
        work, st = load_build_state(args)
        if not st.get("team") and not args.team and not Runner.dry:
            raise StepError("no recorded team: run 'sv build' first or pass --team")
        team = args.team or st.get("team") or "DRYRUN0000"
        if st.get("team") and team != st["team"]:
            raise StepError("Renew must use the original app team; build a separate app to change teams.")
        paid = st.get("paid", False) if not (args.paid or args.free) else bool(args.paid)
        app = st["app"]
        if not Runner.dry and not os.path.isdir(app):
            raise StepError("the built app is missing: run build before renewing")
        st.update(team=team, paid=paid)
        device = args.device if args.no_install else resolve_device(args)
        before = profile_expiry(app) if not Runner.dry else None
        resign_existing(st, work, device, fresh_profile=True)
        if not Runner.dry:
            st["expires"] = profile_expiry(app)
            if before and st["expires"] and st["expires"] <= before:
                emit("notice", step="renew", message=f"The signature still expires {st['expires']}: Apple did not issue a newer profile.")
            st["renewed_at"] = time.strftime("%Y-%m-%dT%H:%M:%S")
            save_json(state_path(work), st)
        if not args.no_install:
            install_app(st["app"], device, "install")
            if not Runner.dry:
                st["selected_device"] = device
                st.setdefault("installed_devices", {})[device] = time.time()
                save_json(state_path(work), st)
    except StepError as e:
        emit("error", code=getattr(e, "code", "E_STEP"), message=str(e))
        emit("done", step="renew", ok=False)
        return 1
    emit("done", step="renew", ok=True, dry_run=Runner.dry, expires=st.get("expires"))
    return 0


COLLECT = ["Documents/import-log.txt", "Documents/game-receipt.json", "Library/Application Support/SheikahVision/runtime.log",
           "Library/Application Support/SheikahVision/runtime-prev.log"]  # runtime/src/main.cpp writes runtime.log and runtime-prev.log


def cmd_collect(args):
    if getattr(args, "local_only", False):
        emit("notice", step="collect", message="Skipping headset logs: no session-verified device; local diagnostics remain available.")
        emit("done", step="collect", ok=True, files=[], skipped=True, dry_run=Runner.dry)
        return 0
    try:
        work, st = load_build_state(args, need_app=False)
        device = resolve_device(args)
    except StepError as e:
        emit("notice", step="collect", message=f"Skipping headset logs: {e}")
        emit("done", step="collect", ok=True, files=[], skipped=True, dry_run=Runner.dry)
        return 0
    out = os.path.join(work, "feedback", time.strftime("%Y%m%d-%H%M%S"))
    if not Runner.dry:
        os.makedirs(out, exist_ok=True)
    got = []
    for src in COLLECT:
        dst = os.path.join(out, os.path.basename(src))
        cmd = ["xcrun", "devicectl", "device", "copy", "from", "--device", device, "--domain-type", "appDataContainer",
               "--domain-identifier", st["bundle_id"], "--source", src, "--destination", dst]
        try:
            sh(cmd, "collect", capture=True)
            if Runner.dry or os.path.exists(dst):
                got.append(os.path.basename(src))
        except StepError as e:  # a file that does not exist yet is normal
            emit("progress", step="collect", line=f"{src}: not available ({str(e)[-80:]})")
    emit("step", step="collect", status="ok" if got else "missing", detail=f"{', '.join(got) or 'nothing'} in {out}")
    emit("done", step="collect", ok=bool(got), dir=out, files=got, dry_run=Runner.dry)
    return 0 if got else 1


# ---- CLI ----

def parser():
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--json", action="store_true", help="one JSON event per line on stdout")
    common.add_argument("--work", default=default_work(), help="work directory (default %(default)s; env SV_WORK)")
    common.add_argument("--dry-run", action="store_true", help="print the commands, change nothing")
    common.add_argument("--device", help="device UDID (default: the single paired Vision Pro)")
    common.add_argument("--team", help="Apple team ID to sign for (default: the only available team; choose explicitly when several exist)")
    common.add_argument("--bundle-id", help="override the bundle id (default %s.t<team>)" % BUNDLE_BASE)
    common.add_argument("--runtime", help="optional public runtime kit; incompatible kits fall back to source")
    common.add_argument("--paid", action="store_true", help="sign with the paid-team entitlements")
    common.add_argument("--free", action="store_true", help="force free-team entitlements")
    p = argparse.ArgumentParser(prog="sv", description="SheikahVision installer engine")
    sub = p.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("doctor", parents=[common], help="check this Mac and headset")
    d.add_argument("--probe", action="store_true", help="also connect to the headset to refresh Developer Mode state")
    i = sub.add_parser("import", parents=[common], help="validate and import the game")
    i.add_argument("paths", nargs="+", metavar="path", help="decrypted game folder(s) with code/content/meta, base first then update")
    i.add_argument("--validate-only", action="store_true", help="check the folders, copy nothing")
    for n, h in (("launch", "open the installed headset app"), ("documents", "check whether the app container is ready (read-only)"), ("status", "validate and restore local progress"), ("devices", "list paired headsets"), ("account", "list teams and warm up the selected signing key"), ("build", "recompile (cached), build and sign the app"), ("install", "install the built app"),
                 ("push-game", "send the game zip to the headset"), ("renew", "re-sign and reinstall"),
                 ("collect", "fetch logs from the headset")):
        s = sub.add_parser(n, parents=[common], help=h)
        if n == "collect":
            s.add_argument("--local-only", action="store_true", help="skip headset logs when setup has not verified a device")
        if n == "devices":
            s.add_argument("--probe", action="store_true", help="confirm available paired devices using read-only details queries")
        if n == "account":
            s.add_argument("--list-only", action="store_true", help="refresh teams without signing")
        if n == "renew":
            s.add_argument("--no-install", action="store_true", help="re-sign only")
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    Out.json = args.json
    Runner.dry = args.dry_run
    fix_path()
    use_xcode()
    try:
        return {"doctor": doctor, "import": cmd_import, "build": cmd_build, "install": cmd_install,
            "push-game": cmd_push_game, "renew": cmd_renew, "collect": cmd_collect,
            "account": cmd_account, "devices": cmd_devices, "status": cmd_status,
            "launch": cmd_launch, "documents": cmd_documents}[args.cmd](args)
    except (StepError, gi.GameError, OSError, subprocess.SubprocessError) as e:
        emit("error", code=getattr(e, "code", "E_COMMAND"), message=str(e), next="retry")
        emit("done", step=args.cmd, ok=False)
        return 1
    except KeyboardInterrupt:
        emit("notice", message="Cancelled. Run the same command again to reuse completed work.")
        emit("done", step=args.cmd, ok=False, cancelled=True)
        return 130


def install_signal_handlers():
    def interrupted(_signum, _frame):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, interrupted)


if __name__ == "__main__":
    install_signal_handlers()
    sys.exit(main())
