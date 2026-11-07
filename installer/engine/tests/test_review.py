"""Review regressions with synthetic profiles, archives and real local CMake builds; no device writes."""
import contextlib
import datetime
import hashlib
import io
import json
import os
from pathlib import Path
import plistlib
import signal
import subprocess
import sys
import time
from unittest import mock
from test_sv import Base, make_game
import sv
import runtimekit


class ReviewTests(Base):
    def kit(self, compiler='Apple clang version 21.0.0\nTarget: old\nInstalledDir: /old'):
        kit = Path(self.p('kit')); kit.mkdir(exist_ok=True)
        files = {}
        for name in ('libsheikah_runtime.a','libbotw_immersive.a'):
            (kit/name).write_bytes(b'synthetic')
            files[name] = runtimekit.digest(kit/name)
        sv.save_json(kit/'runtime.json',dict(source='source',compiler=compiler,sdk='27',architecture='arm64',minimum_os='26.0',files=files))
        return kit

    def test_B1_only_clang_line_and_sdk_decide_compiler_compatibility(self):
        kit = self.kit()
        with mock.patch.object(sv,'source_version',return_value='source'), mock.patch.object(sv,'run',side_effect=lambda *cmd: (0,'27' if '--show-sdk-version' in cmd else 'Apple clang version 21.0.0\nTarget: new\nInstalledDir: /new','')):
            runtimekit.verify(kit)
            args = sv.parser().parse_args(['doctor','--runtime',str(kit)])
            self.assertEqual(sv.resolve_runtime(args),str(kit))

    def test_B1_incompatible_kit_falls_back_and_checks_headers(self):
        kit = self.kit('Apple clang version 20.0.0')
        args = sv.parser().parse_args(['doctor','--runtime',str(kit)])
        with mock.patch.object(sv,'source_version',return_value='source'), mock.patch.object(sv,'run',return_value=(0,'Apple clang version 21.0.0','')), mock.patch.object(sv.shutil,'which',return_value='/tool'), mock.patch.object(sv,'runtime_headers',return_value={'boost':'','glm':''}), contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(sv.check_tools(args)[0],'fail')
            self.assertIn('falling back',output.getvalue())
            self.assertIn('brew install boost glm',sv.check_tools(args)[2])

    def profile(self, **changes):
        certificate = b'synthetic development certificate'
        profile = dict(TeamIdentifier=['TEAMA00001'],ExpirationDate=datetime.datetime.now().replace(microsecond=0)+datetime.timedelta(days=7),
                       ProvisionedDevices=['HEADSET'],DeveloperCertificates=[certificate],
                       Entitlements={'application-identifier':'TEAMA00001.com.example.test', 'get-task-allow':True,
                                     'com.apple.developer.kernel.increased-memory-limit':True})
        profile.update(changes)
        return profile,hashlib.sha1(certificate).hexdigest().upper()

    def test_B2_profile_filters_team_capability_device_expiry_identity(self):
        profile, identity = self.profile()
        wanted={'get-task-allow':True,'com.apple.developer.kernel.increased-memory-limit':True}
        for device in (None,'HEADSET'):
            self.assertEqual(sv.profile_identity(profile,'TEAMA00001','com.example.test',wanted,device,{identity}),identity)
        self.assertIsNone(sv.profile_identity(profile,'TEAMA00001','com.example.test',wanted,'OTHER',{identity}))
        for changes in (dict(TeamIdentifier=['OTHER']),dict(ExpirationDate=datetime.datetime(2020,1,1)),dict(DeveloperCertificates=[]),dict(ProvisionedDevices=[]),dict(Entitlements={'application-identifier':'TEAMA00001.com.example.test'})):
            p,_=self.profile(**changes)
            self.assertIsNone(sv.profile_identity(p,'TEAMA00001','com.example.test',wanted,'HEADSET',{identity}))
        self.assertIsNone(sv.profile_identity(profile,'TEAMA00001','com.example.test',wanted,'HEADSET',set()))

    def test_B2_install_repairs_only_missing_device_profile(self):
        profile,identity=self.profile()
        work=Path(self.p('work')); work.mkdir()
        app=work/'app';app.mkdir()
        (app/'Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier':'com.example.test'}))
        (app/'embedded.mobileprovision').write_bytes(plistlib.dumps(profile))
        sv.save_json(work/'build.json',dict(team='TEAMA00001',bundle_id='com.example.test',app=str(app)))
        for device,repairs in [('HEADSET',0),('OTHER',1)]:
            args=sv.parser().parse_args(['install','--work',str(work),'--device',device])
            with mock.patch.object(sv,'run',return_value=(0,identity,'')),mock.patch.object(sv,'resign_existing') as repair,mock.patch.object(sv,'install_app'),contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(sv.cmd_install(args),0)
                self.assertEqual(repair.call_count,repairs)
            self.assertIn(device,sv.load_json(work/'build.json')['installed_devices'])

    def test_B2_offline_provision_and_M3_prewarm_order(self):
        events=[]
        def sh(cmd,*a,**kw):
            events.append(cmd[0])
            if 'platform=visionOS,id=HEADSET' in cmd:
                raise sv.StepError('Unable to find a destination')
        with mock.patch.object(sv,'sh',side_effect=sh) as calls,mock.patch.object(sv,'warm_up_signing',side_effect=lambda *a: events.append('warm') or 'identity'),contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sv.provision('TEAMA00001','com.example.test',False,self.tmp,'HEADSET'),'identity')
        self.assertLess(events.index('warm'),events.index('xcodebuild'))
        self.assertIn('generic/platform=visionOS',calls.call_args.args[0])

    def test_M2_multiple_teams_require_explicit_choice(self):
        args=sv.parser().parse_args(['build'])
        with mock.patch.dict(os.environ,{},clear=True),mock.patch.object(sv,'xcode_teams',return_value=[('TEAMA00001','A',True),('ABCDE12345','B',False)]):
            with self.assertRaisesRegex(sv.StepError,'select a team explicitly'):
                sv.resolve_team(args)

    def test_B3_manifest_stamp_relinks_even_when_new_archive_is_older(self):
        root=Path(self.tmp); kit=root/'kit';kit.mkdir()
        project=root/'project';project.mkdir();build=root/'build'
        (project/'main.c').write_text('int main(){return 0;}')
        (project/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.24)\nproject(stamp C)\nadd_executable(app main.c)\ninclude("'+str(Path(sv.ROOT)/'installer/cmake/RuntimeStamp.cmake')+'")\nbotw_runtime_stamp(app "'+str(kit)+'")\n')
        def manifest(v,mtime):
            for n in ('libsheikah_runtime.a','libbotw_immersive.a'):
                (kit/n).write_text(v);os.utime(kit/n,(mtime,mtime))
            (kit/'runtime.json').write_text(json.dumps({'revision':v}))
            os.utime(kit/'runtime.json',(mtime,mtime))
        def configure():
            subprocess.run(['cmake','-S',str(project),'-B',str(build),'-G','Ninja'],check=True,capture_output=True)
        def run():
            return subprocess.run(['cmake','--build',str(build)],check=True,capture_output=True,text=True).stdout
        manifest('newer-time',time.time()-100);configure();run()
        self.assertIn('no work',run())
        manifest('older-time',time.time()-10000);configure()
        self.assertIn('Linking',run())
        self.assertIn('no work',run())

    def test_B3_host_invalidation_preserves_guest_cache(self):
        b=Path(self.p('build')); guest=b/'CMakeFiles/guest.dir';host=b/'CMakeFiles/runtime_host.dir'
        guest.mkdir(parents=True);host.mkdir();(guest/'one.o').write_text('guest');(host/'one.o').write_text('host')
        sv.refresh_host_cache(str(b),'source-a',None)
        self.assertTrue((guest/'one.o').exists());self.assertFalse(host.exists())
        host.mkdir();(host/'one.o').write_text('host')
        sv.refresh_host_cache(str(b),'source-a',None);self.assertTrue(host.exists())
        sv.refresh_host_cache(str(b),'source-b',None);self.assertFalse(host.exists())

    def test_L1_early_build_failure_emits_done_and_code(self):
        rc,out,_=self.run_sv('build','--work',self.tmp,'--team','TEAMA00001','--dry-run','--json')
        self.assertEqual(rc,1)
        events=[json.loads(line) for line in out.splitlines()]
        self.assertEqual(events[-2]['code'],'E_NO_GAME');self.assertFalse(events[-1]['ok'])

    def test_L3_real_sigterm_kills_child(self):
        pid=Path(self.p('pid'))
        child='import os,time;open('+repr(str(pid))+',"w").write(str(os.getpid()));print("ready",flush=True);time.sleep(60)'
        code='import sv,sys;sv.install_signal_handlers();sv.sh([sys.executable,"-c",'+repr(child)+'],"test")'
        env=dict(os.environ,PYTHONPATH=str(Path(sv.ROOT)/'installer/engine'))
        p=subprocess.Popen([sys.executable,'-c',code],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        try:
            for _ in range(100):
                if pid.exists():break
                time.sleep(.02)
            self.assertTrue(pid.exists());p.terminate();p.wait(timeout=10)
            with self.assertRaises(ProcessLookupError):os.kill(int(pid.read_text()),0)
        finally:
            if p.poll() is None:p.kill();p.wait()

    def test_L4_cancelled_publish_preserves_previous_app(self):
        source=Path(self.p('source'));source.mkdir();(source/'new').write_text('new')
        dest=Path(self.p('app'));dest.mkdir();(dest/'old').write_text('old')
        def copy_then_cancel(cmd,*a,**kw):
            Path(cmd[-1]).mkdir();Path(cmd[-1],'partial').write_text('partial')
            raise KeyboardInterrupt()
        with mock.patch.object(sv,'sh',side_effect=copy_then_cancel),self.assertRaises(KeyboardInterrupt):
            sv.publish_app(str(source),str(dest))
        self.assertEqual((dest/'old').read_text(),'old')
        self.assertFalse(list(dest.parent.glob('app.staging-*')))

    def test_L8_extra_files_are_rejected(self):
        kit=self.kit('Apple clang version 21')
        (kit/'unlisted').write_text('not part of manifest')
        with mock.patch.object(sv,'source_version',return_value='source'),mock.patch.object(sv,'run',side_effect=lambda *cmd:(0,'27' if '--show-sdk-version' in cmd else 'Apple clang version 21','')):
            with self.assertRaisesRegex(sv.StepError,'unlisted'):
                runtimekit.verify(kit)

    def test_B1_kit_selection_happens_before_game_or_provision(self):
        args=sv.parser().parse_args(['build','--work',self.tmp,'--runtime','/missing','--dry-run'])
        sv.Runner.dry=True
        try:
            with mock.patch.object(sv,'resolve_runtime',return_value=None) as selection,mock.patch.object(sv,'provision') as provision:
                with self.assertRaisesRegex(sv.StepError,'no imported game'):
                    sv.cmd_build(args)
                selection.assert_called_once_with(args);provision.assert_not_called()
        finally:sv.Runner.dry=False

    def test_M2_account_reports_actual_warmup_only(self):
        for value in (None,'identity'):
            with mock.patch.object(sv,'xcode_teams',return_value=[('TEAMA00001','Test',True)]),mock.patch.object(sv,'warm_up_signing',return_value=value):
                rc,out,_=self.run_sv('account','--work',self.tmp,'--team','TEAMA00001','--json')
            self.assertEqual(rc,0)
            self.assertEqual(json.loads(out.splitlines()[-1])['warmed'],bool(value))

    def test_L7_untracked_source_and_normal_build_cache_are_rejected(self):
        root=Path(self.p('checkout'));root.mkdir()
        subprocess.run(['git','init','-q',str(root)],check=True)
        (root/'ios').mkdir();(root/'ios/new.cpp').write_text('untracked source')
        script=Path(sv.ROOT)/'installer/scripts/check_source.py'
        p=subprocess.run([sys.executable,str(script),str(root),str(root/'cache')],capture_output=True,text=True)
        self.assertNotEqual(p.returncode,0);self.assertIn('untracked',p.stderr)
        (root/'ios/new.cpp').unlink();(root/'cache').mkdir();(root/'cache/CMakeCache.txt').write_text('BOTW_RUNTIME_ONLY:BOOL=OFF')
        p=subprocess.run([sys.executable,str(script),str(root),str(root/'cache')],capture_output=True,text=True)
        self.assertNotEqual(p.returncode,0);self.assertIn('ordinary build',p.stderr)

    def test_L5_renew_rejects_different_team(self):
        work=Path(self.p('work'));work.mkdir()
        sv.save_json(work/'build.json',dict(team='TEAMA00001',bundle_id='com.example.test',app='/fake/app'))
        rc,out,err=self.run_sv('renew','--work',str(work),'--team','ABCDE12345','--dry-run','--json')
        self.assertEqual(rc,1);self.assertIn('original app team',out)

    def test_M1_real_cmake_overrides_stale_package_dirs(self):
        kit=Path(self.p('cmake-kit'));kit.mkdir()
        (kit/'runtime.json').write_text('{"revision":"fixture"}')
        packages={'SDL3':['SDL3::SDL3-static'],'fmt':['fmt::fmt'],'zstd':['zstd::libzstd_static']}
        for name,targets in packages.items():
            directory=kit/'prefix-xros/lib/cmake'/name;directory.mkdir(parents=True)
            (directory/(name+'Config.cmake')).write_text('\n'.join('add_library('+target+' INTERFACE IMPORTED)' for target in targets))
        build=Path(self.p('cmake-build'))
        env=dict(os.environ,CLANG_MODULE_CACHE_PATH=self.p('clang-cache'),SWIFT_MODULECACHE_PATH=self.p('swift-cache'))
        cmd=['cmake','-S',str(Path(sv.ROOT)/'ios'),'-B',str(build),'-G','Ninja','-DCMAKE_SYSTEM_NAME=visionOS','-DCMAKE_OSX_SYSROOT=xros','-DCMAKE_OSX_ARCHITECTURES=arm64','-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0','-DBOTW_RUNTIME_ONLY=ON','-DBOTW_PREBUILT_RUNTIME='+str(kit),'-DBOTW_IOS_DEPS=/stale/ios-deps']
        cmd += ['-D'+n+'_DIR=/stale/old-package' for n in packages]
        p=subprocess.run(cmd,env=env,capture_output=True,text=True)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        cache=(build/'CMakeCache.txt').read_text()
        for name in packages:self.assertIn(name+'_DIR:PATH='+str(kit/'prefix-xros/lib/cmake'/name),cache)

    def test_non_dry_build_publishes_state_and_reuses_real_cmake_build(self):
        from test_sv import RPX_SHA
        self.do_import([make_game(self.p('input'))])
        root=Path(self.p('source'));(root/'ios').mkdir(parents=True)
        (root/'ios-deps/prefix-xros').mkdir(parents=True)
        (root/'ios/CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.24)\nproject(Fixture C)\nadd_executable(SheikahVision MACOSX_BUNDLE main.c)\n')
        (root/'ios/main.c').write_text('int main(){return 0;}')
        work=self.p('work');cache=Path(work)/'cache'/('generated-'+RPX_SHA[:12]+'-generator')
        cache.mkdir(parents=True);(cache/'fn_0000.cpp').write_text('// fixture')
        sv.save_json(cache/'.complete',dict(rpx_sha256=RPX_SHA,recompiler_version='generator'))
        args=sv.parser().parse_args(['build','--work',work,'--team','TEAMA00001','--device','HEADSET'])
        profile=((datetime.datetime.now(),None,{},'profile-key'),{})
        with mock.patch.object(sv,'ROOT',str(root)),mock.patch.object(sv,'recompiler_version',return_value=('generator','git')),mock.patch.object(sv,'source_version',return_value='source-content'),mock.patch.object(sv,'provision',return_value='older-key'),mock.patch.object(sv,'select_profile',return_value=profile),mock.patch.object(sv,'warm_up_signing',return_value='profile-key') as warm,mock.patch.object(sv,'sign_app'),contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(sv.cmd_build(args),0)
            first=sv.load_json(Path(work)/'build.json')
            self.assertEqual(sv.cmd_build(args),0)
            second=sv.load_json(Path(work)/'build.json')
        self.assertEqual(first['source_version'],'source-content')
        self.assertIsNone(second['kit_manifest_sha'])
        self.assertTrue(Path(second['app'],'Info.plist').is_file())
        self.assertIn('no work to do',output.getvalue())
        self.assertEqual(warm.call_count,2)

    def test_M3_newest_valid_team_identity_is_warmed(self):
        from types import SimpleNamespace
        old='A'*40;new='B'*40
        pem=lambda s:'SHA-1 hash: '+s+'\n-----BEGIN CERTIFICATE-----\n'+s+'\n-----END CERTIFICATE-----'
        def run(*cmd):return (0,pem(new)+'\n'+pem(old) if 'find-certificate' in cmd else old+' '+new,'')
        def openssl(cmd,**kw):
            year='2026' if new in kw['input'] else '2025'
            return SimpleNamespace(stdout='subject=OU=TEAMA00001,CN=Apple Development\nnotBefore=Jan  1 00:00:00 '+year+' GMT\n')
        with mock.patch.object(sv,'run',side_effect=run),mock.patch.object(sv.subprocess,'run',side_effect=openssl):
            self.assertEqual(sv.latest_identity('TEAMA00001'),new)

    def test_M5_status_checks_game_without_copying(self):
        self.do_import([make_game(self.p('input'))])
        args=sv.parser().parse_args(['status','--work',self.p('work')])
        with contextlib.redirect_stdout(io.StringIO()) as output,mock.patch.object(sv.gi,'copy_tree',side_effect=AssertionError('must not copy')):
            self.assertEqual(sv.cmd_status(args),0)
        self.assertIn('208',output.getvalue() if sv.Out.json else str(sv.gi.validate([self.p('work','game')])))

    def test_B2_sign_app_reads_fixture_profiles_for_empty_matching_and_missing_device(self):
        profile,identity=self.profile()
        home=Path(self.p('home'));directory=home/'Library/Developer/Xcode/UserData/Provisioning Profiles'
        directory.mkdir(parents=True)
        (directory/'valid.mobileprovision').write_bytes(plistlib.dumps(profile))
        for device in (None,'HEADSET','OTHER'):
            app=Path(self.p('app-'+str(device)));app.mkdir()
            with mock.patch.dict(os.environ,{'HOME':str(home)}),mock.patch.object(sv,'run',return_value=(0,identity,'')),mock.patch.object(sv,'warm_up_signing',return_value=identity),mock.patch.object(sv,'sh') as sh:
                if device=='OTHER':
                    with self.assertRaisesRegex(sv.StepError,'connect and pair'):
                        sv.sign_app(str(app),'TEAMA00001','com.example.test',False,self.tmp,device,compile_assets=False)
                    sh.assert_not_called()
                else:
                    sv.sign_app(str(app),'TEAMA00001','com.example.test',False,self.tmp,device,compile_assets=False)
                    self.assertEqual(sv.decode_profile(app/'embedded.mobileprovision'),profile)
                    self.assertTrue(any(identity in call.args[0] for call in sh.call_args_list))
                    self.assertFalse(any('actool' in call.args[0] for call in sh.call_args_list))

    def test_benchmark_tracks_children_in_separate_job_groups(self):
        import importlib.util
        path=Path(sv.ROOT)/'installer/scripts/benchmark_guest.py'
        spec=importlib.util.spec_from_file_location('benchmark_review',path)
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        rows=[(100,1,'cmake'),(101,100,'ninja'),(200,101,'clang++'),(201,200,'clang'),(300,1,'swiftc')]
        self.assertEqual(module.external_compilers(rows,100),['300 swiftc'])
        self.assertEqual(module.external_compilers(rows[:-1],100),[])

    def test_L4_interrupt_between_renames_restores_published_app(self):
        import shutil
        source=Path(self.p('source'));source.mkdir();(source/'new').write_text('new')
        dest=Path(self.p('app'));dest.mkdir();(dest/'old').write_text('old')
        original=os.rename;interrupted=[]
        def rename(a,b):
            original(a,b)
            if os.fspath(a)==str(dest) and not interrupted:
                interrupted.append(True);raise KeyboardInterrupt()
        def copy(cmd,*a,**kw):shutil.copytree(cmd[1],cmd[2])
        with mock.patch.object(sv,'sh',side_effect=copy),mock.patch.object(sv.os,'rename',side_effect=rename),self.assertRaises(KeyboardInterrupt):
            sv.publish_app(str(source),str(dest))
        self.assertEqual((dest/'old').read_text(),'old')

    def test_M2_list_refresh_does_not_warm_selected_team(self):
        with mock.patch.object(sv,'xcode_teams',return_value=[('TEAMA00001','Test',True)]),mock.patch.object(sv,'warm_up_signing') as warm:
            rc,out,_=self.run_sv('account','--team','TEAMA00001','--list-only','--json')
        self.assertEqual(rc,0);warm.assert_not_called()
        self.assertFalse(json.loads(out.splitlines()[-1])['warmed'])

    def test_runtime_compiler_arguments_preserve_source_paths_with_spaces(self):
        import shutil
        import shlex
        original=Path(sv.ROOT)
        source=Path(self.p('checkout with spaces'));(source/'ios').mkdir(parents=True)
        shutil.copyfile(original/'ios/CMakeLists.txt',source/'ios/CMakeLists.txt')
        shutil.copyfile(original/'VERSION',source/'VERSION')  # ios/CMakeLists.txt reads the app version from it
        (source/'ios/Info.plist.in').symlink_to(original/'ios/Info.plist.in')
        for directory in ('runtime','installer'):(source/directory).symlink_to(original/directory,target_is_directory=True)
        for directory in ('src','compat','assets'):(source/'ios'/directory).symlink_to(original/'ios'/directory,target_is_directory=True)
        build=Path(self.p('space-build'))
        env=dict(os.environ,CLANG_MODULE_CACHE_PATH=self.p('clang-cache'),SWIFT_MODULECACHE_PATH=self.p('swift-cache'))
        cmd=['cmake','-S',str(source/'ios'),'-B',str(build),'-G','Ninja','-DCMAKE_SYSTEM_NAME=visionOS','-DCMAKE_OSX_SYSROOT=xros','-DCMAKE_OSX_ARCHITECTURES=arm64','-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0','-DBOTW_RUNTIME_ONLY=ON','-DBOTW_IOS_DEPS='+str(original/'ios-deps')]
        p=subprocess.run(cmd,env=env,capture_output=True,text=True)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        commands=subprocess.check_output(['ninja','-C',str(build),'-t','commands','botw_immersive'],text=True)
        args=shlex.split(next(line for line in commands.splitlines() if '-import-objc-header' in line))
        self.assertEqual(args[args.index('-import-objc-header')+1],str(source/'ios/src/vr_bridge.h'))
        self.assertEqual(args[args.index('-debug-prefix-map')+1],str(source)+'=.')
        commands=subprocess.check_output(['ninja','-C',str(build),'-t','commands','cemu_gpu'],text=True)
        args=shlex.split(commands.splitlines()[0])
        self.assertEqual(args[args.index('-include')+1],str(source/'ios/compat/botw_ios_platform.h'))

    def test_B1_cache_fingerprints_ignore_host_target_and_install_path(self):
        values=[]
        for host in ('first','second'):
            def run(*cmd):
                return 0,('27.0\n' if '--show-sdk-version' in cmd else 'Apple clang version 21.0.0\nTarget: '+host+'\nInstalledDir: /'+host),' '
            with mock.patch.object(sv,'run',side_effect=run),mock.patch.object(sv,'source_version',return_value='source'):
                values.append((sv.guest_key('rpx'),sv.compiled_key('rpx')))
        self.assertEqual(values[0],values[1])

    def test_B2_provision_pins_xros_with_inherited_macos_sdk(self):
        sdk=subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-path'],text=True).strip()
        original=sv.sh
        def commands(cmd,step,**kw):
            if cmd[0]=='cmake':
                self.assertIn('-DCMAKE_OSX_SYSROOT=xros',cmd)
                # SDK initialization is generator-independent; isolate global Xcode build services in unit tests.
                configure=list(cmd);configure[configure.index('-G')+1]='Ninja'
                return original(configure,step,**kw)
            self.assertEqual(cmd[0],'xcodebuild')
            self.assertIn('platform=visionOS,id=HEADSET',cmd)
            return ''
        with mock.patch.dict(os.environ,{'SDKROOT':sdk}),mock.patch.object(sv,'sh',side_effect=commands),mock.patch.object(sv,'warm_up_signing',return_value=None),contextlib.redirect_stdout(io.StringIO()):
            sv.provision('TEAMA00001','com.botw.sheikahvision.installertest',False,self.tmp,'HEADSET')
        cache=Path(self.tmp)/'provision/TEAMA00001/com.botw.sheikahvision.installertest/CMakeCache.txt'
        self.assertIn('CMAKE_OSX_SYSROOT:STRING=xros',cache.read_text())
