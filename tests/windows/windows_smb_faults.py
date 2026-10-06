"""Windows production UNC I/O and real WebView2 WM_CLOSE against a Linux VM."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'performance'))
from process_tree import Workflow, TreeMonitor, wait_terminal
from smb_faults import healthy, until
from smb_vm import SmbVm


class Gui:
    def __init__(self, executable, config, folder):
        self.folder, self.log = folder, folder / 'gui.log'
        fixture = folder / 'fixture.json'
        fixture.write_text(json.dumps(config), encoding='utf-8')
        self.process = subprocess.Popen([str(executable), '--self-test', str(self.log),
                                        '--self-test-data', str(folder / 'data'), '--self-test-smb', str(fixture)])
        self.monitor = TreeMonitor(self.process.pid).start()
        self.window = None
        self.next_ack = 1

    def find_window(self):
        if self.process.poll() is not None:
            return None
        user = ctypes.WinDLL('user32', use_last_error=True)
        callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
        user.EnumWindows.restype = wintypes.BOOL
        user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        user.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
        windows = []
        @callback_type
        def visit(window, _):
            pid = wintypes.DWORD()
            user.GetWindowThreadProcessId(window, ctypes.byref(pid))
            if pid.value == self.process.pid:
                name = ctypes.create_unicode_buffer(128)
                user.GetClassNameW(window, name, len(name))
                if name.value == 'TorrentControl.MainWindow':
                    windows.append(window)
            return True
        user.EnumWindows(visit, 0)
        self.window = windows[0] if windows else None
        return self.window

    def post(self, message, wparam=0, lparam=0):
        if not self.find_window():
            raise RuntimeError('The task-owned TorrentControl HWND was not found')
        post = ctypes.WinDLL('user32', use_last_error=True).PostMessageW
        post.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        post.restype = wintypes.BOOL
        if not post(self.window, message, wparam, lparam):
            raise ctypes.WinError(ctypes.get_last_error())

    def lines(self):
        if not self.log.exists():
            return []
        text = self.log.read_text(encoding='utf-8')
        lines = text.splitlines()
        return lines if text.endswith('\n') else lines[:-1]

    def snapshot(self):
        lines = self.lines()
        failures = [line for line in lines if line.startswith('FAIL ')]
        if failures:
            raise RuntimeError(failures[-1])
        snapshots = [json.loads(line[4:]) for line in lines if line.startswith('SMB ')]
        return snapshots[-1] if snapshots else {'job': None, 'probe': {'held': False}}

    def wait(self, predicate, timeout=30):
        started = time.monotonic()
        while True:
            snapshot = self.snapshot()
            if predicate(snapshot):
                return snapshot
            if self.process.poll() is not None or time.monotonic() - started > timeout:
                raise TimeoutError(f'GUI did not reach its fault precondition: {snapshot}')
            time.sleep(0.025)

    def heartbeat(self):
        acknowledgement = self.next_ack
        self.next_ack += 1
        started = time.monotonic()
        self.post(0x8000 + 5, 2, acknowledgement)
        while True:
            self.snapshot()  # also rejects a frontend failure
            if any(json.loads(line[4:]).get('ack') == acknowledgement for line in self.lines() if line.startswith('SMB ')):
                return (time.monotonic() - started) * 1000
            if time.monotonic() - started > 2:
                raise TimeoutError('The native UI did not acknowledge control during the SMB stall')
            time.sleep(0.025)

    def finish_evidence(self):
        deadline = time.monotonic() + 15
        while self.monitor.live_descendants() and time.monotonic() < deadline:
            time.sleep(0.1)
        evidence = self.monitor.finish()
        evidence['survivingChildren'] = self.monitor.live_descendants()
        (self.folder / 'processes.json').write_text(json.dumps(evidence, indent=2) + '\n')
        assert not evidence['survivingChildren']
        assert not evidence['measurementErrors']
        assert any(p['name'].lower() == 'msedgewebview2.exe' for p in evidence['discoveredProcesses'])
        return evidence['processIdentities']

    def cleanup(self):
        # Used only after restoring the peer. A forced process termination
        # never counts as a successful shutdown scenario.
        try:
            if self.process.poll() is None:
                if self.find_window():
                    self.post(0x8000 + 5, 1)
                    self.post(0x0010)  # real WM_CLOSE
                self.process.wait(timeout=30)
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait(timeout=15)
            self.monitor.stop.set()
            self.monitor.thread.join(timeout=10)


def preserved(output, original):
    assert output.read_bytes() == original, 'Previous torrent changed during the SMB fault'
    assert not list(output.parent.glob('*.tmp')) and not list(output.parent.glob('.*.tmp'))


def console_case(executable, vm, config, output, original, folder, scenario, site):
    with (folder / 'stderr.log').open('w', encoding='utf-8') as error:
        workflow = Workflow(executable, {**config, 'probe': True}, error)
        try:
            workflow.call('scan')
            workflow.call('arm', site=site)
            workflow.call('create')
            until(workflow, lambda s: s['probe']['held'])
            vm.suspend()
            workflow.call('release')
            key = 'nativeRead' if site == 'read' else 'nativeOpen'
            until(workflow, lambda s: s['probe'][key])
            time.sleep(0.25)
            stalled = workflow.call('snapshot', timeout=2)
            assert stalled['probe'][key] and not stalled['probe']['held'], 'The production OS operation must remain pending after the gate is released'
            assert stalled['probe']['bytesRead'] == 0
            preserved(output, original)
            started = time.monotonic()
            during_fault = None
            if scenario == 'server-disconnect':
                vm.disconnect()
                result = wait_terminal(workflow, timeout=120)
                assert result['job']['state'] == 'Failed', result
                assert result['job']['error']['code'] in ('SOURCE_UNREADABLE', 'SOURCE_MISSING', 'SOURCE_CHANGED'), result
                vm.start_server()
                reply_ms = None
            else:
                workflow.call('cancel', timeout=2)
                reply_ms = (time.monotonic() - started) * 1000
                time.sleep(0.2)
                during_fault = workflow.call('snapshot', timeout=2)
                assert during_fault['job']['state'] in ('Cancelling', 'Cancelled'), during_fault
                if during_fault['job']['state'] == 'Cancelling':
                    assert during_fault['probe']['readers'] or during_fault['probe']['nativeOpen'] or during_fault['probe']['nativeRead']
                else:
                    assert during_fault['probe']['readers'] == 0 and not during_fault['probe']['nativeRead'] and not during_fault['probe']['nativeOpen']
                vm.resume()
                result = wait_terminal(workflow, timeout=30)
                assert result['job']['state'] == 'Cancelled', result
            terminal_ms = (time.monotonic() - started) * 1000
            joined = workflow.call('join', timeout=15)
            assert joined['probe']['readers'] == 0 and not joined['probe']['nativeRead'] and not joined['probe']['nativeOpen']
            preserved(output, original)
            workflow.finish()
            case = {'format': config['format'], 'scenario': scenario, 'mode': 'headless',
                    'nativePendingConfirmed': True, 'probeWhileStalled': stalled['probe'],
                    'terminalState': result['job']['state'], 'error': result['job'].get('error'),
                    'cancelReplyMs': reply_ms, 'faultToTerminalMs': terminal_ms,
                    'stateAfterCancelWhileStalled': during_fault['job']['state'] if during_fault else None,
                    'probeAfterCancelWhileStalled': during_fault['probe'] if during_fault else None,
                    'liveReadersAfterJoin': 0, 'previousOutputPreserved': True, 'temporaryFiles': 0}
        finally:
            # A stalled worker is recovered and joined before fixture teardown.
            # Never hide a failure by killing it and reporting success.
            vm.restore()
            if workflow.process.poll() is None:
                try:
                    workflow.call('release', timeout=3)
                    workflow.call('cancel', timeout=3)
                    workflow.call('join', timeout=30)
                    workflow.finish()
                except Exception:
                    workflow.process.kill()
                    workflow.process.wait(timeout=15)
    return case


def gui_case(executable, vm, config, output, original, folder, site):
    gui = Gui(executable, {**config, 'site': site}, folder)
    try:
        gui.wait(lambda s: s['probe']['held'])
        vm.suspend()
        gui.post(0x8000 + 5, 1)
        key = 'nativeRead' if site == 'read' else 'nativeOpen'
        gui.wait(lambda s: s['probe'].get(key, False) and not s['probe']['held'])
        time.sleep(0.25)
        stalled = gui.snapshot()
        assert stalled['probe'][key] and stalled['probe']['bytesRead'] == 0
        heartbeat_ms = gui.heartbeat()
        preserved(output, original)
        started = time.monotonic()
        gui.post(0x0010)
        time.sleep(0.5)
        exit_during_fault = gui.process.poll() is not None
        assert 'SMB_CLOSE_REQUESTED' in gui.lines(), 'The actual native WM_CLOSE path must run'
        if site == 'open':
            assert not exit_during_fault, 'A synchronous pending open must retain its worker until the OS call returns'
        vm.resume()
        assert gui.process.wait(timeout=30) == 0
        exit_ms = (time.monotonic() - started) * 1000
        shutdown = [json.loads(line[13:]) for line in gui.lines() if line.startswith('SMB_SHUTDOWN ')]
        assert len(shutdown) == 1
        assert shutdown[0]['readers'] == 0 and not shutdown[0]['nativeRead'] and not shutdown[0]['nativeOpen']
        assert not any(line.startswith('FAIL ') for line in gui.lines())
        preserved(output, original)
        identities = gui.finish_evidence()
        return {'format': config['format'], 'scenario': 'shutdown-stalled-' + site, 'mode': 'gui',
                'nativePendingConfirmed': True, 'probeWhileStalled': stalled['probe'],
                'uiControlReplyMs': heartbeat_ms, 'actualWmClose': True, 'normalExit': True,
                'exitedDuringFault': exit_during_fault, 'closeToExitMs': exit_ms,
                'processIdentities': identities, 'survivingChildren': [], 'liveReadersAfterJoin': 0,
                'previousOutputPreserved': True, 'temporaryFiles': 0}
    finally:
        vm.restore()
        gui.cleanup()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--proof', type=Path, required=True)
    parser.add_argument('--gui', type=Path, required=True)
    parser.add_argument('--vm-config', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != 'win32':
        parser.error('The real Windows SMB redirector and WebView2 are required; this is not a skippable Linux test')
    args.report = args.report.resolve()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    details = args.report.parent / (args.report.stem + '-details')
    details.mkdir(exist_ok=False)
    report = {'platform': platform.platform(), 'scope': 'production Windows UNC payload adapter and actual WebView2 WM_CLOSE over private TAP TCP/SMB to a separate Linux guest kernel',
              'limitations': 'one Windows Server 2022 CI host with a controlled VM peer; not a physical NAS, all desktop versions, arbitrary drivers or a universal cancellation/shutdown deadline',
              'cases': [], 'passed': False, 'vmStopped': False, 'connectionRemoved': False, 'fixtureCleaned': False}
    vm = SmbVm(json.loads(args.vm_config.read_text(encoding='utf-8-sig')), details / 'vm')
    try:
        vm.start()
        report['peer'] = vm.metadata
        report['provenance'] = vm.config['provenance']
        source = Path(vm.share) / 'payload.bin'
        digest = hashlib.sha256()
        for _ in range(64):
            digest.update(bytes(1048576))
        assert vm.metadata['payloadSha256'] == digest.hexdigest()
        for fmt in ('v1', 'v2', 'hybrid'):
            output = details / f'{fmt}.torrent'
            config = {'root': str(source), 'output': str(output), 'format': fmt, 'replace': True}
            baseline = details / f'{fmt}-baseline'
            baseline.mkdir()
            healthy(args.proof.resolve(), config, baseline)
            verify = subprocess.run([sys.executable, str(Path(__file__).resolve().parents[2] / 'tools/reference/verify_torrent.py'),
                                     str(output), '--root', str(source)], capture_output=True, text=True, timeout=180)
            (baseline / 'reference.json').write_text(verify.stdout)
            assert verify.returncode == 0, verify.stdout + verify.stderr
            original = output.read_bytes()
            scenarios = [('cancel-stalled-read', 'read'), ('cancel-delayed-open', 'open'), ('server-disconnect', 'read'),
                         ('shutdown-stalled-read', 'read'), ('shutdown-stalled-open', 'open')]
            for scenario, site in scenarios:
                folder = details / f'{fmt}-{scenario}'
                folder.mkdir()
                case = (gui_case(args.gui.resolve(), vm, config, output, original, folder, site)
                        if scenario.startswith('shutdown-') else
                        console_case(args.proof.resolve(), vm, config, output, original, folder, scenario, site))
                case['clientRecoveryMs'] = vm.wait_recovery()
                recovery = folder / 'recovery'
                recovery.mkdir()
                healthy(args.proof.resolve(), config, recovery)
                assert output.read_bytes() == original, 'Healthy retry must reproduce the baseline bytes'
                case['healthyRetryVerified'] = case['baselineReferenceVerified'] = True
                report['cases'].append(case)
                print(json.dumps(case), flush=True)
        assert vm.console.command('sha256sum /srv/tc-payload/payload.bin').split()[0] == digest.hexdigest()
        report['payloadUnchanged'] = True
        report['passed'] = True
    except Exception as error:
        report['passed'] = False
        report['error'] = str(error)
        raise
    finally:
        try:
            vm.close()
            report['vmStopped'] = report['connectionRemoved'] = True
        except Exception as error:
            report['passed'] = False
            report['cleanupError'] = str(error)
            raise
        finally:
            args.report.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
