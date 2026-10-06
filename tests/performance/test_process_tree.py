"""Meaningful sampler control: actual child/grandchild RSS and reparenting."""
import subprocess
import sys
import time
import unittest

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
        monitor = TreeMonitor(process.pid, interval=0.02).start()
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


if __name__ == '__main__':
    unittest.main()
