"""Fresh-process creation memory/IO evidence on real payloads, no network."""
import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def run_json(command):
    completed = subprocess.run(command, text=True, capture_output=True, timeout=600)
    if completed.returncode:
        raise RuntimeError(f"{command[0]} exited {completed.returncode}: {completed.stderr[:2000]} {completed.stdout[:2000]}")
    return json.loads(completed.stdout)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--proof', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--files', type=int, default=10000)
    parser.add_argument('--large-mib', type=int, default=96)
    args = parser.parse_args()
    if not 2 <= args.files <= 100000 or not 16 <= args.large_mib <= 1024:
        parser.error('files must be 2..100000 and large-mib 16..1024')
    proof = args.proof.resolve()
    verifier = Path(__file__).resolve().parents[2] / 'tools/reference/verify_torrent.py'
    report = {'scope': 'fresh headless core processes, scan/create/commit/reopen; excludes Python driver, WebView2 and total application process tree',
              'platform': platform.platform(), 'cpuCount': os.cpu_count(),
              'cachePolicy': 'new fixture; OS cache is not flushed, runs after the first are warm',
              'cases': []}
    with tempfile.TemporaryDirectory(prefix='tc-create-memory-') as temp:
        root = Path(temp)
        tree = root / 'Collection'
        for i in range(args.files):
            folder = tree / f'folder-{i // 1000:03d}'
            folder.mkdir(parents=True, exist_ok=True)
            (folder / f'file-{i:06d}.bin').write_bytes(bytes([i % 251]) * (257 + i % 31))
        large = root / 'large.bin'
        block = bytes(range(256)) * 4096  # one MiB, independent of payload size
        with large.open('wb') as out:
            for _ in range(args.large_mib):
                out.write(block)
            out.write(b'boundary')
        for shape, source in [('tree', tree), ('large-file', large)]:
            expected_files = args.files if shape == 'tree' else 1
            payload = sum(p.stat().st_size for p in tree.rglob('*.bin')) if shape == 'tree' else large.stat().st_size
            for fmt in ('v1', 'v2', 'hybrid'):
                baseline = None
                for workers, budget in [(1, 1024 * 1024), (4, 8 * 1024 * 1024)]:
                    output = root / f'{shape}-{fmt}-{workers}.torrent'
                    begin = time.monotonic()
                    result = run_json([str(proof), 'create', '--source', str(source), '--format', fmt,
                                       '--piece-length', '262144', '--budget', str(budget), '--threads', str(workers),
                                       '--buffer', '1048576', '-o', str(output)])
                    assert result['status'] == 'succeeded'
                    assert result['manifest_files'] == expected_files
                    assert result['payload_bytes_read'] == payload
                    assert result['io']['bytesRead'] == payload, 'Instrumented reader must prove one payload read pass'
                    assert result['io']['opens'] == expected_files
                    h = result['hashing']
                    assert 0 < h['peakPayloadBufferBytes'] <= h['plannedPayloadBufferBytes'] <= budget
                    assert h['maxReadRequestBytes'] <= min(h['unitBytes'], 1048576)
                    assert 0 < h['hashWorkers'] <= workers
                    assert result['peakRssBytes'] > 0
                    digest = hashlib.sha256(output.read_bytes()).hexdigest()
                    if baseline is None:
                        baseline = digest
                    assert digest == baseline, 'Budget/worker changes must preserve identical torrent bytes'
                    reference = run_json([sys.executable, str(verifier), str(output), '--root', str(source)])
                    assert reference['ok'], reference.get('errors')
                    report['cases'].append({'shape': shape, 'format': fmt, 'files': expected_files,
                                            'budgetBytes': budget, 'requestedWorkers': workers, 'payloadBytes': payload,
                                            'payloadBytesRead': result['payload_bytes_read'], 'io': result['io'], 'hashing': h,
                                            'peakRssBytes': result['peakRssBytes'], 'estimatedMemoryBytes': result['estimated_memory_bytes'],
                                            'creationMs': result['creationMs'], 'workflowMs': (time.monotonic() - begin) * 1000,
                                            'outputSha256': digest, 'referenceVerified': True, 'engine': result['engine']})
                    print(json.dumps(report['cases'][-1]), flush=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
