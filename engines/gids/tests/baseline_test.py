#!/usr/bin/env python3
"""Reject incomplete or mismatched CPU baselines before comparing latencies."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from summarize_baseline import summarize


class BaselineTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'baseline-fio.json'
        self.document = {'fio version': 'fio-3.42', 'time': 'test', 'jobs': [{
            'error': 0, 'read': {'total_ios': 5200},
            'write': {'total_ios': 0}, 'job options': {}}]}
        self.rows = ['0, 1000000, 0, 4096, 1073741824\n'] * 200
        self.rows += ['1, 10000, 0, 4096, 1073741824\n'] * 5000

    def analyze(self):
        self.path.write_text('SPDK startup banner\n' + json.dumps(self.document))
        self.path.with_name('baseline_lat.log').write_text(''.join(self.rows))
        return summarize(self.path)

    def test_units_and_warmup(self):
        result = self.analyze()
        self.assertEqual(result['samples'], 5000)
        self.assertEqual(result['metrics']['mean'], 10)
        self.assertEqual(result['metrics']['p99'], 10)

    def test_reject_failed_or_incomplete_job(self):
        for field, key, value in ((None, 'error', 5), ('read', 'total_ios', 5199),
                                  ('write', 'total_ios', 1)):
            with self.subTest(field=field), self.assertRaises(ValueError):
                job = self.document['jobs'][0]
                target = job if field is None else job[field]
                old = target[key]
                target[key] = value
                try:
                    self.analyze()
                finally:
                    target[key] = old

    def test_reject_incomplete_log(self):
        self.rows.pop()
        with self.assertRaises(ValueError):
            self.analyze()

    def test_reject_wrong_endpoint_even_in_warmup(self):
        original = self.rows[0]
        for old, new in (('1073741824', '0'), ('4096', '8192'),
                         (', 0, 4096', ', 1, 4096'), ('1000000', '0')):
            with self.subTest(change=new), self.assertRaises(ValueError):
                self.rows[0] = original.replace(old, new)
                self.analyze()


if __name__ == '__main__':
    unittest.main()
