#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise actual metadata-only code on isolated Linux trees, never the console."""
import hashlib
import os
import pathlib
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

PROGRAM = pathlib.Path(sys.argv.pop(1)).resolve()
FAULTS = pathlib.Path(sys.argv.pop(1)).resolve()
TITLE_ID = os.environ.get('MHP3RD_TITLE_ID', 'PPSA99373')
SUCCESS_TOKEN = os.environ.get('MHP3RD_PERMISSION_SUCCESS', 'PERMISOS LISTOS')
CHANGE_TOKEN = os.environ.get('MHP3RD_PERMISSION_CHANGE', 'cambios=')


class PermissionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='yakumo-permissions-', dir='/tmp')
        self.base = pathlib.Path(self.temp.name)
        self.root = self.base/TITLE_ID
        self.other = self.base/'PPSA00000'
        self.other.mkdir()
        (self.other/'sentinel').write_bytes(b'never touch')

    def tearDown(self):
        self.temp.cleanup()

    def fixture(self):
        for name in ('eboot.bin', 'sce_module/libc.prx', 'sce_sys/param.json',
                     'yakumo/title.id', 'yakumo/game/EBOOT.ELF', 'yakumo/game/game.iso',
                     'yakumo/saves/ms0/save.dat', 'yakumo/config/preferences.ini',
                     'yakumo/logs/game-first-frame.log', 'yakumo/cache/radv/cache',
                     'yakumo/cache/first-frame/cache', 'yakumo/textures/NPJB40001/texture'):
            p = self.root/name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(('preserve '+name).encode())
            p.chmod(0o640)
        (self.root/'yakumo/title.id').write_text(TITLE_ID+'\n')
        (self.root/'sce_sys/param.json').write_text('{"titleId":"'+TITLE_ID+'"}')
        for p in [self.root, *self.root.rglob('*')]:
            if p.is_dir(): p.chmod(0o750)

    def snapshot(self):
        result = {}
        for p in [self.base, *self.base.rglob('*')]:
            s = p.lstat()
            result[str(p.relative_to(self.base))] = (
                stat.S_IMODE(s.st_mode), s.st_uid, s.st_gid, s.st_ino,
                hashlib.sha256(p.read_bytes()).hexdigest() if stat.S_ISREG(s.st_mode) else None)
        return result

    def run_payload(self, fault=None, root=None, uid=None):
        env = os.environ.copy()
        if fault: env.update(LD_PRELOAD=str(FAULTS), YAKUMO_TEST_FAULT=fault, YAKUMO_TEST_ROOT=str(self.root))
        return subprocess.run([str(PROGRAM), str(root or self.root)], capture_output=True,
                              text=True, timeout=10, env=env, user=uid)

    def assert_rejected_unchanged(self, **kwargs):
        before = self.snapshot()
        result = self.run_payload(**kwargs)
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(SUCCESS_TOKEN, result.stderr)
        self.assertEqual(before, self.snapshot(), result.stderr)
        return result

    def test_merge_preserves_all_content_and_is_idempotent_under_strict_umask(self):
        self.fixture()
        before = self.snapshot()
        mask = os.umask(0o077)
        try: result = self.run_payload()
        finally: os.umask(mask)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(SUCCESS_TOKEN, result.stderr)
        after = self.snapshot()
        self.assertEqual(before.keys(), after.keys())
        for name, value in before.items():
            self.assertEqual(value[1:], after[name][1:], name)
            if name == TITLE_ID or name.startswith(TITLE_ID+'/'):
                p = self.base/name
                expected = 0o777 if p.is_dir() or name in (
                    TITLE_ID+'/eboot.bin', TITLE_ID+'/sce_module/libc.prx') else 0o666
                self.assertEqual(after[name][0], expected, name)
            else: self.assertEqual(value, after[name])
        result = self.run_payload()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(result.stderr, r'\b0 (?:changes|cambios)\b')
        self.assertEqual(after, self.snapshot())

    def test_clean_copied_title_without_persistent_files_or_iso(self):
        self.fixture()
        for name in ('saves', 'config', 'cache', 'logs', 'textures'):
            shutil.rmtree(self.root/'yakumo'/name)
        (self.root/'yakumo/game/game.iso').unlink()
        result = self.run_payload()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.root/'yakumo/logs').exists())

    def test_absent_incomplete_or_wrong_identity_never_bootstraps(self):
        self.assert_rejected_unchanged()
        self.fixture()
        for name, content in (
            ('yakumo/title.id', b'PPSA00000\n'),
            ('yakumo/title.id', (TITLE_ID+'\0other').encode()),
            ('sce_sys/param.json', b'{"titleId":"PPSA00000"}'),
            ('sce_sys/param.json', ('{"titleId":"'+TITLE_ID+'","titleId":"PPSA00000"}').encode()),
            ('sce_sys/param.json', ('{"titleId":"'+TITLE_ID+'"suffix}').encode())):
            p = self.root/name; old = p.read_bytes(); p.write_bytes(content)
            self.assert_rejected_unchanged(); p.write_bytes(old)
        for name in ('yakumo/title.id', 'sce_sys/param.json', 'eboot.bin',
                     'sce_module/libc.prx', 'yakumo/game/EBOOT.ELF'):
            p = self.root/name; old = p.read_bytes(); p.unlink()
            self.assert_rejected_unchanged(); p.write_bytes(old)

    def test_links_special_files_and_unsafe_names_reject_before_any_chmod(self):
        self.fixture()
        for kind in ('symlink', 'dangling', 'hardlink', 'fifo', 'backslash', 'control'):
            with self.subTest(kind=kind):
                p = self.root/('bad\\name' if kind == 'backslash' else 'bad\nname' if kind == 'control' else 'zz-bad')
                if kind == 'symlink': p.symlink_to(self.other, target_is_directory=True)
                elif kind == 'dangling': p.symlink_to(self.base/'absent')
                elif kind == 'hardlink': os.link(self.other/'sentinel', p)
                elif kind == 'fifo': os.mkfifo(p)
                else: p.write_bytes(b'unsafe')
                self.assert_rejected_unchanged(); p.unlink()

    def test_ancestor_and_title_links_or_unsafe_root_reject(self):
        self.fixture()
        alias = self.base/'alias'; alias.symlink_to(self.base, target_is_directory=True)
        self.assert_rejected_unchanged(root=alias/TITLE_ID)
        alias.unlink()
        for suffix in ('/../'+TITLE_ID, '//yakumo', '/'):
            self.assert_rejected_unchanged(root=str(self.root)+suffix)

    def test_unknown_metadata_and_failed_or_ignored_chmod_never_pass(self):
        self.fixture()
        for fault in ('unknown-links', 'unknown-inode', 'denied-chmod', 'ignored-chmod'):
            with self.subTest(fault=fault): self.assert_rejected_unchanged(fault=fault)

    def test_depth_guard_before_chmod(self):
        self.fixture(); p = self.root
        for _ in range(18): p = p/'deep'; p.mkdir()
        self.assert_rejected_unchanged()

    def test_partial_failure_reports_error_and_safe_retry_finishes(self):
        self.fixture(); before = self.snapshot()
        result = self.run_payload(fault='partial-chmod')
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn(SUCCESS_TOKEN, result.stderr)
        self.assertIn(CHANGE_TOKEN+'1', result.stderr)
        for name, value in before.items(): self.assertEqual(value[1:], self.snapshot()[name][1:])
        result = self.run_payload()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_membership_change_during_repair_never_returns_success(self):
        self.fixture()
        result = self.run_payload(fault='late-file')
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn(SUCCESS_TOKEN, result.stderr)
        self.assertIn('Stale file handle', result.stderr)

    def test_exact_policy_removes_special_bits_without_altering_bytes(self):
        self.fixture()
        (self.root/'eboot.bin').chmod(0o4777)
        (self.root/'yakumo/game/EBOOT.ELF').chmod(0o4755)
        (self.root/'yakumo').chmod(0o3777)
        result = self.run_payload()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(stat.S_IMODE((self.root/'eboot.bin').stat().st_mode), 0o777)
        self.assertEqual(stat.S_IMODE((self.root/'yakumo/game/EBOOT.ELF').stat().st_mode), 0o666)
        self.assertEqual(stat.S_IMODE((self.root/'yakumo').stat().st_mode), 0o777)

    @unittest.skipUnless(os.geteuid() == 0, 'requires root on isolated Linux fixtures')
    def test_foreign_owner_blocks_before_changes_and_root_repairs_without_chown(self):
        self.fixture()
        self.base.chmod(0o755)
        # Allow UID 1 to inspect, but deliberately leave root-owned wrong modes.
        for p in [self.root, *self.root.rglob('*')]:
            p.chmod(0o755 if p.is_dir() else 0o644)
        denied = self.assert_rejected_unchanged(uid=1)
        self.assertIn('owner preflight', denied.stderr)
        target = self.root/'yakumo/logs/game-first-frame.log'
        os.chown(target, 1, 1)
        result = self.run_payload()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((target.stat().st_uid, target.stat().st_gid), (1, 1))


if __name__ == '__main__': unittest.main(verbosity=2)
