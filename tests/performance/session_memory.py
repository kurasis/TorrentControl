"""3000 real jobs in one AppService process; paired with actual WebView2."""
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

from process_tree import TreeMonitor, Workflow


def assert_cleared(value):
    for key in ('jobs', 'createSpecs', 'verifySpecs', 'inputManifestEntries', 'verifyInputBytes',
                'jobThreads', 'batches', 'appBatches', 'appBatchJobIds', 'running'):
        assert value[key] == 0, (key, value)
    assert value['workers'] <= 8 and value['archivedBatches'] <= 64, value


def headless(executable, config, folder):
    phase = 'idle'
    checkpoints, cycles = [], []
    with (folder / 'stderr.log').open('w', encoding='utf-8') as error:
        workflow = Workflow(executable, config, error)
        monitor = TreeMonitor(workflow.process.pid, lambda: phase, root_resources=True).start()
        def checkpoint(name):
            nonlocal phase
            phase = name
            value = {'phase': name, **workflow.call('sessionStatus')}
            checkpoints.append(value)
            time.sleep(0.4)
            return value
        try:
            checkpoint('idle')
            for fmt in ('v1', 'v2', 'hybrid'):
                assert workflow.call('sessionPrepare', format=fmt)['files'] == 32
                checkpoint(f'{fmt}-warm')
                began = time.monotonic()
                for wave in range(10):
                    checkpoint(f'{fmt}-wave-{wave}')
                    workflow.call('sessionEnqueue', kind=('create', 'verify', 'batch')[wave % 3], count=100)
                    workflow.call('join')
                cycle = workflow.call('sessionFinish')
                cycle['elapsedMs'] = (time.monotonic() - began) * 1000
                cycles.append(cycle)
                checkpoint(f'{fmt}-completed')
                assert workflow.call('clear')['remaining'] == 0
                assert_cleared(checkpoint(f'{fmt}-cleared'))
            workflow.finish()
            evidence = monitor.finish()
            assert not monitor.live_descendants()
            return {**evidence, 'checkpoints': checkpoints, 'cycles': cycles, 'jobs': 3000,
                    'historyCleared': True, 'survivingChildren': []}
        finally:
            monitor.stop.set(); monitor.thread.join(timeout=10)
            if workflow.process.poll() is None:
                workflow.process.kill(); workflow.process.wait(timeout=10)


