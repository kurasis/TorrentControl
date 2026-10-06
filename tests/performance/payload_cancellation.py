"""Local regular-file cancellation evidence; no UNC-stall timing claim."""
import argparse
import hashlib
import json
import platform
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--proof', type=Path, required=True)
parser.add_argument('--report', type=Path, required=True)
args = parser.parse_args()
proof = args.proof.resolve()
report = {'scope': 'local regular-file core cancellation/worker join; excludes UNC stalls and WebView2',
          'platform': platform.platform(), 'cases': []}
with tempfile.TemporaryDirectory(prefix='tc-cancel-io-') as temp:
    root = Path(temp)
    source = root / 'payload.bin'
    block = bytes(range(256)) * 4096
    with source.open('wb') as out:
        for _ in range(64):
            out.write(block)
    original_payload = hashlib.sha256(source.read_bytes()).hexdigest()
    for fmt in ('v1', 'v2', 'hybrid'):
        output = root / f'{fmt}.torrent'
        base = [str(proof), 'create', '--source', str(source), '--format', fmt,
                '--piece-length', '262144', '-o', str(output)]
        success = subprocess.run(base, text=True, capture_output=True, timeout=120)
        assert success.returncode == 0, success.stderr
        original_output = output.read_bytes()
        for budget in (1048576, 8388608):
            cancelled = subprocess.run(base + ['--replace', '--budget', str(budget), '--threads', '4',
                                               '--cancel-after-bytes', '4194304'],
                                       text=True, capture_output=True, timeout=120)
            assert cancelled.returncode == 3, cancelled.stderr
            result = json.loads(cancelled.stdout)
            assert result['status'] == 'cancelled'
            assert 4194304 <= result['io']['bytesRead'] <= 8388608
            assert result['payload_bytes_read'] == result['io']['bytesRead']
            assert result['io']['opens'] == 1
            assert result['cancelLatencyMs'] < 2000, 'Local cancellation/worker join exceeded two seconds'
            assert output.read_bytes() == original_output, 'Cancelled creation must preserve previous output'
            assert not list(root.glob('*.tmp')) and not list(root.glob('.*.tmp'))
            case = {'format': fmt, 'budgetBytes': budget, 'cancelLatencyMs': result['cancelLatencyMs'],
                    'io': result['io'], 'previousOutputPreserved': True, 'temporaryFiles': 0, 'engine': result['engine']}
            report['cases'].append(case)
            print(json.dumps(case), flush=True)
    assert hashlib.sha256(source.read_bytes()).hexdigest() == original_payload
report['payloadUnchanged'] = True
args.report.parent.mkdir(parents=True, exist_ok=True)
args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
