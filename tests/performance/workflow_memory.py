"""Fresh GUI/headless AppService processes over identical real payloads."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import time

from process_tree import TreeMonitor, Workflow, wait_terminal


def headless(executable, config, folder):
    phase = 'idle'
    with (folder / 'stderr.log').open('w', encoding='utf-8') as error:
        workflow = Workflow(executable, config, error)
        monitor = TreeMonitor(workflow.process.pid, lambda: phase).start()
        try:
            time.sleep(0.3)
            phase = 'scan'
            fixture = workflow.call('scan', timeout=480)
            time.sleep(0.3)
            phase = 'review'
            assert workflow.call('review')['canCreate']
            time.sleep(0.3)
            phase = 'create'
            workflow.call('create', timeout=480)
            snapshot = wait_terminal(workflow, timeout=480)
            assert snapshot['job']['state'].startswith('Succeeded'), snapshot['job']['error']
            workflow.call('join')
            time.sleep(0.1)  # Include receipt/join of a very short job in this phase.
            phase = 'completed'
            time.sleep(0.3)
            assert workflow.call('clear')['remaining'] == 0
            phase = 'cleared'
            time.sleep(0.3)
            workflow.finish()
            evidence = monitor.finish()
            evidence['workflow'] = {**fixture, 'historyCleared': True, 'jobState': snapshot['job']['state']}
            evidence['survivingChildren'] = monitor.live_descendants()
            assert not evidence['survivingChildren']
            return evidence
        finally:
            monitor.stop.set()
            monitor.thread.join(timeout=10)
            if workflow.process.poll() is None:
                workflow.process.kill()
                workflow.process.wait(timeout=10)


def gui(executable, config, folder):
    config_file = folder / 'fixture.json'
    config_file.write_text(json.dumps(config), encoding='utf-8')
    log = folder / 'gui.log'
    phase = 'startup'
    consumed = 0
    def current_phase():
        nonlocal phase, consumed
        if log.exists():
            contents = log.read_text(encoding='utf-8')
            lines = contents.splitlines()
            if contents and not contents.endswith('\n'):
                lines = lines[:-1]  # A partially written checkpoint is not an event.
            for line in lines[consumed:]:
                if line.startswith('MEMORY '):
                    phase = line[7:]
            consumed = len(lines)
        return phase
    process = subprocess.Popen([str(executable), '--self-test', str(log), '--self-test-data', str(folder / 'data'),
                                '--self-test-memory', str(config_file)])
    monitor = TreeMonitor(process.pid, current_phase).start()
    try:
        assert process.wait(timeout=620) == 0, log.read_text(encoding='utf-8') if log.exists() else 'No GUI log'
        deadline = time.monotonic() + 15
        while monitor.live_descendants() and time.monotonic() < deadline:
            time.sleep(0.05)
        evidence = monitor.finish()
        evidence['survivingChildren'] = monitor.live_descendants()
        assert not evidence['survivingChildren'], 'WebView2 child processes survived normal shutdown'
        passes = [line[5:] for line in log.read_text(encoding='utf-8').splitlines() if line.startswith('PASS ')]
        assert len(passes) == 1
        evidence['workflow'] = json.loads(passes[0])
        assert evidence['workflow']['ok'] and evidence['workflow']['historyCleared']
        assert evidence['processIdentities'] > 1 and any(p['maxProcesses'] > 1 for p in evidence['phases'].values()), 'WebView2 descendants were not measured'
        assert any(p['name'].lower() == 'msedgewebview2.exe' for p in evidence['discoveredProcesses']), 'No real WebView2 process was sampled'
        return evidence
    finally:
        monitor.stop.set()
        monitor.thread.join(timeout=10)
        if process.poll() is None:
            process.kill()
            process.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--proof', type=Path, required=True)
    parser.add_argument('--gui', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--files', type=int, default=100000)
    parser.add_argument('--large-mib', type=int, default=96)
    args = parser.parse_args()
    if not 2 <= args.files <= 100000 or not 16 <= args.large_mib <= 1024:
        parser.error('files must be 2..100000; large-mib must be 16..1024')
    if args.gui and os.name != 'nt':
        parser.error('Real WebView2 measurements require Windows')
    report = {'platform': platform.platform(), 'cpuCount': os.cpu_count(),
              'scope': 'fresh AppService scan/review/create/job-history workflow; task-owned descendants included',
              'metrics': 'RSS/working-set sum within each sampling sweep; shared pages may be counted multiple times; Linux PSS and Windows private commit are separate metrics',
              'limitations': '50 ms samples may miss short-lived children and peaks; excludes driver/kernel/page-cache memory and Python driver; no machine-independent memory threshold',
              'cachePolicy': 'OS cache is not flushed; headless precedes GUI on each dataset', 'cases': [], 'comparisons': []}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    evidence_root = args.report.parent / (args.report.stem + '-details')
    evidence_root.mkdir(parents=True, exist_ok=False)
    verifier = Path(__file__).resolve().parents[2] / 'tools/reference/verify_torrent.py'
    try:
        with tempfile.TemporaryDirectory(prefix='tc-process-memory-') as temp:
            root = Path(temp)
            tree = root / 'Collection'
            for i in range(args.files):
                folder = tree / f'folder-{i // 1000:03d}'
                folder.mkdir(parents=True, exist_ok=True)
                (folder / f'file-{i:06d}.bin').write_bytes(bytes([i % 251]) * (257 + i % 31))
            large = root / 'large.bin'
            with large.open('wb') as out:
                for _ in range(args.large_mib):
                    out.write(bytes(range(256)) * 4096)
                out.write(b'boundary')
            modes = [('headless', args.proof.resolve(), headless)]
            if args.gui:
                modes.append(('gui', args.gui.resolve(), gui))
            for shape, source, count in [('tree', tree, args.files), ('large-file', large, 1)]:
                for fmt in ('v1', 'v2', 'hybrid'):
                    pair = []
                    for mode, executable, run in modes:
                        folder = evidence_root / f'{shape}-{fmt}-{mode}'
                        folder.mkdir()
                        output = root / f'{shape}-{fmt}-{mode}.torrent'
                        config = {'root': str(source), 'format': fmt, 'output': str(output),
                                  'pieceLength': 16384 if shape == 'tree' else 262144}
                        evidence = run(executable, config, folder)
                        assert evidence['workflow']['files'] == count
                        for phase in ('idle', 'scan', 'review', 'create', 'completed', 'cleared'):
                            assert phase in evidence['phases'], f'Missing phase: {phase}'
                        reference = subprocess.run([sys.executable, str(verifier), str(output), '--root', str(source)],
                                                   capture_output=True, text=True, encoding='utf-8', timeout=600)
                        assert reference.returncode == 0, reference.stdout + reference.stderr
                        digest = hashlib.sha256(output.read_bytes()).hexdigest()
                        (folder / 'samples.json').write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
                        case = {'shape': shape, 'format': fmt, 'mode': mode, 'files': count,
                                'outputSha256': digest, 'referenceVerified': True, 'phases': evidence['phases'],
                                'processIdentities': evidence['processIdentities'], 'survivingChildren': evidence['survivingChildren']}
                        report['cases'].append(case)
                        pair.append(case)
                        print(json.dumps(case), flush=True)
                    if len(pair) == 2:
                        assert pair[0]['outputSha256'] == pair[1]['outputSha256'], 'GUI/headless torrent bytes differ'
                        report['comparisons'].append({'shape': shape, 'format': fmt, 'identicalTorrentBytes': True,
                            'guiMinusHeadlessPeakResidentBytes': {phase: pair[1]['phases'][phase]['peakResidentSumBytes'] - pair[0]['phases'][phase]['peakResidentSumBytes'] for phase in pair[0]['phases']},
                            'interpretation': 'difference between separate sampled runs, not an additive component-memory bound'})
            report['passed'] = True
    except Exception as error:
        report['passed'] = False
        report['error'] = str(error)
        raise
    finally:
        args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