def gui(executable, config, folder):
    fixture = folder / 'fixture.json'
    fixture.write_text(json.dumps(config), encoding='utf-8')
    log = folder / 'gui.log'
    phase, consumed = 'startup', 0
    def current_phase():
        nonlocal phase, consumed
        if log.exists():
            contents = log.read_text(encoding='utf-8')
            lines = contents.splitlines()
            if contents and not contents.endswith('\n'):
                lines = lines[:-1]
            for line in lines[consumed:]:
                if line.startswith('SESSION '):
                    phase = json.loads(line[8:])['phase']
            consumed = len(lines)
        return phase
    process = subprocess.Popen([str(executable), '--self-test', str(log), '--self-test-data', str(folder / 'data'),
                                '--self-test-session', str(fixture)])
    monitor = TreeMonitor(process.pid, current_phase, root_resources=True).start()
    try:
        assert process.wait(timeout=620) == 0, log.read_text(encoding='utf-8') if log.exists() else 'No GUI log'
        deadline = time.monotonic() + 15
        while monitor.live_descendants() and time.monotonic() < deadline:
            time.sleep(0.05)
        evidence = monitor.finish()
        assert not monitor.live_descendants(), 'WebView2 descendants survived normal exit'
        passes = [json.loads(line[5:]) for line in log.read_text(encoding='utf-8').splitlines() if line.startswith('PASS ')]
        assert len(passes) == 1 and passes[0]['ok'] and passes[0]['jobs'] == 3000
        assert any(p['name'].lower() == 'msedgewebview2.exe' for p in evidence['discoveredProcesses'])
        for checkpoint in passes[0]['checkpoints']:
            if checkpoint['phase'].endswith('-cleared'):
                assert_cleared(checkpoint)
        return {**evidence, **passes[0], 'survivingChildren': []}
    finally:
        monitor.stop.set(); monitor.thread.join(timeout=10)
        if process.poll() is None:
            process.kill(); process.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--proof', type=Path, required=True)
    parser.add_argument('--gui', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    if args.gui and os.name != 'nt':
        parser.error('Actual WebView2 requires Windows')
    args.report.parent.mkdir(parents=True, exist_ok=True)
    details = args.report.parent / (args.report.stem + '-details')
    details.mkdir(exist_ok=False)
    report = {'platform': platform.platform(), 'scope': 'three 1000-job cycles in each same AppService process; 32 real files; v1/v2/hybrid',
              'metrics': '50 ms process-tree resident sums, Linux PSS, Windows private commit; native root threads and handles/fds',
              'limitations': 'Shared RSS pages can be counted more than once; inter-sample peaks can be missed; excludes driver, kernel and OS cache memory. Cleared RSS drift is reported, without a machine-independent threshold. Full native result history persists until Clear.',
              'cachePolicy': 'OS cache not flushed; headless precedes GUI; elapsed time includes checkpoint holds and rendering/bridge overhead',
              'budgets': {'workers': 8, 'completedInputSpecs': 0, 'rendererSummaries': 51, 'visibleJobs': 50, 'recentBatchAggregates': 64},
              'cases': [], 'comparisons': []}
    verifier = Path(__file__).resolve().parents[2] / 'tools/reference/verify_torrent.py'
    try:
        with tempfile.TemporaryDirectory(prefix='tc-session-') as temp:
            root = Path(temp); source = root / 'Collection'; source.mkdir()
            for i in range(32):
                (source / f'{i:02}.bin').write_bytes(bytes([i]) * (257 + i))
            original = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in source.iterdir()}
            modes = [('headless', args.proof.resolve(), headless)]
            if args.gui:
                modes.append(('gui', args.gui.resolve(), gui))
            for mode, executable, run in modes:
                folder = details / mode; folder.mkdir()
                batch = root / f'batch-{mode}'; batch.mkdir()
                config = {'root': str(source), 'output': str(root / f'{mode}.torrent'), 'batchOutput': str(batch),
                          'artifactRoot': str(folder.resolve()), 'format': 'v1', 'pieceLength': 16384}
                evidence = run(executable, config, folder)
                outputs = {}
                for fmt in ('v1', 'v2', 'hybrid'):
                    assert f'{fmt}-completed' in evidence['phases'] and f'{fmt}-cleared' in evidence['phases']
                    for suffix in ('', '-batch'):
                        name = fmt + suffix
                        output = folder / (name + '.torrent')
                        reference = subprocess.run([sys.executable, str(verifier), str(output), '--root', str(source)],
                                                   capture_output=True, text=True, encoding='utf-8', timeout=120)
                        assert reference.returncode == 0, reference.stdout + reference.stderr
                        outputs[name] = hashlib.sha256(output.read_bytes()).hexdigest()
                assert {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in source.iterdir()} == original
                assert not list(root.rglob('*.tmp')) and not list(root.rglob('.*.tmp'))
                cleared = [evidence['phases'][f'{fmt}-cleared'] for fmt in ('v1', 'v2', 'hybrid')]
                drift = {}
                for metric in ('peakResidentSumBytes', 'peakPssBytes', 'peakPrivateCommitBytes', 'peakRootThreads', 'peakRootHandlesOrFds'):
                    values = [p[metric] for p in cleared if p[metric] is not None]
                    drift[metric] = {'cycles': values, 'lastMinusFirst': values[-1] - values[0]} if values else None
                (folder / 'samples.json').write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
                case = {'mode': mode, 'jobs': evidence['jobs'], 'cycles': evidence['cycles'], 'checkpoints': evidence['checkpoints'],
                        'phases': evidence['phases'], 'clearedDrift': drift, 'outputs': outputs, 'referenceVerified': True,
                        'sourceUnchanged': True, 'temporaryOutputs': 0, 'survivingChildren': [], 'normalExit': True}
                if 'responsiveness' in evidence:
                    case['responsiveness'] = evidence['responsiveness']
                report['cases'].append(case)
                print(json.dumps({'mode': mode, 'jobs': 3000, 'clearedDrift': drift}), flush=True)
            if len(report['cases']) == 2:
                for name, digest in report['cases'][0]['outputs'].items():
                    assert report['cases'][1]['outputs'][name] == digest, f'GUI/headless output mismatch: {name}'
                    report['comparisons'].append({'output': name, 'identicalBytes': True, 'sha256': digest})
        report['ok'] = True
    finally:
        args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
