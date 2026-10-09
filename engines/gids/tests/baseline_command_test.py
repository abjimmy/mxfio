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
    def check_mode(self, mode, expected):
        script = Path(__file__).resolve().parents[1] / 'scripts/run_baseline.sh'
        # Both external commands are shell functions: no fio, taskset, or
        # storage access can occur in this test.
        harness = '''
taskset() { printf '%s\\0' "$@"; }
python3() { :; }
source "$1" "$2" /dev/test-only 4k "$3"
'''
        with tempfile.TemporaryDirectory() as directory:
            env = dict(os.environ, FIO_ROOT='/test/fio', BASELINE_GPU='2')
            result = subprocess.run(['bash', '-c', harness, 'test', script.as_posix(), mode,
                                     (Path(directory) / 'output').as_posix()],
                                    env=env, capture_output=True, check=True)
        args = result.stdout.decode().split('\0')
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


if __name__ == '__main__':
    unittest.main()
