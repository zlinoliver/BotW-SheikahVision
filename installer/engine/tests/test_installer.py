"""Installer orchestration: synthetic files and devicectl JSON fixtures; no device access."""
import contextlib
import io
import json
import os
import plistlib
import tempfile
import sys
import unittest
from unittest import mock

from test_sv import Base, make_game, write, RPX_SHA
import sv


class EngineTests(Base):
    def setUp(self):
        super().setUp()
        self.addCleanup(setattr, sv.Runner, 'dry', False)

    def test_cache_key_includes_compiler_source_and_rpx(self):
        with mock.patch.object(sv, 'run', return_value=(0, 'clang-1', '')), mock.patch.object(sv, 'source_version', return_value='src1'):
            a = sv.compiled_key('rpx1')
            self.assertEqual(a, sv.compiled_key('rpx1'))
            self.assertNotEqual(a, sv.compiled_key('rpx2'))
        with mock.patch.object(sv, 'run', return_value=(0, 'clang-2', '')), mock.patch.object(sv, 'source_version', return_value='src1'):
            self.assertNotEqual(a, sv.compiled_key('rpx1'))
        with mock.patch.object(sv, 'run', return_value=(0, 'clang-1', '')), mock.patch.object(sv, 'source_version', return_value='src2'):
            self.assertNotEqual(a, sv.compiled_key('rpx1'))

    def test_generated_cache_hit_skips_recompile(self):
        self.do_import([make_game(self.p('src'))])
        cache, _, _ = sv.cache_dir(self.p('work'), RPX_SHA)
        os.makedirs(cache)
        write(os.path.join(cache, 'fn_0000.cpp'), '// fake generated TU')
        sv.save_json(os.path.join(cache, '.complete'), {'rpx_sha256': RPX_SHA, 'recompiler_version': sv.recompiler_version()[0]})
        with mock.patch.object(sv, 'sh') as sh, mock.patch.object(sv, 'warm_up_signing'):
            rc, _, _ = self.run_sv('build', '--work', self.p('work'), '--team', 'TEAMA00001', '--dry-run')
        self.assertEqual(rc, 0)
        self.assertFalse(any('tools/recompile.py' in str(c) for c in sh.call_args_list))

    def test_free_capabilities_default_even_for_paid_team(self):
        args = sv.parser().parse_args(['build', '--team', 'TEAMA00001'])
        with mock.patch.object(sv, 'xcode_teams', return_value=[('TEAMA00001', 'Test', True)]):
            self.assertEqual(sv.resolve_team(args), ('TEAMA00001', False))
        with open(os.path.join(sv.ROOT, 'ios/SheikahVision.entitlements'), 'rb') as f:
            self.assertEqual(set(plistlib.load(f)) - {'get-task-allow'}, {'com.apple.developer.kernel.increased-memory-limit'})
        self.assertEqual(sv.tool_env('TEAMA00001', 'com.x.y', False, self.tmp)['BOTW_PAID'], '0')

    def test_bundle_id_rejects_invalid_teams(self):
        for team in ('', '../bad', 'A;echo x', '123'):
            with self.assertRaisesRegex(sv.StepError, 'team ID'):
                sv.bundle_id_for(team)
        self.assertNotEqual(sv.bundle_id_for('TEAMA00001'), sv.bundle_id_for('ABCDE12345'))

    def test_dangerous_flags_rejected(self):
        for flag in ('--remove-existing-content', '--remove-existing-content=true', '-r', '-r=true', 'uninstall', 'delete', 'erase', 'reset'):
            with self.assertRaisesRegex(sv.StepError, 'never deletes'):
                sv.guard(['xcrun', 'devicectl', 'device', flag])

    def test_device_json_ignores_human_stdout(self):
        fixture = os.path.join(os.path.dirname(__file__), 'fixtures', 'apps.json')
        def fake(cmd, *a, **kw):
            with open(fixture) as f:
                data = json.load(f)
            sv.save_json(cmd[-1], data)
            return 'human output deliberately names a different app'
        with mock.patch.object(sv, 'sh', side_effect=fake):
            result = sv.device_json(['device', 'info', 'apps'])
        self.assertEqual(result['apps'][0]['bundleIdentifier'], 'com.example.test')

    def test_device_json_malformed_or_failed_is_not_success(self):
        for data in ({}, {'info': {'outcome': 'failed'}, 'result': {}}, {'info': {'outcome': 'success'}, 'result': []}):
            with mock.patch.object(sv, 'sh', side_effect=lambda cmd, *a, **kw: sv.save_json(cmd[-1], data)):
                with self.assertRaisesRegex(sv.StepError, 'usable device information'):
                    sv.device_json(['device', 'info', 'files'])

    def push_setup(self):
        work = self.p('work')
        self.do_import([make_game(self.p('src'))])
        sv.save_json(os.path.join(work, 'build.json'), {'bundle_id': 'com.example.test'})
        return sv.parser().parse_args(['push-game', '--work', work, '--device', 'FAKE'])

    def test_push_retries_five_times_ten_seconds_reuses_zip_after_failure_and_drops_it_after_success(self):
        args = self.push_setup()
        copies = []
        fail_until = [10]
        def device(arguments, step='preflight'):
            if 'apps' in arguments:
                return {'apps': [{'bundleIdentifier': 'com.example.test'}]}
            if step == 'push':
                copies.append(arguments)
                if len(copies) < fail_until[0]:
                    raise sv.StepError('disconnected')
            return {}
        zip_path = self.p('work', 'SheikahVision-game.zip')
        with mock.patch.object(sv, 'device_json', side_effect=device), mock.patch.object(sv.time, 'sleep') as sleep, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sv.cmd_push_game(args), 1)          # five failed attempts: the zip stays for a retry
            stamp = os.stat(zip_path).st_mtime_ns
            fail_until[0] = 7
            self.assertEqual(sv.cmd_push_game(args), 0)          # the retry reuses it, then removes it
            self.assertFalse(os.path.exists(zip_path))
            self.assertFalse(os.path.exists(zip_path + '.json'))
            self.assertTrue(os.path.isdir(self.p('work', 'game')))
        self.assertEqual(sleep.call_args_list, [mock.call(10)] * 5)
        self.assertEqual(len(copies), 7)
        self.assertTrue(all(c == copies[0] for c in copies))
        self.assertIn('Documents/Import/SheikahVision-game.zip', copies[0])
        self.assertGreater(stamp, 0)

    def test_devicectl_gets_the_coredevice_identifier_instead_of_the_udid(self):
        sv._COREDEVICE_IDS.clear()
        ident = 'A1B2C3D4-0000-4000-8000-000000000001'
        with mock.patch.object(sv, 'headsets', return_value=[{'udid': '00009999-0001234', 'id': ident}]):
            cmd = sv.with_coredevice_ids(['xcrun', 'devicectl', 'device', 'install', 'app', '--device', '00009999-0001234', '/x.app'])
            self.assertEqual(cmd[6], ident)
            self.assertEqual(sv.coredevice_target(ident), ident)                     # already an identifier
            self.assertEqual(sv.coredevice_target('UNKNOWN-UDID'), 'UNKNOWN-UDID')   # unknown: unchanged
        other = ['xcodebuild', '-destination', 'id=00009999-0001234']                 # provisioning keeps the UDID
        self.assertEqual(sv.with_coredevice_ids(other), other)
        sv._COREDEVICE_IDS.clear()

    def test_status_says_when_the_app_was_built_from_other_source(self):
        work = self.p('w'); os.makedirs(work)
        sv.save_json(os.path.join(work, 'build.json'), {'team': 'TEAMA00001', 'source_version': 'old', 'source_commit': 'aaaa1111'})
        with mock.patch.object(sv, 'source_version', return_value='new'), mock.patch.object(sv, 'source_commit', return_value='bbbb2222'):
            rc, out, _ = self.run_sv('status', '--work', work, '--json')
        done = [json.loads(l) for l in out.splitlines()][-1]
        self.assertEqual(rc, 0)
        self.assertTrue(done['source_changed'])
        self.assertEqual((done['built_commit'], done['source_commit']), ('aaaa1111', 'bbbb2222'))

    def test_bundled_source_commit_file_wins_over_git(self):
        with mock.patch.object(sv, 'ROOT', self.tmp):
            write(os.path.join(self.tmp, 'source-commit'), '1234abcd\n')
            self.assertEqual(sv.source_commit(), '1234abcd')

    def test_successful_push_restarts_the_app_once_and_tolerates_a_failed_restart(self):
        args = self.push_setup()
        launches = []
        def device(arguments, step='preflight', fail_launch=[False]):
            if 'apps' in arguments:
                return {'apps': [{'bundleIdentifier': 'com.example.test'}]}
            if 'launch' in arguments:
                launches.append(arguments)
                if fail_launch[0]:
                    raise sv.StepError('locked')
            return {}
        out = io.StringIO()
        with mock.patch.object(sv, 'device_json', side_effect=device), contextlib.redirect_stdout(out):
            self.assertEqual(sv.cmd_push_game(args), 0)
        self.assertEqual(launches, [['device', 'process', 'launch', '--terminate-existing', '--device', 'FAKE', 'com.example.test']])
        sv.guard(['xcrun', 'devicectl'] + launches[0])            # restarting is not a deleting option
        self.do_import([make_game(self.p('src'))])                  # a new zip for the second transfer
        def failing(arguments, step='preflight'):
            if 'launch' in arguments:
                raise sv.StepError('locked')
            return {'apps': [{'bundleIdentifier': 'com.example.test'}]} if 'apps' in arguments else {}
        with mock.patch.object(sv, 'device_json', side_effect=failing), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sv.cmd_push_game(args), 0)

    def test_push_preflight_missing_app_or_documents(self):
        args = self.push_setup()
        for installed in (False, True):
            def device(arguments, step='preflight'):
                if 'apps' in arguments:
                    return {'apps': [{'bundleIdentifier': 'com.example.test'}] if installed else []}
                raise sv.StepError('no Documents')
            with mock.patch.object(sv, 'device_json', side_effect=device), contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(out):
                self.assertEqual(sv.cmd_push_game(args), 1)
            self.assertIn('Documents' if installed else 'not installed', out.getvalue())

    def test_import_dry_run_never_copies(self):
        rc, out, _ = self.run_sv('import', '/missing/folder', '--work', self.p('absent'), '--dry-run', '--json')
        self.assertEqual(rc, 0)
        self.assertFalse(os.path.exists(self.p('absent')))
        self.assertTrue(json.loads(out.splitlines()[-1])['dry_run'])

    def test_renew_only_provisions_signs_and_installs(self):
        work = self.p('work')
        os.makedirs(work)
        sv.save_json(os.path.join(work, 'build.json'), {'team': 'TEAMA00001', 'bundle_id': 'com.example.test', 'app': '/fake/test.app'})
        with mock.patch.object(sv, 'sh') as sh, mock.patch.object(sv, 'warm_up_signing'):
            rc, out, _ = self.run_sv('renew', '--work', work, '--dry-run', '--device', 'FAKE')
        self.assertEqual(rc, 0)
        self.assertFalse(any('recompile.py' in str(c) or '--build' in str(c) or 'copy' in str(c) for c in sh.call_args_list))
        self.assertIn('install', str(sh.call_args_list))

