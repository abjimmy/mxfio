#!/usr/bin/env python3
"""Check the analysis rejects measurements that cannot support a latency claim."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'summarize_ap', Path(__file__).resolve().parents[1] / 'scripts/summarize_ap.py')
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)

VALID = '''# ticks_per_us_before=100
sample,offset,bytes,status,sq_begin,doorbell,cq_seen,first_read,all_read,mismatches
0,1073741824,4096,0,100,200,1000,1100,2100,0
1,1073741824,4096,0,3000,3100,4900,5000,6000,0
# ticks_per_us_after=100
'''


class ProfileTests(unittest.TestCase):
    def analyze(self, text=VALID, warmup=0):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'samples.csv'
            path.write_text(text)
            return analysis.summarize(path, warmup)

    def test_clock_conversion_and_percentiles(self):
        result = self.analyze()
        first = result['metrics']['sqe_to_first_load']
        self.assertEqual(first['mean'], 15)
        self.assertEqual(first['p50'], 15)
        self.assertEqual(first['p95'], 19.5)
        self.assertEqual(first['p99'], 19.9)
        self.assertEqual(result['total_samples'], 2)

    def test_warmup_is_excluded_from_statistics(self):
        result = self.analyze(warmup=1)
        self.assertEqual(result['samples'], 1)
        self.assertEqual(result['metrics']['sqe_to_first_load']['mean'], 20)

    def test_bad_warmup(self):
        for count in (-1, 2, 3):
            with self.subTest(count=count), self.assertRaises(ValueError):
                self.analyze(warmup=count)

    def test_even_warmup_payload_errors_invalidate_run(self):
        for bad in ('0,1073741824,4096,5,100,200,1000,1100,2100,0',
                    '0,1073741824,4096,0,100,200,1000,1100,2100,1'):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                self.analyze(VALID.replace(VALID.splitlines()[2], bad), warmup=1)

    def test_missing_end_calibration(self):
        with self.assertRaises(KeyError):
            self.analyze(VALID.replace('# ticks_per_us_after=100', ''))
        with self.assertRaises(ValueError):
            self.analyze(VALID + '# end_calibration_error=failed I/O\n')

    def test_invalid_clock(self):
        for value in ('0', '-1', 'nan', 'inf', '103'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.analyze(VALID.replace('ticks_per_us_after=100', 'ticks_per_us_after=' + value))
        with self.assertRaises(ValueError):
            self.analyze(VALID.replace('ticks_per_us_before=100', 'ticks_per_us_before=0'))

    def test_missing_or_reordered_timestamp(self):
        for stamps in ('0,200,1000,1100,2100', '100,200,1200,1100,2100'):
            with self.subTest(stamps=stamps), self.assertRaises(ValueError):
                self.analyze(VALID.replace('100,200,1000,1100,2100', stamps))

    def test_mixed_blocks_and_missing_samples(self):
        for prefix in ('2,1073741824,4096', '1,1073745920,4096', '1,1073741824,8192'):
            with self.subTest(prefix=prefix), self.assertRaises(ValueError):
                self.analyze(VALID.replace('1,1073741824,4096', prefix))

    def test_random_plan_addresses_and_incomplete_replay(self):
        planned = '# access=planned_random\n# operation=read\n# expected_samples=2\n# namespace_bytes=2147483648\n' + VALID
        planned = planned.replace('1,1073741824,4096', '1,4096,4096')
        result = self.analyze(planned)
        self.assertIsNone(result['offset'])
        self.assertEqual(result['address']['unique_offsets'], 2)
        self.assertEqual(sum(result['address']['decile_counts']), 2)
        for bad in (planned.replace('expected_samples=2', 'expected_samples=3'),
                    planned.replace('1,4096,4096', '1,2147483648,4096'),
                    planned.replace('1,4096,4096', '1,1,4096')):
            with self.assertRaises(ValueError):
                self.analyze(bad)

    def test_write_endpoint_and_readback_failure(self):
        write = '''# ticks_per_us_before=100
# ticks_per_us_after=100
# access=planned_random
# operation=write
# expected_samples=1
# namespace_bytes=16384
# write_readback=1
sample,offset,bytes,status,sq_begin,doorbell,cq_seen,first_read,all_read,mismatches,ap_done
0,4096,4096,0,100,200,1000,0,0,0,1200
'''
        metrics = self.analyze(write)['metrics']
        self.assertEqual(metrics['sqe_to_cq']['mean'], 9)
        self.assertEqual(metrics['sqe_to_ap_done']['mean'], 11)
        self.assertNotIn('sqe_to_first_load', metrics)
        for bad in (write.replace('write_readback=1', 'write_readback=0'),
                    write.replace('0,0,0,1200', '0,0,1,1200'),
                    write.replace('0,0,0,1200', '0,0,0,900'),
                    write.replace('0,4096,4096,0,', '0,4096,4096,3,')):
            with self.assertRaises(ValueError):
                self.analyze(bad)

    TIMED = '''# ticks_per_us_before=100
sample,offset,bytes,status,sq_begin,doorbell,cq_seen,cqe_observed,slot_reclaimed,first_read,all_read,mismatches
0,1073741824,4096,0,100,200,1000,1010,1060,1500,2100,0
1,1073741824,4096,0,3000,3100,4900,4910,4960,5400,6000,0
# ticks_per_us_after=100
'''

    def test_entry_timing_split(self):
        metrics = self.analyze(self.TIMED)['metrics']
        self.assertEqual(metrics['cq_seen_to_cqe_observed']['mean'], 0.1)
        self.assertEqual(metrics['cqe_to_slot_reclaimed']['mean'], 0.5)
        self.assertEqual(metrics['slot_reclaimed_to_first_load']['mean'], 4.4)
        # The split must decompose the pre-existing whole-interval metric.
        self.assertAlmostEqual(
            metrics['cq_to_first_load']['mean'],
            metrics['cq_seen_to_cqe_observed']['mean']
            + metrics['cqe_to_slot_reclaimed']['mean']
            + metrics['slot_reclaimed_to_first_load']['mean'])

    def test_entry_timing_nonmonotonic_rejected(self):
        for stamps in ('1000,1060,1010,1500', '1000,1010,1060,1000'):
            with self.subTest(stamps=stamps), self.assertRaises(ValueError):
                self.analyze(self.TIMED.replace('1000,1010,1060,1500', stamps))

    def test_entry_timing_zero_columns_fall_back(self):
        # RDMA leaves both columns zero in every sample.
        rdma = '# transport=rdma\n' + self.TIMED.replace(
            '1000,1010,1060', '1000,0,0').replace('4900,4910,4960', '4900,0,0')
        metrics = self.analyze(rdma)['metrics']
        self.assertNotIn('cqe_to_slot_reclaimed', metrics)
        self.assertEqual(metrics['sqe_to_first_load']['mean'], 19)

    def test_entry_timing_partial_or_mixed_samples_rejected(self):
        for stamps in ('1000,0,1060', '1000,1010,0', '1000,0,0',
                       '1000,-1,1060'):
            bad = self.TIMED.replace('1000,1010,1060', stamps)
            for warmup in (0, 1):
                with self.subTest(stamps=stamps, warmup=warmup), self.assertRaises(ValueError):
                    self.analyze(bad, warmup=warmup)

    def test_missing_stamp_cannot_hide_other_nonmonotonic_timing(self):
        bad = self.TIMED.replace('1000,1010,1060', '1000,0,1060')
        bad = bad.replace('4900,4910,4960', '4900,4910,4800')
        with self.assertRaises(ValueError):
            self.analyze(bad)

    def test_entry_timing_requires_both_columns(self):
        for name, values in (('cqe_observed', ('1010', '4910')),
                             ('slot_reclaimed', ('1060', '4960'))):
            bad = self.TIMED.replace(name + ',', '')
            for value in values:
                bad = bad.replace(',' + value + ',', ',')
            with self.subTest(column=name), self.assertRaises(ValueError):
                self.analyze(bad)

    def test_pcie_with_timing_columns_cannot_fall_back_to_zero(self):
        bad = '# transport=pcie\n' + self.TIMED.replace(
            '1000,1010,1060', '1000,0,0').replace('4900,4910,4960', '4900,0,0')
        with self.assertRaises(ValueError):
            self.analyze(bad)
        # PCIe archives predating per-entry timing remain valid.
        self.analyze('# transport=pcie\n' + VALID)

    def test_entry_timing_write_has_no_read_split(self):
        write = '''# ticks_per_us_before=100
# ticks_per_us_after=100
# access=planned_random
# operation=write
# expected_samples=1
# namespace_bytes=16384
# write_readback=1
sample,offset,bytes,status,sq_begin,doorbell,cq_seen,cqe_observed,slot_reclaimed,first_read,all_read,mismatches,ap_done
0,4096,4096,0,100,200,1000,1010,1060,0,0,0,1200
'''
        metrics = self.analyze(write)['metrics']
        self.assertEqual(metrics['cqe_to_slot_reclaimed']['mean'], 0.5)
        self.assertNotIn('slot_reclaimed_to_first_load', metrics)


if __name__ == '__main__':
    unittest.main()
