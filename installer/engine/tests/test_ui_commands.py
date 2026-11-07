"""Small JSON-contract extensions for the automatic wizard; no device commands run."""
import contextlib
import io
import json
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import sv


class WizardCommands(unittest.TestCase):
    def invoke(self, name, error=None, dry=False, reachable=True, listing=None):
        args = sv.parser().parse_args([name, '--device', 'FIXTURE', '--json'] + (['--dry-run'] if dry else []))
        stream = io.StringIO()
        with mock.patch.object(sv, 'load_build_state', return_value=('/synthetic', {'bundle_id': 'com.botw.sheikahvision.installertest'})), \
             mock.patch.object(sv, 'device_json', side_effect=error, return_value=listing or {}) as device, \
             mock.patch.object(sv, 'headsets', return_value=[{'udid':'FIXTURE', 'paired':True, 'reachable':reachable}]), \
             mock.patch.object(sv.Out, 'json', True), mock.patch.object(sv.Runner, 'dry', dry), contextlib.redirect_stdout(stream):
            result = {'launch': sv.cmd_launch, 'documents': sv.cmd_documents}[name](args)
        return result, [json.loads(line) for line in stream.getvalue().splitlines()], device

    def test_launch_uses_selected_device_and_test_bundle(self):
        result, events, device = self.invoke('launch')
        self.assertEqual(result, 0)
        command, stage = device.call_args.args
        self.assertEqual(command, ['device', 'process', 'launch', '--device', 'FIXTURE', 'com.botw.sheikahvision.installertest'])
        sv.guard(['xcrun', 'devicectl'] + command)
        self.assertTrue(events[-1]['ok'])
        self.assertEqual(stage, 'launch')

    def test_documents_is_a_read_only_json_probe(self):
        result, events, device = self.invoke('documents')
        command = device.call_args_list[0].args[0]          # Documents first; Documents/Import is the second, read-only look
        self.assertTrue(all(c.args[0][:3] == ['device', 'info', 'files'] for c in device.call_args_list))
        self.assertEqual(command[:3], ['device', 'info', 'files'])
        self.assertIn('Documents', command)
        self.assertIn('appDataContainer', command)
        self.assertTrue(events[-1]['ready'])
        self.assertEqual(result, 0)

    def test_documents_reports_an_imported_game(self):
        _, events, _ = self.invoke('documents')
        self.assertFalse(events[-1]['has_game'])
        listing = {'files': [{'name': 'Import'}, {'name': 'game-receipt.json'}, {'name': 'game'}]}
        _, events, _ = self.invoke('documents', listing=listing)
        self.assertTrue(events[-1]['has_game'])

    def test_documents_reports_a_package_already_waiting_in_import(self):
        listings = {'Documents': {'files': [{'name': 'Import'}]}, 'Documents/Import': {'files': [{'name': 'SheikahVision-game.zip'}]}}
        def device(arguments, step='preflight'):
            return listings[arguments[arguments.index('--subdirectory') + 1]]
        args = sv.parser().parse_args(['documents', '--device', 'FIXTURE', '--json'])
        stream = io.StringIO()
        with mock.patch.object(sv, 'load_build_state', return_value=('/synthetic', {'bundle_id': 'com.botw.sheikahvision.installertest'})), \
             mock.patch.object(sv, 'device_json', side_effect=device), \
             mock.patch.object(sv, 'headsets', return_value=[{'udid': 'FIXTURE', 'paired': True, 'reachable': True}]), \
             mock.patch.object(sv.Out, 'json', True), contextlib.redirect_stdout(stream):
            sv.cmd_documents(args)
        done = [json.loads(l) for l in stream.getvalue().splitlines()][-1]
        self.assertTrue(done['game_pending']); self.assertFalse(done['has_game'])

    def documents_with_package(self, size_on_headset, expected):
        """documents with a zip of size_on_headset in Documents/Import, the whole package being `expected` bytes"""
        listings = {'Documents': {'files': [{'name': 'Import'}]},
                    'Documents/Import': {'files': [{'name': 'SheikahVision-game.zip', 'metadata': {'size': size_on_headset}}]}}
        def device(arguments, step='preflight'):
            return listings[arguments[arguments.index('--subdirectory') + 1]]
        args = sv.parser().parse_args(['documents', '--device', 'FIXTURE', '--json'])
        stream = io.StringIO()
        with mock.patch.object(sv, 'load_build_state', return_value=('/synthetic', {'bundle_id': 'com.botw.sheikahvision.installertest'})), \
             mock.patch.object(sv, 'device_json', side_effect=device), \
             mock.patch.object(sv, 'expected_package_size', return_value=expected), \
             mock.patch.object(sv, 'headsets', return_value=[{'udid': 'FIXTURE', 'paired': True, 'reachable': True}]), \
             mock.patch.object(sv.Out, 'json', True), contextlib.redirect_stdout(stream):
            sv.cmd_documents(args)
        return [json.loads(l) for l in stream.getvalue().splitlines()]

    def test_a_cut_off_package_is_sent_again(self):
        # 2026-11-18: a transfer cancelled at 3.89 of 13.0 GB was taken for a package already on the headset
        events = self.documents_with_package(3_890_000_000, 13_025_566_939)
        self.assertFalse(events[-1]['game_pending'])
        self.assertTrue(any(e['event'] == 'notice' and 'incomplete' in e['message'] for e in events))

    def test_a_whole_package_is_still_waiting(self):
        events = self.documents_with_package(13_025_566_939, 13_025_566_939)
        self.assertTrue(events[-1]['game_pending'])

    def test_expected_size_comes_from_the_kept_zip_then_push_json(self):
        import tempfile
        with tempfile.TemporaryDirectory() as work:
            self.assertIsNone(sv.expected_package_size(work))
            with open(os.path.join(work, 'push.json'), 'w') as f:
                json.dump({'size': 1234}, f)
            self.assertEqual(sv.expected_package_size(work), 1234)
            with open(os.path.join(work, 'SheikahVision-game.zip'), 'wb') as f:
                f.write(b'x' * 10)
            self.assertEqual(sv.expected_package_size(work), 10)

    def test_transfer_progress_reports_bytes_on_the_headset(self):
        import tempfile, time
        sizes = iter([100, 600, 1000])
        def device(arguments, step='preflight'):
            self.assertEqual(step, 'progress')              # the quiet, read-only sample
            self.assertEqual(arguments[:3], ['device', 'info', 'files'])
            return {'files': [{'name': 'SheikahVision-game.zip', 'metadata': {'size': next(sizes, 1000)}}]}
        with tempfile.TemporaryDirectory() as work:
            zp = os.path.join(work, 'SheikahVision-game.zip')
            with open(zp, 'wb') as f:
                f.write(b'x' * 1000)
            stream = io.StringIO()
            with mock.patch.object(sv, 'device_json', side_effect=device), mock.patch.object(sv.TransferProgress, 'INTERVAL', 0.01), \
                 mock.patch.object(sv.Out, 'json', True), mock.patch.object(sv.Runner, 'dry', False), contextlib.redirect_stdout(stream):
                with sv.TransferProgress('FIXTURE', 'com.example.test', zp, time.monotonic()):
                    time.sleep(0.2)
        events = [json.loads(l) for l in stream.getvalue().splitlines()]
        self.assertTrue(events)
        self.assertTrue(all(e['event'] == 'progress' and e['total_bytes'] == 1000 for e in events))
        self.assertEqual(events[0]['bytes'], 100)
        self.assertEqual(events[-1]['percent'], 100.0)

    def test_free_team_renewal_sets_this_apps_cached_profiles_aside_and_can_restore_them(self):
        import tempfile
        with tempfile.TemporaryDirectory() as root:
            folder, work = os.path.join(root, 'profiles'), os.path.join(root, 'work')
            os.makedirs(folder)
            for name in ('mine.mobileprovision', 'other.mobileprovision'):
                open(os.path.join(folder, name), 'w').close()
            def decode(path):
                app = 'TEAM1.com.x.app' if os.path.basename(path).startswith('mine') else 'TEAM1.com.other'
                return {'TeamIdentifier': ['TEAM1'], 'Entitlements': {'application-identifier': app}}
            with mock.patch.object(sv, 'PROFILE_FOLDER', folder), mock.patch.object(sv, 'decode_profile', side_effect=decode), \
                 contextlib.redirect_stdout(io.StringIO()):
                moved = sv.retire_cached_profiles('TEAM1', 'com.x.app', work)
                self.assertEqual([os.path.basename(m[0]) for m in moved], ['mine.mobileprovision'])
                self.assertEqual(sorted(os.listdir(folder)), ['other.mobileprovision'])   # another app's profile stays
                self.assertTrue(os.path.exists(moved[0][1]))                               # moved, not deleted
                sv.restore_profiles(moved)
                self.assertEqual(sorted(os.listdir(folder)), ['mine.mobileprovision', 'other.mobileprovision'])

    def probe(self, listings, apps=None, expected=None):
        def device(arguments, step='preflight'):
            if 'apps' in arguments:
                if apps is None: raise sv.StepError('no answer')
                return {'apps': apps}
            sub = arguments[arguments.index('--subdirectory') + 1]
            if sub not in listings: raise sv.StepError('The system failed to get a list of files on the remote device.')
            return listings[sub]
        args = sv.parser().parse_args(['documents', '--device', 'FIXTURE', '--json'])
        stream = io.StringIO()
        with mock.patch.object(sv, 'load_build_state', return_value=('/synthetic', {'bundle_id': 'com.x.app'})), \
             mock.patch.object(sv, 'device_json', side_effect=device), mock.patch.object(sv, 'expected_package_size', return_value=expected), \
             mock.patch.object(sv, 'headsets', return_value=[{'udid': 'FIXTURE', 'paired': True, 'reachable': True}]), \
             mock.patch.object(sv.Out, 'json', True), contextlib.redirect_stdout(stream):
            sv.cmd_documents(args)
        return [json.loads(l) for l in stream.getvalue().splitlines()][-1]

    def test_a_stopped_import_is_sent_again_and_trash_is_not_an_import(self):
        zipped = {'files': [{'name': 'SheikahVision-game.zip', 'metadata': {'size': 10}}]}
        done = self.probe({'Documents': {'files': [{'name': 'Import'}, {'name': '.game-import-released.json'}]}, 'Documents/Import': zipped}, expected=10)
        self.assertFalse(done['game_pending'])
        done = self.probe({'Documents': {'files': [{'name': 'Import'}, {'name': '.game-import-trash-1'}]}, 'Documents/Import': zipped}, expected=10)
        self.assertTrue(done['game_pending'])          # the whole zip waits; the trash folder is not an import under way
        done = self.probe({'Documents': {'files': [{'name': '.game-import'}]}})
        self.assertTrue(done['game_pending'])          # an import under way

    def test_a_failed_listing_says_whether_the_app_is_there(self):
        self.assertIs(self.probe({}, apps=[])['app_installed'], False)
        self.assertIs(self.probe({}, apps=[{'bundleIdentifier': 'com.x.app'}])['app_installed'], True)
        self.assertIsNone(self.probe({}, apps=None)['app_installed'])
        self.assertIs(self.probe({'Documents': {'files': []}, 'Documents/Import': {'files': []}})['app_installed'], True)

    def test_a_package_from_another_dump_is_not_trusted(self):
        import tempfile
        with tempfile.TemporaryDirectory() as work:
            sv.save_json(os.path.join(work, 'game.json'), {'rpx_sha256': 'new'})
            sv.save_json(os.path.join(work, 'push.json'), {'size': 99, 'rpx_sha256': 'old'})
            self.assertIsNone(sv.expected_package_size(work))
            sv.save_json(os.path.join(work, 'push.json'), {'size': 99, 'rpx_sha256': 'new'})
            self.assertEqual(sv.expected_package_size(work), 99)
            listing = {'files': [{'name': 'SheikahVision-game.zip', 'metadata': {'size': 5}}]}
            os.remove(os.path.join(work, 'push.json'))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertFalse(sv.waiting_package_complete(work, listing))   # unknown size, game on this Mac: resend

    def test_profiles_come_back_when_signing_fails(self):
        moved = [('/a', '/b')]
        st = {'team': 'T', 'bundle_id': 'com.x.app', 'app': '/app'}
        with mock.patch.object(sv, 'retire_cached_profiles', return_value=moved), mock.patch.object(sv, 'provision', return_value=None), \
             mock.patch.object(sv, 'cached_profiles', return_value=['/new']), \
             mock.patch.object(sv, 'sign_app', side_effect=sv.StepError('keychain')), \
             mock.patch.object(sv, 'restore_profiles') as restore, mock.patch.object(sv.Runner, 'dry', False):
            with self.assertRaises(sv.StepError):
                sv.resign_existing(st, '/work', fresh_profile=True)
        restore.assert_called_with(moved)

    def test_missing_documents_is_waiting_not_a_failed_pipeline(self):
        result, events, _ = self.invoke('documents', sv.StepError('Documents does not exist'))
        self.assertEqual(result, 0)
        self.assertTrue(events[-1]['ok'])
        self.assertFalse(events[-1]['ready'])
        self.assertEqual(events[-1]['reason'], 'not-opened')
        self.assertEqual(events[0]['event'], 'notice')

    def test_launch_failure_is_left_to_cli_error_handler(self):
        with self.assertRaisesRegex(sv.StepError, 'trust developer'):
            self.invoke('launch', sv.StepError('trust developer'))

    def test_documents_unreachable_does_not_query_container(self):
        result, events, device = self.invoke('documents', reachable=False)
        self.assertEqual(result, 0)
        self.assertTrue(events[-1]['ok'])
        self.assertFalse(events[-1]['ready'])
        self.assertEqual(events[-1]['reason'], 'unreachable')
        device.assert_not_called()

    def test_online_container_errors_are_not_opened_in_any_language(self):
        for message in ('permission denied', '尚未信任开发者，容器不可用', ''):
            with self.subTest(message=message):
                _, events, _ = self.invoke('documents', sv.StepError(message))
                self.assertEqual(events[-1]['reason'], 'not-opened')

    def test_launch_disconnect_has_a_specific_error_code(self):
        with self.assertRaises(sv.StepError) as raised:
            self.invoke('launch', sv.StepError('connection lost'), reachable=False)
        self.assertEqual(raised.exception.code, 'E_DEVICE_UNREACHABLE')

    def test_connection_is_rechecked_after_missing_documents(self):
        with mock.patch.object(sv, 'device_is_reachable', side_effect=[True, False]):
            _, events, _ = self.invoke('documents', sv.StepError('Documents does not exist'))
        self.assertEqual(events[-1]['reason'], 'unreachable')

    def test_failed_live_probe_does_not_trust_stale_connected_list(self):
        fixture = os.path.join(os.path.dirname(__file__), 'fixtures', 'devices.json')
        def run(*cmd, **kwargs):
            if 'list' in cmd:
                with open(fixture) as source:
                    sv.save_json(cmd[cmd.index('--json-output') + 1], json.load(source))
                return 0, '', ''
            self.assertIn('details', cmd)
            return 1, '', 'disconnected'
        with mock.patch.object(sv, 'run', side_effect=run):
            result = sv.headsets(probe=True)
        self.assertTrue(result[0]['paired'])
        self.assertFalse(result[0]['reachable'])
        self.assertEqual(result[0]['transport'], 'wired')

    def probe_fixture(self, state, success):
        with open(os.path.join(os.path.dirname(__file__), 'fixtures', 'devices.json')) as source:
            listing = json.load(source)
        listing['result']['devices'][0]['connectionProperties']['tunnelState'] = state
        calls = []
        def run(*cmd, **kwargs):
            path = cmd[cmd.index('--json-output') + 1]
            if 'list' in cmd:
                sv.save_json(path, listing)
                return 0, '', ''
            calls.append(cmd)
            if success:
                sv.save_json(path, {'info':{'outcome':'success'}, 'result':{'deviceProperties':{'developerModeStatus':'enabled'}}})
                return 0, '', ''
            return 1, '', 'not reachable'
        with mock.patch.object(sv, 'run', side_effect=run):
            result = sv.headsets(probe=True)
        return result[0], calls

    def test_idle_available_headset_can_be_confirmed(self):
        device, calls = self.probe_fixture('disconnected', True)
        self.assertEqual(device['availability'], 'available')
        self.assertTrue(device['reachable']); self.assertEqual(len(calls), 1)

    def test_available_probe_failure_has_a_reason(self):
        device, calls = self.probe_fixture('disconnected', False)
        self.assertFalse(device['reachable']); self.assertTrue(device['probe_error'])
        self.assertEqual(len(calls), 1)

    def test_unavailable_headset_is_not_probed(self):
        device, calls = self.probe_fixture('unavailable', True)
        self.assertEqual(device['availability'], 'offline')
        self.assertFalse(device['reachable']); self.assertEqual(calls, [])

    def test_connected_headset_is_confirmed_by_details(self):
        device, calls = self.probe_fixture('connected', True)
        self.assertEqual(device['availability'], 'connected')
        self.assertTrue(device['reachable']); self.assertEqual(len(calls), 1)

    def test_devices_probe_flag_is_forwarded(self):
        for flag in ([], ['--probe']):
            args = sv.parser().parse_args(['devices'] + flag)
            with mock.patch.object(sv, 'headsets', return_value=[]) as headsets, mock.patch.object(sv, 'emit'), mock.patch.object(sv.Runner, 'dry', False):
                sv.cmd_devices(args)
                headsets.assert_called_once_with(probe=bool(flag))

    def test_collect_without_setup_or_device_is_a_successful_skip(self):
        args = sv.parser().parse_args(['collect'])
        with mock.patch.object(sv, 'load_build_state', side_effect=sv.StepError('no app built yet')), mock.patch.object(sv, 'emit') as emit, mock.patch.object(sv, 'sh') as sh:
            self.assertEqual(sv.cmd_collect(args), 0)
            sh.assert_not_called()
            self.assertTrue(emit.call_args.kwargs['ok']); self.assertTrue(emit.call_args.kwargs['skipped'])

    def test_local_collect_never_resolves_a_device(self):
        args = sv.parser().parse_args(['collect', '--local-only'])
        with mock.patch.object(sv, 'load_build_state') as state, mock.patch.object(sv, 'emit') as emit:
            self.assertEqual(sv.cmd_collect(args), 0)
            state.assert_not_called(); self.assertTrue(emit.call_args.kwargs['skipped'])

    def test_dry_run_only_prints_plans_without_invoking_devicectl(self):
        for name in ('launch', 'documents'):
            args = sv.parser().parse_args([name, '--device', 'FIXTURE', '--dry-run'])
            with mock.patch.object(sv, 'load_build_state', return_value=('/synthetic', {'bundle_id': 'com.botw.sheikahvision.installertest'})), \
                 mock.patch.object(sv.Runner, 'dry', True), mock.patch.object(sv, 'run') as run, mock.patch.object(sv, 'emit') as emit:
                self.assertEqual({'launch':sv.cmd_launch, 'documents':sv.cmd_documents}[name](args), 0)
                run.assert_not_called()
                self.assertTrue(emit.call_args.kwargs['dry_run'])


if __name__ == '__main__':
    unittest.main()
