"""Sample simultaneous resident memory of a task-owned process tree.

RSS/working-set sums can count shared pages more than once. Linux PSS is also
recorded when available; Windows private commit is not physical resident RAM.
Sampling cannot see every short-lived process or every inter-sample peak.
"""
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time

import psutil


class Workflow:
    def __init__(self, executable, config, stderr):
        self.process = subprocess.Popen([str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=stderr, text=True, encoding='utf-8')
        self.responses = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.process.stdin.write(json.dumps(config) + '\n')
        self.process.stdin.flush()
        self.receive()

    def _read(self):
        for line in self.process.stdout:
            self.responses.put(line)
        self.responses.put(None)

    def receive(self, timeout=120):
        try:
            line = self.responses.get(timeout=timeout)
        except queue.Empty as error:
            raise TimeoutError('Workflow response deadline exceeded') from error
        if line is None:
            raise RuntimeError(f'Workflow exited before a response: {self.process.poll()}')
        response = json.loads(line)
        if not response['ok']:
            raise RuntimeError(response['error'])
        return response.get('result', response)

    def call(self, operation, timeout=120, **values):
        self.process.stdin.write(json.dumps({'operation': operation, **values}) + '\n')
        self.process.stdin.flush()
        return self.receive(timeout)

    def finish(self):
        self.call('exit')
        self.process.stdin.close()
        if self.process.wait(timeout=30):
            raise RuntimeError('Workflow failed during shutdown')
        self.reader.join(timeout=5)
        self.process.stdout.close()


class TreeMonitor:
    def __init__(self, pid, phase=lambda: 'workflow', interval=0.05):
        root = psutil.Process(pid)
        self.root = (pid, root.create_time())
        self.known = {self.root: root}
        self.discovered = {self.root: {'pid': pid, 'created': self.root[1], 'name': root.name()}}
        self.phase = phase
        self.interval = interval
        self.samples = []
        self.errors = []
        self.races = 0
        self.started = time.monotonic()
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()
        return self

    def _sample(self):
        # Remember identities after discovery, so reparented children remain
        # included and a reused PID can never become part of this workload.
        for identity, process in list(self.known.items()):
            try:
                if process.create_time() != identity[1] or not process.is_running():
                    continue
                for child in process.children(recursive=True):
                    key = (child.pid, child.create_time())
                    if child.is_running():
                        self.known[key] = child
                        if key not in self.discovered:
                            self.discovered[key] = {'pid': child.pid, 'created': key[1], 'name': child.name()}
            except (psutil.NoSuchProcess, psutil.ZombieProcess):
                self.races += 1
            except psutil.AccessDenied:
                self.errors.append(f'Cannot enumerate descendants of {identity[0]}')
        resident = root_resident = private = pss = 0
        count = 0
        pss_complete = os.name != 'nt'
        for identity, process in list(self.known.items()):
            try:
                if not process.is_running() or process.create_time() != identity[1] or process.status() == psutil.STATUS_ZOMBIE:
                    continue
                info = process.memory_info()
                resident += info.rss
                if identity == self.root:
                    root_resident = info.rss
                private += getattr(info, 'private', 0)
                count += 1
                if os.name != 'nt':
                    try:
                        pss += process.memory_full_info().pss
                    except (psutil.AccessDenied, AttributeError):
                        pss_complete = False
            except (psutil.NoSuchProcess, psutil.ZombieProcess):
                self.races += 1
                pss_complete = False
            except psutil.AccessDenied:
                self.errors.append(f'Cannot read resident memory of {identity[0]}')
        if count:
            self.samples.append({'tMs': round((time.monotonic() - self.started) * 1000, 2),
                                 'phase': self.phase(), 'processes': count, 'residentSumBytes': resident,
                                 'rootResidentBytes': root_resident, 'childResidentSumBytes': resident - root_resident,
                                 'privateCommitBytes': private if os.name == 'nt' else None,
                                 'pssBytes': pss if pss_complete else None})

    def _run(self):
        try:
            while not self.stop.is_set():
                self._sample()
                self.stop.wait(self.interval)
        except Exception as error:
            self.errors.append(str(error))

    def finish(self):
        self.stop.set()
        self.thread.join(timeout=10)
        if self.thread.is_alive():
            raise RuntimeError('Process-tree sampler did not stop')
        if not self.samples or self.errors:
            raise RuntimeError(f'Incomplete process-tree evidence: {self.errors[:5]}')
        phases = {}
        for phase in dict.fromkeys(s['phase'] for s in self.samples):
            values = [s for s in self.samples if s['phase'] == phase]
            peak = max(values, key=lambda s: s['residentSumBytes'])
            phases[phase] = {'samples': len(values), 'peakResidentSumBytes': peak['residentSumBytes'],
                             'rootResidentAtPeakBytes': peak['rootResidentBytes'],
                             'childResidentAtPeakBytes': peak['childResidentSumBytes'],
                             'processesAtPeak': peak['processes'], 'maxProcesses': max(s['processes'] for s in values),
                             'peakPssBytes': max((s['pssBytes'] for s in values if s['pssBytes'] is not None), default=None),
                             'peakPrivateCommitBytes': max((s['privateCommitBytes'] for s in values if s['privateCommitBytes'] is not None), default=None)}
        return {'intervalMs': self.interval * 1000, 'processIdentities': len(self.known),
                'discoveredProcesses': list(self.discovered.values()),
                'transientProcessRaces': self.races, 'measurementErrors': [], 'phases': phases, 'samples': self.samples}

    def live_descendants(self):
        alive = []
        for identity, process in self.known.items():
            if identity == self.root:
                continue
            try:
                if process.create_time() == identity[1] and process.is_running() and process.status() != psutil.STATUS_ZOMBIE:
                    alive.append(identity[0])
            except psutil.NoSuchProcess:
                pass
        return alive


def wait_terminal(workflow, timeout=120):
    deadline = time.monotonic() + timeout
    while True:
        snapshot = workflow.call('snapshot', timeout=5)
        if snapshot['job']['state'] in ('Succeeded', 'SucceededWithWarnings', 'Failed', 'Cancelled'):
            return snapshot
        if time.monotonic() >= deadline:
            raise TimeoutError('Job did not terminate')
        time.sleep(0.05)