class CacheAndKitTests(Base):
    def test_cmake_cache_from_another_source_folder_is_dropped(self):
        build, old, new = self.p('build'), self.p('old-src'), self.p('new-src')
        os.makedirs(old); os.makedirs(new)
        write(os.path.join(build, 'CMakeCache.txt'), 'CMAKE_HOME_DIRECTORY:INTERNAL=' + old + '\n')
        write(os.path.join(build, 'CMakeFiles', 'x'), '')
        write(os.path.join(build, 'guest.o'), 'kept')
        with contextlib.redirect_stdout(io.StringIO()):
            sv.forget_moved_cmake_cache(build, old)
            self.assertTrue(os.path.exists(os.path.join(build, 'CMakeCache.txt')))  # same source: untouched
            sv.forget_moved_cmake_cache(build, new)
        self.assertFalse(os.path.exists(os.path.join(build, 'CMakeCache.txt')))
        self.assertFalse(os.path.exists(os.path.join(build, 'CMakeFiles')))
        self.assertTrue(os.path.exists(os.path.join(build, 'guest.o')))

    def test_source_fingerprint_works_without_git(self):
        write(self.p('source', 'runtime', 'one.cpp'), 'public source')
        sv.save_json(self.p('source', 'source-files.json'), ['runtime/one.cpp', 'installer/engine/sv.py'])
        with mock.patch.object(sv, 'ROOT', self.p('source')), mock.patch.object(sv, 'run', return_value=(1, '', 'not git')):
            a = sv.source_version()
            write(self.p('source', 'runtime', 'one.cpp'), 'new public source')
            self.assertNotEqual(a, sv.source_version())

    def test_runtime_kit_rejects_wrong_compiler_and_corruption(self):
        import runtimekit
        kit = self.p('kit')
        os.makedirs(kit)
        files = {}
        for name in ('libsheikah_runtime.a', 'libbotw_immersive.a'):
            write(os.path.join(kit, name), 'synthetic archive')
            files[name] = runtimekit.digest(os.path.join(kit, name))
        manifest = dict(source='source', compiler='clang\n', sdk='27.0', architecture='arm64', minimum_os='26.0', files=files)
        sv.save_json(os.path.join(kit, 'runtime.json'), manifest)
        def run(*cmd):
            return 0, ('27.0\n' if '--show-sdk-version' in cmd else 'clang\n'), ''
        with mock.patch.object(sv, 'source_version', return_value='source'), mock.patch.object(sv, 'run', side_effect=run):
            runtimekit.verify(kit)
            write(os.path.join(kit, 'libsheikah_runtime.a'), 'broken')
            with self.assertRaisesRegex(sv.StepError, 'damaged'):
                runtimekit.verify(kit)
            manifest['compiler'] = 'other'
            sv.save_json(os.path.join(kit, 'runtime.json'), manifest)
            with self.assertRaisesRegex(sv.StepError, 'different source, Xcode or SDK'):
                runtimekit.verify(kit)

    def test_zip_cache_invalidated_by_changed_game_file(self):
        self.do_import([make_game(self.p('src'))])
        gj = sv.load_json(self.p('work', 'game.json'))
        with contextlib.redirect_stdout(io.StringIO()):
            path = sv.make_zip(self.p('work'), gj)
            before = sv.load_json(path + '.json')
            write(self.p('work', 'game', 'content', 'new.bin'), 'new content')
            sv.make_zip(self.p('work'), gj)
        self.assertNotEqual(before['snapshot'], sv.load_json(path + '.json')['snapshot'])

    def test_command_events_never_dump_environment_secrets(self):
        sv.Runner.dry = True
        with contextlib.redirect_stdout(io.StringIO()) as out:
            sv.sh(['true'], 'test', env={'SECRET_TOKEN': 'hidden-secret', 'BOTW_API_KEY': 'hidden-key', 'BOTW_TEAM': 'TEAMA00001'})
        self.assertNotIn('hidden-secret', out.getvalue())
        self.assertNotIn('hidden-key', out.getvalue())
        self.assertIn('TEAMA00001', out.getvalue())
        sv.Runner.dry = False

    def test_bundle_id_rejects_shell_xml_and_path_fragments(self):
        for bad in ('com.x/<bad>', 'com.x."bad"', 'com.x.y\n', '../com.x.y', 'com..y'):
            with self.assertRaisesRegex(sv.StepError, 'bundle ID'):
                sv.validate_bundle_id(bad)
        self.assertEqual(sv.validate_bundle_id('com.botw.sheikahvision.installertest'), 'com.botw.sheikahvision.installertest')

    def test_paired_headset_from_device_json_fixture(self):
        fixture = os.path.join(os.path.dirname(__file__), 'fixtures', 'devices.json')
        def run(*cmd, **kw):
            self.assertIn('--json-output', cmd)
            with open(fixture) as f:
                sv.save_json(cmd[cmd.index('--json-output') + 1], json.load(f))
            return 0, 'Human output says no devices; it must be ignored.', ''
        with mock.patch.object(sv, 'run', side_effect=run):
            devices = sv.headsets()
        self.assertEqual(len(devices), 1)
        self.assertEqual(devices[0]['udid'], 'TEST-HEADSET')
        self.assertTrue(devices[0]['paired'])
        self.assertTrue(devices[0]['reachable'])

    def test_empty_documents_is_successful_json_listing(self):
        fixture = os.path.join(os.path.dirname(__file__), 'fixtures', 'files.json')
        def sh(cmd, *a, **kw):
            with open(fixture) as f:
                sv.save_json(cmd[-1], json.load(f))
        with mock.patch.object(sv, 'sh', side_effect=sh):
            result = sv.device_json(['device', 'info', 'files', '--subdirectory', 'Documents'])
        self.assertEqual(result['files'], [])
        self.assertEqual(result['subdirectory'], 'Documents')

    def test_warmup_uses_exact_profile_identity(self):
        from types import SimpleNamespace
        identity = 'A' * 40
        with mock.patch.object(sv, 'run', return_value=(0, '', '')), mock.patch.object(sv.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout='', stderr='')) as signing, contextlib.redirect_stdout(io.StringIO()):
            sv.warm_up_signing('TEAMA00001', self.tmp, identity=identity)
        self.assertEqual(signing.call_args.args[0][3], identity)

    def test_renew_skips_asset_compilation(self):
        work = self.p('work'); os.makedirs(work)
        sv.save_json(os.path.join(work, 'build.json'), {'team': 'TEAMA00001', 'bundle_id': 'com.example.test', 'app': '/fake/test.app'})
        with mock.patch.object(sv, 'provision'), mock.patch.object(sv, 'sign_app') as sign, mock.patch.object(sv, 'install_app'):
            rc, _, _ = self.run_sv('renew', '--work', work, '--dry-run', '--device', 'FAKE')
        self.assertEqual(rc, 0)
        self.assertFalse(sign.call_args.kwargs['compile_assets'])

    def test_install_rejects_state_bundle_mismatch(self):
        work = self.p('work'); os.makedirs(work)
        app = self.p('test.app'); os.makedirs(app)
        with open(os.path.join(app, 'Info.plist'), 'wb') as f:
            plistlib.dump({'CFBundleIdentifier': 'com.example.actual'}, f)
        sv.save_json(os.path.join(work, 'build.json'), {'bundle_id': 'com.example.actual', 'app': app})
        args = sv.parser().parse_args(['install', '--work', work, '--bundle-id', 'com.example.wrong'])
        with self.assertRaisesRegex(sv.StepError, 'bundle ID differs'):
            sv.load_build_state(args)

    def test_cancel_stops_the_active_child(self):
        pidfile = self.p('child-pid')
        code = 'import os,time;open(' + repr(pidfile) + ',"w").write(str(os.getpid()));print("ready",flush=True);time.sleep(60)'
        def cancel(line):
            raise KeyboardInterrupt()
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(KeyboardInterrupt):
            sv.sh([sys.executable, '-c', code], 'build', on_line=cancel)
        with open(pidfile) as f:
            pid = int(f.read())
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)
