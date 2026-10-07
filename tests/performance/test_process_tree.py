"""Meaningful sampler control: actual child/grandchild RSS and reparenting."""
import subprocess
import os
import sys
import time
import unittest
import tempfile
from pathlib import Path
from unittest.mock import patch
import psutil

from process_tree import TreeMonitor
from workflow_memory import SampleAcknowledgements, MEMORY_PHASES


class ProcessTreeTests(unittest.TestCase):
    def test_phase_change_during_sweep_is_not_relabelled_or_acknowledged(self):
        process = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
        phases = iter(('review', 'create'))
        acknowledgements = []
        monitor = TreeMonitor(process.pid, lambda: next(phases), on_sample=acknowledgements.append)
        try:
            monitor._sample()
            self.assertEqual(monitor.samples, [])
            self.assertEqual(acknowledgements, [])
            monitor.phase = lambda: 'create'
            monitor._sample()
            self.assertEqual([sample['phase'] for sample in monitor.samples], ['create'])
            self.assertEqual(len(acknowledgements), 1)
            self.assertGreater(acknowledgements[0]['residentSumBytes'], 0)
        finally:
            process.terminate()
            process.wait(timeout=10)

    def test_slow_sampler_releases_each_checkpoint_only_after_a_real_sweep(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            log = directory / 'phases.log'
            acknowledgements = SampleAcknowledgements(directory / 'acks')
            producer = '''import pathlib,sys,time
log,acks=map(pathlib.Path,sys.argv[1:])
payload=b'x'*(1024*1024)
for phase in ('idle','scan','review','create','completed','cleared'):
    with log.open('a') as stream: stream.write('MEMORY '+phase+'\\n')
    deadline=time.monotonic()+10
    while not (acks/('sampled-'+phase)).exists():
        if time.monotonic()>deadline: raise RuntimeError('No sampler acknowledgement')
        time.sleep(0.01)
'''
            process = subprocess.Popen([sys.executable, '-c', producer, str(log), str(acknowledgements.directory)])
            def phase():
                return log.read_text().splitlines()[-1][7:] if log.exists() and log.stat().st_size else 'startup'
            # Slower than the GUI's 300 ms delay: each producer phase must wait.
            monitor = TreeMonitor(process.pid, phase, interval=0.4, on_sample=acknowledgements).start()
            try:
                self.assertEqual(process.wait(timeout=10), 0)
                result = monitor.finish()
                for expected in MEMORY_PHASES:
                    self.assertIn(expected, result['phases'])
                    self.assertGreater(result['phases'][expected]['samples'], 0)
                    self.assertGreater(result['phases'][expected]['peakResidentSumBytes'], 0)
                    self.assertTrue((acknowledgements.directory / ('sampled-' + expected)).exists())
                before = len(monitor.samples)
                with self.assertRaisesRegex(TimeoutError, 'not-observed'):
                    monitor.wait_for_phase('not-observed', timeout=0.01)
                self.assertEqual(len(monitor.samples), before)
            finally:
                monitor.stop.set()
                monitor.thread.join(timeout=10)
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=10)

    def test_acknowledgement_failure_rejects_measurement_evidence(self):
        process = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
        def denied(_sample):
            raise PermissionError('Cannot acknowledge sample')
        monitor = TreeMonitor(process.pid, on_sample=denied).start()
        try:
            monitor.thread.join(timeout=5)
            self.assertFalse(monitor.thread.is_alive())
            with self.assertRaisesRegex(RuntimeError, 'Cannot acknowledge sample'):
                monitor.finish()
        finally:
            process.terminate()
            process.wait(timeout=10)

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
