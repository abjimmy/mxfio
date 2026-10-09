#!/usr/bin/env python3
"""Exercise destructive configfs preconditions without touching configfs."""
import copy
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    'nvmet_config', Path(__file__).resolve().parents[1] / 'scripts/nvmet_config.py')
nvmet = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nvmet)


class SnapshotGuard(unittest.TestCase):
    def setUp(self):
        self.before = {
            'objects': {'hosts': ['host'], 'subsystems': ['subsystem'], 'ports': ['1']},
            'attributes': {'subsystems/subsystem/namespaces/1/enable': '1'},
            'links': {'ports/1/subsystems/subsystem': 'subsystems/subsystem'},
        }

    def refuses_change(self, after):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / 'untouched'
            marker.write_text('original')
            with patch.object(nvmet, 'ROOT', Path(directory)), patch.object(nvmet, 'snapshot', return_value=after):
                with self.assertRaisesRegex(RuntimeError, 'changed since snapshot'):
                    nvmet.clear(self.before)
            self.assertEqual(marker.read_text(), 'original')

    def test_new_namespace_under_existing_subsystem(self):
        after = copy.deepcopy(self.before)
        after['attributes']['subsystems/subsystem/namespaces/2/enable'] = '1'
        self.refuses_change(after)

    def test_changed_host_acl(self):
        after = copy.deepcopy(self.before)
        after['links']['subsystems/subsystem/allowed_hosts/new-host'] = 'hosts/new-host'
        self.refuses_change(after)

    def test_changed_backing_device(self):
        after = copy.deepcopy(self.before)
        after['attributes']['subsystems/subsystem/namespaces/1/device_path'] = '/dev/another'
        self.refuses_change(after)


class RestoreLinks(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for name in ('hosts/host', 'subsystems/wanted', 'subsystems/other',
                     'ports/1/subsystems'):
            (self.root/name).mkdir(parents=True, exist_ok=True)
        root_patch = patch.object(nvmet, 'ROOT', self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)
        self.name = 'ports/1/subsystems/wanted'
        self.link = self.root/self.name
        self.marker = self.root/'subsystems/wanted/attr_allow_any_host'
        self.marker.write_text('0\n')
        self.data = {
            'objects': {'hosts': ['host'], 'subsystems': ['wanted'], 'ports': ['1']},
            'attributes': {'subsystems/wanted/attr_allow_any_host': '1'},
            'links': {self.name: 'subsystems/wanted'},
        }

    def rejects_conflict(self):
        with self.assertRaisesRegex(RuntimeError, 'configfs link'):
            nvmet.restore(self.data)
        self.assertEqual(self.marker.read_text(), '0\n')

    def test_different_target_rejected_before_attribute_changes(self):
        self.link.symlink_to(self.root/'subsystems/other')
        self.rejects_conflict()
        self.assertEqual(self.link.resolve(), self.root/'subsystems/other')

    def test_dangling_conflicting_link_rejected(self):
        self.link.symlink_to(self.root/'subsystems/missing')
        self.rejects_conflict()

    def test_nonlink_conflict_rejected(self):
        self.link.write_text('unrelated')
        self.rejects_conflict()
        self.assertEqual(self.link.read_text(), 'unrelated')

    def test_matching_relative_link_allows_repeated_restore(self):
        self.link.symlink_to('../../../subsystems/wanted')
        for _ in range(2):
            nvmet.restore(self.data)
        self.assertEqual(self.link.readlink(), Path('../../../subsystems/wanted'))
        self.assertEqual(self.marker.read_text(), '1\n')

    def test_partial_restore_creates_missing_link(self):
        nvmet.restore(self.data)
        self.assertEqual(self.link.resolve(), self.root/'subsystems/wanted')
        self.assertEqual(self.marker.read_text(), '1\n')


if __name__ == '__main__':
    unittest.main()
