#!/usr/bin/env python3
"""Check 3.42 mcFile command generation while intercepting every I/O command."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which('bash'), 'bash is required for command-generation checks')
class BaselineCommandTests(unittest.TestCase):
    def command(self, mode, environment=None):
        script = Path(__file__).resolve().parents[1] / 'scripts/run_baseline.sh'
        # Both external commands are shell functions: no fio, taskset, or
        # storage access can occur in this test.
        harness = '''
taskset() { printf '%s\\0' "LD_LIBRARY_PATH=$LD_LIBRARY_PATH" "$@"; }
python3() { :; }
source "$1" "$2" /dev/test-only 4k "$3"
'''
        with tempfile.TemporaryDirectory() as directory:
            env = dict(os.environ, FIO_ROOT='/test/fio', BASELINE_GPU='2')
            env.pop('MACA_PATH', None)
            env.pop('LD_LIBRARY_PATH', None)
            if environment:
                env.update(environment)
            result = subprocess.run(['bash', '-c', harness, 'test', script.as_posix(), mode,
                                     (Path(directory) / 'output').as_posix()],
                                    env=env, capture_output=True, check=True)
        return result.stdout.decode().split('\0')

    def check_mode(self, mode, expected):
        args = self.command(mode)
        self.assertIn('--ioengine=libmcfile', args)
        self.assertIn('--maca_io=' + expected, args)
        self.assertIn('--gpu_dev_ids=2', args)
        self.assertIn('--readonly', args)
        self.assertFalse(any(arg.startswith('--cuda_io') for arg in args))

    def test_posix(self):
        self.check_mode('posix', 'posix')

    def test_mcfile(self):
        self.check_mode('mcfile', 'mcfile')

    def test_legacy_mode_alias(self):
        self.check_mode('cufile', 'mcfile')

    def test_selected_sdk_precedes_inherited_library_paths(self):
        args = self.command('mcfile', {'MACA_PATH': '/test/maca390',
                                      'LD_LIBRARY_PATH': '/existing/lib:/opt/maca/lib'})
        self.assertEqual(args[0], 'LD_LIBRARY_PATH=/test/maca390/lib:/opt/mxdriver/lib:'
                                 '/existing/lib:/opt/maca/lib')

    def test_default_sdk_when_unset_or_empty(self):
        for environment in ({}, {'MACA_PATH': ''}):
            with self.subTest(environment=environment):
                args = self.command('mcfile', environment)
                self.assertEqual(args[0], 'LD_LIBRARY_PATH=/opt/maca/lib:/opt/mxdriver/lib')


if __name__ == '__main__':
    unittest.main()
