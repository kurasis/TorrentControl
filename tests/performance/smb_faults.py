"""Real Linux CIFS/Samba fault evidence in an isolated network namespace.

Requires a disposable root-capable Linux runner with samba, cifs-utils and
iproute2. No public server, user credentials or machine-wide SMB service is
used. A test gate coordinates fault injection BEFORE the native operation;
assertions about pending I/O run only AFTER that gate has been released.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import uuid

from process_tree import Workflow, wait_terminal


def run(*command, timeout=30):
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'{command[0]} failed: {result.stderr.strip()}')
    return result.stdout.strip()


class SmbFixture:
    def __init__(self, root):
        self.root = root
        suffix = uuid.uuid4().hex[:8]
        self.namespace = 'tc-smb-' + suffix
        self.host_if, self.peer_if = 'smbh' + suffix, 'smbp' + suffix
        subnet = int(suffix[:2], 16) % 200 + 1
        self.host_ip, self.server_ip = f'198.18.{subnet}.1', f'198.18.{subnet}.2'
        self.mounted = self.namespace_created = self.link_created = False
        self.server = self.server_log = None
        self.mount = root / 'client'
        self.mount.mkdir()
        root.chmod(0o755)
        self.backing = root / 'payload'
        self.backing.mkdir(mode=0o755)
        self.source = self.backing / 'payload.bin'
        with self.source.open('wb') as out:
            for _ in range(64):
                out.write(bytes(range(256)) * 4096)
        self.source.chmod(0o644)
        for name in ('state', 'lock', 'private', 'cache', 'pid', 'ncalrpc'):
            (root / name).mkdir()
        self.config = root / 'smb.conf'
        self.config.write_text(f'''[global]
server role = standalone server
netbios name = TC-SMB-FIXTURE
workgroup = TC-FIXTURE
security = user
map to guest = Bad User
guest account = nobody
interfaces = {self.server_ip}
bind interfaces only = yes
smb ports = 1445
server min protocol = SMB3_00
server max protocol = SMB3_11
disable netbios = yes
load printers = no
printing = bsd
printcap name = /dev/null
dns proxy = no
state directory = {root / 'state'}
lock directory = {root / 'lock'}
private dir = {root / 'private'}
cache directory = {root / 'cache'}
pid directory = {root / 'pid'}
ncalrpc dir = {root / 'ncalrpc'}
log file = {root / 'samba.log'}
[payload]
path = {self.backing}
guest ok = yes
read only = yes
''', encoding='utf-8')

    def start(self):
        run('ip', 'netns', 'add', self.namespace)
        self.namespace_created = True
        run('ip', 'link', 'add', self.host_if, 'type', 'veth', 'peer', 'name', self.peer_if)
        self.link_created = True
        run('ip', 'link', 'set', self.peer_if, 'netns', self.namespace)
        run('ip', 'addr', 'add', self.host_ip + '/30', 'dev', self.host_if)
        run('ip', 'link', 'set', self.host_if, 'up')
        run('ip', '-n', self.namespace, 'addr', 'add', self.server_ip + '/30', 'dev', self.peer_if)
        run('ip', '-n', self.namespace, 'link', 'set', self.peer_if, 'up')
        run('ip', '-n', self.namespace, 'link', 'set', 'lo', 'up')
        self.start_server()
        run('mount', '-t', 'cifs', f'//{self.server_ip}/payload', str(self.mount), '-o',
            'guest,username=tc-fixture,port=1445,vers=3.1.1,cache=none,actimeo=0,closetimeo=0,nolease,soft,echo_interval=1,ro,noserverino', timeout=30)
        self.mounted = True
        assert run('stat', '-f', '-c', '%T', str(self.mount)) in ('smb2', 'cifs'), 'The source must be a kernel CIFS mount'

    def start_server(self):
        if self.server_log:
            self.server_log.close()
        self.server_log = (self.root / 'server-stdout.log').open('a', encoding='utf-8')
        # This daemon owns its sockets; it is not the systemd service that may
        # have launched the CI runner. Do not inherit socket/notify activation.
        environment = {key: value for key, value in os.environ.items()
                       if key not in ('NOTIFY_SOCKET', 'LISTEN_FDS', 'LISTEN_PID', 'LISTEN_FDNAMES')}
        self.server = subprocess.Popen(['ip', 'netns', 'exec', self.namespace, 'smbd', '-F', '--no-process-group',
                                       '--debug-stdout', '-d', '3', '-s', str(self.config)],
                                       stdout=self.server_log, stderr=subprocess.STDOUT, start_new_session=True, env=environment)
        deadline = time.monotonic() + 15
        while True:
            if self.server.poll() is not None:
                messages = (self.root / 'server-stdout.log').read_text(errors='replace')
                samba_log = self.root / 'samba.log'
                if samba_log.exists():
                    messages += '\n' + samba_log.read_text(errors='replace')
                raise RuntimeError(f'Fixture Samba server exited {self.server.returncode} during startup: ' + messages[-4000:])
            try:
                with socket.create_connection((self.server_ip, 1445), timeout=0.2):
                    return
            except OSError:
                if time.monotonic() >= deadline:
                    raise TimeoutError('Fixture Samba server did not listen')
                time.sleep(0.05)

    def suspend(self):
        os.killpg(self.server.pid, signal.SIGSTOP)

    def resume(self):
        if self.server and self.server.poll() is None:
            os.killpg(self.server.pid, signal.SIGCONT)

    def disconnect(self):
        if self.server and self.server.poll() is None:
            os.killpg(self.server.pid, signal.SIGKILL)
            self.server.wait(timeout=10)

    def close(self):
        self.resume()
        try:
            if self.mounted:
                run('umount', str(self.mount), timeout=30)
                self.mounted = False
        finally:
            self.disconnect()
            if self.server_log:
                self.server_log.close()
            if self.link_created:
                run('ip', 'link', 'delete', self.host_if)
            if self.namespace_created:
                run('ip', 'netns', 'delete', self.namespace)


def until(workflow, predicate, timeout=15):
    deadline = time.monotonic() + timeout
    while True:
        snapshot = workflow.call('snapshot', timeout=3)
        if predicate(snapshot):
            return snapshot
        if time.monotonic() >= deadline:
            raise TimeoutError(f'Fault precondition was not reached: {snapshot}')
        time.sleep(0.025)


def healthy(executable, config, folder):
    with (folder / 'stderr.log').open('w') as error:
        workflow = Workflow(executable, config, error)
        try:
            assert workflow.call('scan')['files'] == 1
            workflow.call('create')
            result = wait_terminal(workflow, timeout=90)
            assert result['job']['state'].startswith('Succeeded'), result
            assert workflow.call('join')['probe']['readers'] == 0
            workflow.finish()
        finally:
            if workflow.process.poll() is None:
                workflow.process.kill()
                workflow.process.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--proof', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != 'linux' or os.geteuid() != 0:
        parser.error('A disposable root-capable Linux CIFS runner is required')
    executable = args.proof.resolve()
    args.report = args.report.resolve()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    details = args.report.parent / (args.report.stem + '-details')
    details.mkdir(exist_ok=False)
    report = {'scope': 'production AppService and POSIX payload adapter on kernel CIFS over TCP to isolated Samba network namespace',
              'platform': platform.platform(), 'server': run('smbd', '--version'), 'client': run('mount.cifs', '-V'),
              'limitations': 'one physical Linux runner, isolated peer network stack; not a remote Windows SMB driver or an arbitrary storage-driver deadline',
              'cases': [], 'passed': False, 'fixtureCleaned': False}
    fixture = None
    try:
        with tempfile.TemporaryDirectory(prefix='tc-smb-') as temp:
            fixture = SmbFixture(Path(temp))
            try:
                fixture.start()
                payload_digest = hashlib.sha256(fixture.source.read_bytes()).hexdigest()
                for fmt in ('v1', 'v2', 'hybrid'):
                    output = Path(temp) / f'{fmt}.torrent'
                    config = {'root': str(fixture.mount / 'payload.bin'), 'output': str(output), 'format': fmt, 'probe': True}
                    folder = details / f'{fmt}-baseline'
                    folder.mkdir()
                    healthy(executable, config, folder)
                    reference = subprocess.run([sys.executable, str(Path(__file__).resolve().parents[2] / 'tools/reference/verify_torrent.py'),
                                                str(output), '--root', str(fixture.mount / 'payload.bin')],
                                               capture_output=True, text=True, timeout=120)
                    assert reference.returncode == 0, reference.stdout + reference.stderr
                    original = output.read_bytes()
                    config['replace'] = True
                    for scenario, site in [('cancel-stalled-read', 'read'), ('cancel-delayed-open', 'open'), ('server-disconnect', 'read')]:
                        folder = details / f'{fmt}-{scenario}'
                        folder.mkdir()
                        with (folder / 'stderr.log').open('w') as error:
                            workflow = Workflow(executable, config, error)
                            try:
                                workflow.call('scan')
                                workflow.call('arm', site=site)
                                workflow.call('create')
                                until(workflow, lambda s: s['probe']['held'])
                                fixture.suspend()
                                workflow.call('release')
                                pending_key = 'nativeRead' if site == 'read' else 'nativeOpen'
                                pending = until(workflow, lambda s: s['probe'][pending_key])
                                time.sleep(0.25)
                                stalled = workflow.call('snapshot')
                                assert stalled['probe'][pending_key] and not stalled['probe']['held'], 'Only a pending production OS operation qualifies as a stall'
                                assert stalled['probe']['bytesRead'] == pending['probe']['bytesRead'] == 0
                                assert output.read_bytes() == original
                                kernel_waits = []
                                for task in (Path('/proc') / str(workflow.process.pid) / 'task').iterdir():
                                    try:
                                        kernel_waits.append({'tid': int(task.name), 'wchan': (task / 'wchan').read_text().strip()})
                                    except OSError:
                                        pass
                                request_start = time.monotonic()
                                if scenario == 'server-disconnect':
                                    fixture.disconnect()
                                    result = wait_terminal(workflow, timeout=90)
                                    assert result['job']['state'] == 'Failed', result
                                    assert result['job']['error']['code'] in ('SOURCE_UNREADABLE', 'SOURCE_MISSING', 'SOURCE_CHANGED'), result
                                    fixture.start_server()
                                    control_ms = None
                                else:
                                    workflow.call('cancel', timeout=2)
                                    control_ms = (time.monotonic() - request_start) * 1000
                                    time.sleep(0.2)
                                    cancelling = workflow.call('snapshot', timeout=2)
                                    assert cancelling['job']['state'] == 'Cancelling', cancelling
                                    assert cancelling['probe'][pending_key], 'Linux cancellation must retain resources while the syscall is pending'
                                    assert cancelling['probe']['readers'] == (1 if site == 'read' else 0)
                                    fixture.resume()
                                    result = wait_terminal(workflow, timeout=30)
                                    assert result['job']['state'] == 'Cancelled', result
                                elapsed_ms = (time.monotonic() - request_start) * 1000
                                joined = workflow.call('join', timeout=15)
                                assert joined['probe']['readers'] == 0 and not joined['probe']['nativeRead'] and not joined['probe']['nativeOpen']
                                assert output.read_bytes() == original, 'Previous torrent changed during the fault'
                                assert not list(Path(temp).glob('*.tmp')) and not list(Path(temp).glob('.*.tmp'))
                                workflow.finish()
                                case = {'format': fmt, 'scenario': scenario, 'nativePendingConfirmed': True,
                                        'kernelWaits': kernel_waits, 'state': result['job']['state'],
                                        'error': result['job'].get('error'), 'controlReplyMs': control_ms,
                                        'faultToTerminalMs': elapsed_ms, 'previousOutputPreserved': True,
                                        'liveReadersAfterJoin': 0, 'temporaryFiles': 0, 'baselineReferenceVerified': True}
                            finally:
                                # Restore the server BEFORE attempting task cleanup; never
                                # report a killed or still-pending worker as a passing join.
                                if fixture.server.poll() is None:
                                    fixture.resume()
                                else:
                                    fixture.start_server()
                                if workflow.process.poll() is None:
                                    try:
                                        workflow.call('release', timeout=3)
                                        workflow.call('cancel', timeout=3)
                                        workflow.call('join', timeout=30)
                                        workflow.finish()
                                    except Exception:
                                        workflow.process.kill()
                                        workflow.process.wait(timeout=15)
                        recovery = folder / 'recovery'
                        recovery.mkdir()
                        healthy(executable, config, recovery)
                        assert output.read_bytes() == original, 'A healthy retry must reproduce the baseline torrent'
                        case['healthyRetryVerified'] = True
                        report['cases'].append(case)
                        print(json.dumps(case), flush=True)
                assert hashlib.sha256(fixture.source.read_bytes()).hexdigest() == payload_digest
                report['payloadUnchanged'] = True
                report['passed'] = True
            finally:
                try:
                    fixture.close()
                    report['fixtureCleaned'] = True
                finally:
                    saved = details / 'fixture-logs'
                    saved.mkdir(exist_ok=True)
                    for log in Path(temp).glob('*.log*'):
                        if log.is_file():
                            shutil.copy2(log, saved / log.name)
    except Exception as error:
        report['passed'] = False
        report['error'] = str(error)
        raise
    finally:
        args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
