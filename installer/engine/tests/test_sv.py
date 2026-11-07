"""Tests for the installer engine's validation and event contract.
Run: python3 -m unittest discover -s installer/engine/tests
Fixtures are tiny fake game folders; the real v208 hash is patched in where a fake executable has to pass."""
import contextlib
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
import zipfile
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gameimport as gi  # noqa: E402
import sv  # noqa: E402

META = ('<?xml version="1.0" encoding="utf-8"?><menu type="complex" access="777">'
        '<title_version type="unsignedInt" length="4">{ver}</title_version>'
        '<title_id type="hexBinary" length="8">{tid}</title_id></menu>')
RPX = b"fake-rpx-v208"
RPX_SHA = hashlib.sha256(RPX).hexdigest()
REAL_SHA = "ba58da5b95ce929e005d058ceb08b9b2788d1ab2bbc8a6c189bbadca0bb34d30"


def write(path, data=b"x"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


def make_game(root, low="101C9300", high="00050000", ver=208, langs=("JPja",), rpx=RPX, rpx_present=True, extra=None):
    write(os.path.join(root, "meta", "meta.xml"), META.format(ver=ver, tid=high + low))
    write(os.path.join(root, "code", "app.xml"), META.format(ver=ver, tid=high + low))
    write(os.path.join(root, "code", "cos.xml"), "<cos/>")
    if rpx_present:
        write(os.path.join(root, "code", "U-King.rpx"), rpx)
    for lang in langs:
        write(os.path.join(root, "content", "Pack", f"Bootup_{lang}.pack"), lang)
    os.makedirs(os.path.join(root, "content"), exist_ok=True)
    for rel, data in (extra or {}).items():
        write(os.path.join(root, rel), data)
    return root


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="svtest-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        patch = mock.patch.object(gi, "V208_RPX_SHA256", RPX_SHA)
        patch.start()
        self.addCleanup(patch.stop)
        self.events = []

    def emit(self, event, **kw):
        self.events.append((event, kw))

    def p(self, *a):
        return os.path.join(self.tmp, *a)

    def do_import(self, paths, **kw):
        return gi.import_game(paths, self.p("work"), self.emit, clone=False, **kw)

    def run_sv(self, *argv):
        buf, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
            rc = sv.main(list(argv))
        sv.Out.json = False
        sv.Runner.dry = False
        return rc, buf.getvalue(), err.getvalue()


class RegionTests(Base):
    def test_three_regions_and_their_languages(self):
        cases = {"JPN": ("101C9300", ["JPja"]), "USA": ("101C9400", ["USen", "USfr"]),
                 "EUR": ("101C9500", ["EUen", "EUde", "EUru"])}
        for region, (low, langs) in cases.items():
            g = make_game(self.p(region), low=low, langs=langs)
            info = gi.validate([g])
            self.assertEqual(info["region"], region)
            self.assertEqual(info["title_id"], "00050000" + low)
            self.assertEqual(info["languages"], langs)
            self.assertEqual(info["title_version"], 208)

    def test_not_botw(self):
        g = make_game(self.p("g"), low="10123400")
        with self.assertRaises(gi.GameError) as c:
            gi.validate([g])
        self.assertEqual(c.exception.code, "E_NOT_BOTW")

    def test_language_pack_must_match_region(self):
        g = make_game(self.p("g"), low="101C9400", langs=["EUen"])
        with self.assertRaises(gi.GameError) as c:
            gi.validate([g])
        self.assertEqual(c.exception.code, "E_LANG")
        g2 = make_game(self.p("g2"), langs=[])
        with self.assertRaises(gi.GameError) as c:
            gi.validate([g2])
        self.assertEqual(c.exception.code, "E_LANG")

    def test_bootup_graphics_pack_is_not_a_language(self):
        g = make_game(self.p("g"), extra={"content/Pack/Bootup_Graphics.pack": "gfx"})
        self.assertEqual(gi.validate([g])["languages"], ["JPja"])


class VersionAndHashTests(Base):
    def test_old_version_rejected(self):
        g = make_game(self.p("g"), ver=16)
        with self.assertRaises(gi.GameError) as c:
            gi.validate([g])
        self.assertEqual(c.exception.code, "E_VERSION")
        self.assertIn("208", c.exception.message)

    def test_rpx_hash_mismatch(self):
        g = make_game(self.p("g"), rpx=b"some other executable")
        with self.assertRaises(gi.GameError) as c:
            gi.validate([g])
        self.assertEqual(c.exception.code, "E_RPX_HASH")

    def test_real_constant_rejects_a_fake_rpx(self):
        g = make_game(self.p("g"))
        with mock.patch.object(gi, "V208_RPX_SHA256", REAL_SHA):
            with self.assertRaises(gi.GameError) as c:
                gi.validate([g])
        self.assertEqual(c.exception.code, "E_RPX_HASH")

    def test_missing_pieces(self):
        g = make_game(self.p("g"), rpx_present=False)
        with self.assertRaises(gi.GameError) as c:
            gi.validate([g])
        self.assertEqual(c.exception.code, "E_STRUCTURE")
        self.assertIn("code/U-King.rpx", c.exception.message)


class DlcAndMergeTests(Base):
    def test_dlc_is_ignored_with_a_notice(self):
        top = self.p("dump")
        make_game(os.path.join(top, "base"))
        dlc = os.path.join(top, "dlc")
        write(os.path.join(dlc, "meta", "meta.xml"), META.format(ver=80, tid="0005000C101C9300"))
        write(os.path.join(dlc, "content", "0010", "x.bin"), "dlc")
        s = self.do_import([top])
        self.assertEqual(s["dlc_ignored"], ["dlc"])
        self.assertTrue(any(e == "notice" and "DLC" in kw["message"] for e, kw in self.events))
        self.assertFalse(os.path.exists(self.p("work", "game", "content", "0010")))
        self.assertEqual(s["sources"], ["base"])

    def test_only_dlc_is_an_error(self):
        dlc = self.p("dlc")
        write(os.path.join(dlc, "meta", "meta.xml"), META.format(ver=80, tid="0005000C101C9400"))
        with self.assertRaises(gi.GameError) as c:
            self.do_import([dlc])
        self.assertEqual(c.exception.code, "E_NO_GAME")

    def make_pair(self, swapped=False):
        base = make_game(self.p("base"), ver=0, rpx=b"old-exe",
                         extra={"content/a.bin": "base-a", "content/only_base.bin": "keep"})
        upd = make_game(self.p("upd"), high="0005000E", ver=208, langs=[],
                        extra={"content/a.bin": "update-a", "content/new.bin": "n"})
        return [upd, base] if swapped else [base, upd]

    def test_update_over_base(self):
        s = self.do_import(self.make_pair())
        game = self.p("work", "game")
        self.assertEqual(open(os.path.join(game, "content", "a.bin")).read(), "update-a")
        self.assertEqual(open(os.path.join(game, "content", "only_base.bin")).read(), "keep")
        self.assertTrue(os.path.exists(os.path.join(game, "content", "new.bin")))
        self.assertEqual(open(os.path.join(game, "code", "U-King.rpx"), "rb").read(), RPX)
        self.assertEqual(s["title_version"], 208)
        self.assertEqual(s["languages"], ["JPja"])  # from the base's pack
        self.assertEqual(s["sources"], ["base", "upd"])

    def test_order_given_does_not_matter(self):
        self.do_import(self.make_pair(swapped=True))
        self.assertEqual(open(self.p("work", "game", "content", "a.bin")).read(), "update-a")

    def test_base_alone_is_not_208(self):
        base, _ = self.make_pair()
        with self.assertRaises(gi.GameError) as c:
            self.do_import([base])
        self.assertEqual(c.exception.code, "E_VERSION")

    def test_mixed_regions_rejected(self):
        a = make_game(self.p("a"))
        b = make_game(self.p("b"), low="101C9400", langs=["USen"])
        with self.assertRaises(gi.GameError) as c:
            self.do_import([a, b])
        self.assertEqual(c.exception.code, "E_REGION_MIX")


class ImportTests(Base):
    def test_import_writes_game_json_and_cleans_junk(self):
        src = make_game(self.p("src"), extra={"content/.DS_Store": "j", "content/._x": "j", "content/a/b.bin": "bb"})
        os.symlink("/etc/hosts", os.path.join(src, "content", "link"))
        s = self.do_import([src])
        j = json.load(open(self.p("work", "game.json")))
        for k in ("region", "languages", "size_bytes", "file_count", "title_id", "rpx_sha256"):
            self.assertIn(k, j)
        self.assertEqual(j["region"], "JPN")
        game = self.p("work", "game")
        self.assertFalse(os.path.exists(os.path.join(game, "content", ".DS_Store")))
        self.assertFalse(os.path.lexists(os.path.join(game, "content", "link")))
        n = sum(len(f) for _, _, f in os.walk(game))
        self.assertEqual(s["file_count"], n)
        self.assertTrue(os.path.exists(os.path.join(src, "content", ".DS_Store")))  # the source is untouched

    def test_reimport_replaces_and_leaves_no_staging(self):
        src = make_game(self.p("src"), extra={"content/a.bin": "1"})
        self.do_import([src])
        write(os.path.join(src, "content", "a.bin"), "2")
        self.do_import([src])
        self.assertEqual(open(self.p("work", "game", "content", "a.bin")).read(), "2")
        self.assertEqual(sorted(os.listdir(self.p("work"))), ["game", "game.json"])

    def test_failed_import_keeps_the_old_game(self):
        self.do_import([make_game(self.p("src"))])
        bad = make_game(self.p("bad"), rpx=b"nope")
        with self.assertRaises(gi.GameError):
            self.do_import([bad])
        self.assertTrue(os.path.exists(self.p("work", "game", "code", "U-King.rpx")))

    def test_validate_only_copies_nothing(self):
        s = self.do_import([make_game(self.p("src"))], validate_only=True)
        self.assertTrue(s["validated_only"])
        self.assertFalse(os.path.exists(self.p("work")))

    def test_cemu_title_folder_combines_base_and_update_and_skips_other_games(self):
        title = self.p("mlc01", "usr", "title")
        make_game(os.path.join(title, "00050000", "101c9300"), ver=0, rpx=b"old")
        make_game(os.path.join(title, "0005000e", "101c9300"), high="0005000E", ver=208)
        make_game(os.path.join(title, "00050000", "10143500"), low="10143500")  # another game
        s = self.do_import([title])
        self.assertEqual((s["region"], s["title_version"]), ("JPN", 208))
        self.assertEqual(s["sources"], ["101c9300", "101c9300"])
        self.assertTrue(any("other games" in kw.get("message", "") for e, kw in self.events if e == "notice"))

    def test_chinese_patch_is_recognised_and_plain_copies_are_not(self):
        plain = self.do_import([make_game(self.p("plain"))], validate_only=True)
        self.assertIsNone(plain["text_patch"])
        g = make_game(self.p("zh"))
        with mock.patch.dict(gi.KNOWN_TEXT, {gi.sha256_file(os.path.join(g, "content", "Pack", "Bootup_JPja.pack")): "zh-Hans"}):
            self.assertEqual(self.do_import([g], validate_only=True)["text_patch"], "zh-Hans")

    def test_installer_version_comes_from_the_version_file(self):
        with open(os.path.join(sv.ROOT, "VERSION")) as f:
            self.assertEqual(sv.VERSION, f.read().strip())

    def test_status_survives_a_deleted_game_copy(self):
        self.do_import([make_game(self.p("src"))])
        shutil.rmtree(self.p("work", "game"))
        sv.save_json(self.p("work", "build.json"), {"team": "TEAMA00001", "app": "/fake/app", "installer_version": "0.1.0"})
        rc, out, _ = self.run_sv("status", "--work", self.p("work"), "--json")
        self.assertEqual(rc, 0)
        done = [json.loads(l) for l in out.splitlines()][-1]
        self.assertTrue(done["ok"]); self.assertTrue(done["game_missing"]); self.assertIsNone(done["game"])

    def test_found_in_a_subfolder(self):
        make_game(self.p("outer", "inner", "game"))
        self.assertEqual(self.do_import([self.p("outer")])["region"], "JPN")

    def test_disc_image_file_is_rejected(self):
        img = self.p("g.wux")
        write(img, "x")
        with self.assertRaises(gi.GameError) as c:
            self.do_import([img])
        self.assertEqual(c.exception.code, "E_NOT_FOLDER")
        self.assertIn("code, content and meta", c.exception.message)
        self.assertFalse(os.path.exists(self.p("work")))

    def test_wua_and_disc_images_get_a_specific_message(self):
        for name, words in (("g.wua", ".wua archive"), ("g.WUX", "disc image")):
            write(self.p(name), "x")
            with self.assertRaises(gi.GameError) as c:
                self.do_import([self.p(name)])
            self.assertEqual(c.exception.code, "E_NOT_FOLDER")
            self.assertIn(words, c.exception.message)

    def test_key_options_are_gone(self):
        with self.assertRaises(SystemExit):
            with contextlib.redirect_stderr(io.StringIO()):
                sv.main(["import", self.p("src"), "--key", "00" * 16, "--json"])


class EventTests(Base):
    def test_json_import_events(self):
        src = make_game(self.p("src"))
        rc, out, _ = self.run_sv("import", src, "--json", "--work", self.p("w"))
        self.assertEqual(rc, 0)
        evs = [json.loads(line) for line in out.splitlines()]
        self.assertTrue(all("event" in e for e in evs))
        self.assertEqual(evs[-1]["event"], "done")
        self.assertTrue(evs[-1]["ok"])
        self.assertEqual(evs[-1]["game"]["region"], "JPN")

    def test_json_error_event_has_a_code(self):
        src = make_game(self.p("src"), ver=16)
        rc, out, _ = self.run_sv("import", src, "--json", "--work", self.p("w"))
        self.assertEqual(rc, 1)
        evs = [json.loads(line) for line in out.splitlines()]
        err = next(e for e in evs if e["event"] == "error")
        self.assertEqual(err["code"], "E_VERSION")
        self.assertFalse(evs[-1]["ok"])

    def test_human_output_is_not_json(self):
        src = make_game(self.p("src"))
        rc, out, _ = self.run_sv("import", src, "--work", self.p("w"))
        self.assertEqual(rc, 0)
        self.assertNotIn('{"event"', out)

    def test_check_event_shape(self):
        sv.Out.json = True
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            sv.emit("check", check="x", title="X", status="warn", detail="d", fix="f")
        sv.Out.json = False
        e = json.loads(buf.getvalue())
        self.assertEqual((e["event"], e["status"]), ("check", "warn"))


class DeviceSafetyTests(Base):
    def test_delete_flags_are_refused(self):
        for bad in (["--remove-existing-content", "true"], ["-r", "true"], ["uninstall"]):
            with self.assertRaises(sv.StepError):
                sv.guard(["xcrun", "devicectl", "device", "copy", "to"] + bad)

    def test_dry_run_prints_commands_and_runs_nothing(self):
        make_game(self.p("src"))
        rc, _, _ = self.run_sv("import", self.p("src"), "--work", self.p("w"))
        self.assertEqual(rc, 0)
        rc, out, _ = self.run_sv("push-game", "--work", self.p("w"), "--device", "UDID1", "--bundle-id", "com.x.y",
                                 "--dry-run", "--json")
        self.assertEqual(rc, 0)
        cmds = [json.loads(line) for line in out.splitlines() if '"command"' in line]
        copy = next(c for c in cmds if "copy" in c["cmd"])
        self.assertIn("appDataContainer", copy["cmd"])
        self.assertNotIn("--remove-existing-content", copy["cmd"])
        self.assertFalse(os.path.exists(self.p("w", "SheikahVision-game.zip")))

    def test_zip_is_store_only_and_reused(self):
        make_game(self.p("src"))
        self.run_sv("import", self.p("src"), "--work", self.p("w"))
        gj = sv.load_json(self.p("w", "game.json"))
        sv.Out.json = True
        with contextlib.redirect_stdout(io.StringIO()):
            z = sv.make_zip(self.p("w"), gj)
            m1 = os.path.getmtime(z)
            z2 = sv.make_zip(self.p("w"), gj)
        sv.Out.json = False
        self.assertEqual((z, m1), (z2, os.path.getmtime(z2)))
        with zipfile.ZipFile(z) as zf:
            self.assertTrue(all(i.compress_type == zipfile.ZIP_STORED for i in zf.infolist() if not i.is_dir()))
            self.assertIn("code/U-King.rpx", zf.namelist())

    def test_bundle_id_has_team_suffix(self):
        self.assertEqual(sv.bundle_id_for("TEAMA00001"), "com.botw.sheikahvision.tteama00001")

    def test_recompiler_version_is_stable(self):
        self.assertEqual(sv.recompiler_version(), sv.recompiler_version())


if __name__ == "__main__":
    unittest.main()
