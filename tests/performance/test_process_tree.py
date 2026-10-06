"""Meaningful sampler control: actual child/grandchild RSS and reparenting."""
import subprocess
import os
import sys
import time
import unittest
from unittest.mock import patch
import psutil

from process_tree import TreeMonitor


class ProcessTreeTests(unittest.TestCase):
    def test_descendants_reparenting_and_simultaneous_totals(self):
        grandchild = 'import time; payload = b"g" * (24 * 1024 * 1024); time.sleep(2.5)'
        child = ('import subprocess,sys,time; payload = b"c" * (20 * 1024 * 1024); '
                 f'subprocess.Popen([sys.executable,"-c",{grandchild!r}], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); '
                 'time.sleep(2.5)')
        parent = ('import subprocess,sys,time; payload = b"p" * (8 * 1024 * 1024); '
                  f'subprocess.Popen([sys.executable,"-c",{child!r}], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); '
                  'time.sleep(1.5)')
        process = subprocess.Popen([sys.executable, '-c', parent])
        monitor = TreeMonitor(process.pid, interval=0.02, root_resources=True).start()
        try:
            self.assertEqual(process.wait(timeout=10), 0)
            deadline = time.monotonic() + 10
            while monitor.live_descendants() and time.monotonic() < deadline:
                time.sleep(0.05)
            result = monitor.finish()
            self.assertEqual(result['processIdentities'], 3)
            self.assertTrue(any(s['processes'] == 3 and s['childResidentSumBytes'] >= 44 * 1024 * 1024 for s in result['samples']))
            self.assertTrue(any(s['rootResidentBytes'] == 0 and s['processes'] >= 1 for s in result['samples']), 'Reparented descendants must still be counted')
            self.assertTrue(all(s['residentSumBytes'] == s['rootResidentBytes'] + s['childResidentSumBytes'] for s in result['samples']))
            self.assertEqual(monitor.live_descendants(), [])
            self.assertTrue(any(s['rootThreads'] and s['rootHandlesOrFds'] for s in result['samples']))
        finally:
            monitor.stop.set()
            monitor.thread.join(timeout=10)
            for identity, owned in monitor.known.items():
                try:
                    if owned.is_running() and owned.create_time() == identity[1]:
                        owned.kill()
                except Exception:
                    pass
            process.wait(timeout=10)


    def test_resource_denial_during_exit_is_distinct_from_live_permission_failure(self):
        for exiting in (False, True):
            with self.subTest(exiting=exiting):
                process = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
                try:
                    monitor = TreeMonitor(process.pid, root_resources=True)
                    owned = monitor.known[monitor.root]
                    method = 'num_handles' if os.name == 'nt' else 'num_fds'
                    with patch.object(owned, method, side_effect=psutil.AccessDenied(process.pid)):
                        monitor._sample()
                    self.assertTrue(monitor.samples)
                    self.assertIsNone(monitor.samples[0]['rootHandlesOrFds'])
                    if exiting:
                        process.terminate(); process.wait(timeout=10)
                    monitor._resolve_resource_denials()
                    if exiting:
                        self.assertEqual(monitor.errors, [])
                        self.assertGreater(monitor.races, 0)
                    else:
                        self.assertTrue(any('live process' in error for error in monitor.errors))
                finally:
                    if process.poll() is None:
                        process.terminate(); process.wait(timeout=10)


if __name__ == '__main__':
    unittest.main()
