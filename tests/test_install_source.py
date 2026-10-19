# SPDX-License-Identifier: GPL-3.0-or-later
"""Source-only preflight regressions using synthetic inputs, never private builds."""
import hashlib
import importlib
import io
import json
import os
import signal
import time
import pathlib
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
install = importlib.import_module('prepare_install')
payload = importlib.import_module('prepare_payload_sources')
font = importlib.import_module('acquire_game_font')
codecs = importlib.import_module('prepare_game_codecs')
textures = importlib.import_module('prepare_textures')


def synthetic_image(path, identity='NPJB40001'):
    """Small ISO9660 hierarchy containing independently written placeholder bytes."""
    def record(name, block, size, directory=False):
        name = name.encode('ascii')
        length = 33 + len(name)
        length += length % 2
        data = bytearray(length)
        data[0] = length
        struct.pack_into('<I', data, 2, block)
        struct.pack_into('<I', data, 10, size)
        data[25] = 2 if directory else 0
        data[32] = len(name)
        data[33:33 + len(name)] = name
        return data
    key = b'DISC_ID\0'
    value = identity.encode() + b'\0'
    sfo = b'\0PSF' + struct.pack('<IIII', 0x101, 36, 36 + len(key), 1)
    sfo += struct.pack('<HHIII', 0, 0x204, len(value), len(value), 0) + key + value
    executable = b'Independently written synthetic executable fixture, not a game.'
    data = bytearray(27 * 2048)
    pvd = 16 * 2048
    data[pvd:pvd + 7] = b'\x01CD001\x01'
    struct.pack_into('<H', data, pvd + 128, 2048)
    struct.pack_into('<I', data, pvd + 158, 20)
    struct.pack_into('<I', data, pvd + 166, 2048)
    for block, entries in (
        (20, record('PSP_GAME', 21, 2048, True)),
        (21, record('PARAM.SFO', 24, len(sfo)) + record('SYSDIR', 22, 2048, True)),
        (22, record('EBOOT.BIN', 25, len(executable))),
        (24, sfo), (25, executable)):
        data[block * 2048:block * 2048 + len(entries)] = entries
    path.write_bytes(data)
    return hashlib.sha256(executable).hexdigest()


class SourceInstallTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / 'out/data/source-test-fixtures'
        parent.mkdir(parents=True, exist_ok=True)
        self.workspace = tempfile.TemporaryDirectory(dir=parent)
        self.base = pathlib.Path(self.workspace.name).resolve()
        self.assertTrue(self.base.is_relative_to((ROOT / 'out/data').resolve()))
        self.addCleanup(self.workspace.cleanup)
        self.iso = self.base / 'synthetic.iso'
        self.expected = synthetic_image(self.iso)
        self.pack = self.base / 'author-pack'
        self.pack.mkdir()
        (self.pack / 'textures.ini').write_text('[hashes]\n000000000000000100000000=One.png\n')
        (self.pack / 'One.png').write_bytes(b'Synthetic texture placeholder')

    def test_identity_and_executable_are_independent_checks(self):
        with self.assertRaisesRegex(ValueError, 'executable'):
            install.check_resources(self.iso, self.pack)
        with patch.object(install, 'EXPECTED_EBOOT', self.expected):
            self.assertEqual(install.check_resources(self.iso, self.pack)['textures']['png_count'], 1)
            synthetic_image(self.iso, 'WRONG0000')
            with self.assertRaisesRegex(ValueError, 'DISC_ID'):
                install.check_resources(self.iso, self.pack)

    def test_missing_and_unsafe_texture_inputs(self):
        with self.assertRaisesRegex(ValueError, 'textures.ini'):
            install.check_resources(self.iso, self.base)
        (self.pack / 'textures.ini').write_text('[hashes]\n1=../outside.png\n')
        with patch.object(install, 'EXPECTED_EBOOT', self.expected):
            with self.assertRaisesRegex(ValueError, 'safe'):
                install.check_resources(self.iso, self.pack)

    def test_texture_case_fallback_and_missing_paths(self):
        (self.pack / 'textures.ini').write_text('[hashes]\n1=one.png\n2=missing.png\n3=./One.png\n')
        result = install.inspect_textures(self.pack)
        if sys.platform != 'win32':
            self.assertEqual(result['case_fallback'], ['one.png'])
        self.assertEqual(result['missing'], ['missing.png'])
        self.assertEqual(result['unsafe'], [])

    def test_output_confinement(self):
        for value in (ROOT / 'outside', ROOT / 'out/data', ROOT / 'out/data/../../../outside'):
            with self.assertRaisesRegex(ValueError, 'Output'):
                install.output_path(str(value))
        self.assertEqual(install.output_path(None), ROOT / 'out/data/install')

    def test_resume_never_adopts_existing_data(self):
        for resume in (False, True):
            with self.assertRaisesRegex(ValueError, 'preserve|preserved'):
                install.check_existing(self.pack, resume)
        self.assertTrue((self.pack / 'One.png').exists())

    def test_symlink_outputs_and_inputs_rejected(self):
        link = self.base / 'link'
        try:
            link.symlink_to(self.pack, target_is_directory=True)
        except OSError:
            self.skipTest('Host cannot create symlinks')
        with self.assertRaisesRegex(ValueError, 'Symlink'):
            install.check_resources(self.iso, link)
        with self.assertRaisesRegex(ValueError, 'Symlink'):
            install.output_path(str(link / 'output'))

    def test_isolated_preflight_requires_no_archive_or_historical_folder(self):
        repo = self.base / 'isolated'
        for directory in ('tools','src','cmake','assets','LICENSES'):
            shutil.copytree(ROOT/directory,repo/directory,ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
        for name in ('README.md','INSTALL.md','LICENSE','dependencies.lock.json'):
            shutil.copyfile(ROOT/name,repo/name)
        before_iso = self.iso.read_bytes()
        before_pack = {p.name: p.read_bytes() for p in self.pack.iterdir()}
        entry = ('import sys; sys.path.insert(0, sys.argv.pop(1)); import prepare_install as p; '
                 'p.EXPECTED_EBOOT=sys.argv.pop(1); raise SystemExit(p.main())')
        result = subprocess.run([sys.executable,'-B','-c',entry,str(repo/'tools'),self.expected,
            '--iso',str(self.iso),'--textures',str(self.pack),'--preflight-only'],cwd=repo,
            capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('Preflight PASS',result.stdout)
        self.assertFalse((repo/'out').exists())
        self.assertEqual(before_iso,self.iso.read_bytes())
        self.assertEqual(before_pack,{p.name:p.read_bytes() for p in self.pack.iterdir()})

    def test_resume_requires_matching_owned_input_and_source_state(self):
        import install_pipeline as pipeline
        output=self.base/'owned-stage'
        identity={'iso_sha256':'a','textures_sha256':'b','source_sha256':'c'}
        state=pipeline.state_path(output)
        state.parent.mkdir(parents=True,exist_ok=True)
        state.write_text(json.dumps({'identity':identity,'output':output.relative_to(ROOT).as_posix()}))
        self.addCleanup(state.unlink)
        self.assertIsNotNone(pipeline.existing(output,True,identity))
        for resume, other in ((False,identity),(True,dict(identity,iso_sha256='different'))):
            with self.assertRaisesRegex(ValueError,'preserved'):
                pipeline.existing(output,resume,other)
        self.assertTrue(state.exists())

    def test_prepared_texture_manifest_rejects_changes_and_escape(self):
        import install_pipeline as pipeline
        path=self.pack/'runtime-manifest.json'
        path.write_text(json.dumps({'One.png':install.digest(self.pack/'One.png')}))
        pipeline.texture_identity(self.pack)
        path.write_text(json.dumps({'One.png':'wrong'}))
        with self.assertRaisesRegex(ValueError,'differs'):
            pipeline.texture_identity(self.pack)
        path.write_text(json.dumps({'../outside':'x'}))
        with self.assertRaisesRegex(ValueError,'Unsafe'):
            pipeline.texture_identity(self.pack)

    def test_reserved_build_outputs_rejected(self):
        with self.assertRaisesRegex(ValueError,'reserved'):
            install.output_path(str(ROOT/'out/data/game-boot/install'))

    def test_assembly_conflicts_preserve_unknown_files(self):
        import assemble_install as assembly
        destination=self.base/'existing-file'
        destination.write_bytes(b'original')
        with self.assertRaisesRegex(ValueError,'preserved'):
            assembly.owned_bytes(destination,b'different')
        self.assertEqual(destination.read_bytes(),b'original')

    def test_assembly_rejects_links_and_keeps_original_bytes(self):
        import assemble_install as assembly
        assembly._CHECKED_DIRECTORIES.clear()
        target=self.base/'original'
        target.write_bytes(b'preserved')
        alias=self.base/'alias'
        alias.symlink_to(self.base,target_is_directory=True)
        with self.assertRaisesRegex(ValueError,'Symlink'):
            assembly.owned_bytes(alias/'nested/new',b'no')
        destination=self.base/'hardlink'
        os.link(target,destination)
        with self.assertRaisesRegex(ValueError,'preserved'):
            assembly.owned_bytes(destination,b'preserved')
        self.assertEqual(target.read_bytes(),b'preserved')
        self.assertFalse((self.base/'nested').exists())

    @unittest.skipUnless(os.name=='posix','Source builds use POSIX process groups')
    def test_interruption_stops_owned_child_before_returning(self):
        pidfile=self.base/'child-pid'
        child='import os,pathlib,sys,time; pathlib.Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(30)'
        program=('import sys; sys.path.insert(0,sys.argv[1]); from source_support import run; '
                 'run([sys.executable,"-c",sys.argv[2],sys.argv[3]],__import__("pathlib").Path(sys.argv[4]))')
        parent=subprocess.Popen([sys.executable,'-B','-c',program,str(ROOT/'tools'),child,str(pidfile),
                                 str(self.base/'interruption.log')],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            deadline=time.monotonic()+5
            while not pidfile.exists() and time.monotonic()<deadline:
                time.sleep(0.02)
            self.assertTrue(pidfile.exists())
            child_pid=int(pidfile.read_text())
            parent.send_signal(signal.SIGINT)
            parent.communicate(timeout=5)
            self.assertNotEqual(parent.returncode,0)
            with self.assertRaises(ProcessLookupError):
                os.kill(child_pid,0)
        finally:
            if parent.poll() is None:
                parent.kill()
                parent.communicate()

    def test_modes_include_every_empty_savedata_parent(self):
        import assemble_install as assembly
        output=self.base/'mode-stage'
        title=output/'PPSA99373'
        directories=assembly.managed_directories(output,title)
        for relative in ('yakumo/saves','yakumo/saves/ms0','yakumo/saves/ms0/PSP',
                         'yakumo/saves/ms0/PSP/SAVEDATA','yakumo/cache'):
            self.assertIn(title/relative,directories)

    def test_shell_sources_preserve_lf(self):
        for path in (ROOT/'tools').glob('*.sh'):
            self.assertNotIn(b'\r',path.read_bytes(),str(path))

    def test_source_archive_escape_and_symlink_redirect_are_rejected(self):
        import source_support as support
        archive=self.base/'source.tar'
        for filename,link in (('../outside',''),('root/link','../../outside')):
            with tarfile.open(archive,'w') as tar:
                info=tarfile.TarInfo(filename)
                if link:
                    info.type=tarfile.SYMTYPE
                    info.linkname=link
                    tar.addfile(info)
                else:
                    info.size=4
                    tar.addfile(info,io.BytesIO(b'test'))
            with self.assertRaisesRegex(ValueError,'Unsafe|escapes'):
                support.extract(archive,self.base/'unpack',{'fixture':1})
        self.assertFalse((self.base/'outside').exists())

    def test_container_frontend_passes_read_only_inputs_and_same_entry(self):
        import container_install as frontend
        from types import SimpleNamespace
        arguments=['container_install.py','--iso',str(self.iso),'--textures',str(self.pack),
                   '--output',str(self.base/'container-stage'),'--preflight-only']
        with patch.object(frontend,'check_context'), patch.object(sys,'argv',arguments), patch.object(frontend.shutil,'which',return_value='/usr/bin/docker'), \
             patch.object(install,'EXPECTED_EBOOT',self.expected), \
             patch.object(frontend.subprocess,'run',return_value=SimpleNamespace(returncode=0)) as execute:
            self.assertEqual(frontend.main(),0)
        command=execute.call_args_list[-1].args[0]
        self.assertIn(str(self.iso)+':/inputs/game.iso:ro',command)
        self.assertIn(str(self.pack)+':/inputs/textures:ro',command)
        self.assertIn('tools/prepare_install.sh',command)
        self.assertIn('--preflight-only',command)
        self.assertNotIn('--privileged',command)
        self.assertFalse((self.base/'container-stage').exists())

    def test_container_preflight_does_not_build_missing_toolchain(self):
        import container_install as frontend
        from types import SimpleNamespace
        arguments=['container_install.py','--iso',str(self.iso),'--textures',str(self.pack),
                   '--output',str(self.base/'missing-container-stage'),'--preflight-only']
        with patch.object(frontend,'check_context'), patch.object(sys,'argv',arguments), patch.object(frontend.shutil,'which',return_value='/usr/bin/docker'), \
             patch.object(install,'EXPECTED_EBOOT',self.expected), \
             patch.object(frontend.subprocess,'run',return_value=SimpleNamespace(returncode=1)) as execute:
            with self.assertRaises(SystemExit) as error:
                frontend.main()
            self.assertEqual(error.exception.code,2)
        self.assertEqual(execute.call_count,1)
        self.assertEqual(execute.call_args.args[0][:3],['docker','image','inspect'])

    def test_dirty_or_wrong_dependency_is_preserved(self):
        repo = self.base / 'repos'
        dep = repo / 'upstream/Yakumo'
        dep.mkdir(parents=True)
        (repo / 'dependencies.lock.json').write_text(json.dumps({'repositories': {
            'yakumo': {'path': 'upstream/Yakumo', 'commit': '0' * 40, 'url':'https://invalid.example/source.git'}}}))
        with self.assertRaisesRegex(ValueError, 'dedicated Git'):
            install.dependency_status(repo)
        self.assertTrue(dep.is_dir())

    def test_only_owned_interrupted_dependency_fetch_can_resume(self):
        import fetch_dependencies as sources
        repo=self.base/'pending-repo'
        dep=repo/'upstream/Yakumo'
        dep.parent.mkdir(parents=True)
        subprocess.run(['git','init','-q',str(dep)],check=True)
        entry={'path':'upstream/Yakumo','commit':'0'*40,'url':'https://invalid.example/source.git'}
        with self.assertRaisesRegex(ValueError,'differs'):
            sources.inspect('yakumo',entry,repo)
        marker=repo/'out/data/dependency-fetch/yakumo.json'
        marker.parent.mkdir(parents=True)
        marker.write_text(json.dumps({'identity':entry,'status':'IN_PROGRESS'}))
        self.assertEqual(sources.inspect('yakumo',entry,repo),'pending')
        (dep/'unknown').write_text('preserve')
        with self.assertRaisesRegex(ValueError,'preserved'):
            sources.inspect('yakumo',entry,repo)
        self.assertEqual((dep/'unknown').read_text(),'preserve')

    def test_payload_transformation_rejects_unknown_source(self):
        with self.assertRaisesRegex(ValueError, 'source changed'):
            payload.sources('unknown policy source', 'unknown CRT')

    def test_existing_public_asset_is_verified_without_download_or_overwrite(self):
        path = self.base / 'font-source'
        path.write_bytes(b'existing input')
        spec = dict(bytes=3, sha256=hashlib.sha256(b'new').hexdigest(), url='https://invalid.example/')
        with patch.object(font.urllib.request, 'urlopen') as download:
            with self.assertRaisesRegex(ValueError, 'preserved'):
                font.fetch(path, spec)
            download.assert_not_called()
        self.assertEqual(path.read_bytes(), b'existing input')

    def test_texture_component_resume_preserves_source_and_identical_outputs(self):
        output = self.base / 'texture-stage'
        original = {p.name: p.read_bytes() for p in self.pack.iterdir()}
        textures.prepare(self.pack, output)
        prepared = {p.name: p.read_bytes() for p in output.iterdir()}
        textures.prepare(self.pack, output)
        self.assertEqual(prepared, {p.name: p.read_bytes() for p in output.iterdir()})
        self.assertEqual(original, {p.name: p.read_bytes() for p in self.pack.iterdir()})

    def test_different_texture_metadata_is_preserved(self):
        path = self.base / 'existing-metadata'
        path.write_bytes(b'original')
        with self.assertRaisesRegex(ValueError, 'preserved'):
            textures.write_text(path, 'different')
        self.assertEqual(path.read_bytes(), b'original')

    def test_codec_source_extraction_rejects_escape_before_writes(self):
        repo = self.base / 'codec-repo'
        data = repo / 'out/data/dependencies'
        data.mkdir(parents=True)
        archive = data / 'ffmpeg-fixture.tar.xz'
        with tarfile.open(archive, 'w:xz') as tar:
            member = tarfile.TarInfo('ffmpeg-fixture/../../outside')
            member.size = 4
            tar.addfile(member, io.BytesIO(b'test'))
        lock = {'libraries': {'ffmpeg': {'version': 'fixture', 'sha256': install.digest(archive)}}}
        (repo / 'dependencies.lock.json').write_text(json.dumps(lock))
        with patch.object(codecs, 'ROOT', repo):
            with self.assertRaisesRegex(ValueError, 'Unsafe'):
                codecs.main()
        self.assertEqual(list(data.iterdir()), [archive])


if __name__ == '__main__':
    unittest.main()
