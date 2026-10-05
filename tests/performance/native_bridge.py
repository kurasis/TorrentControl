"""Generate a real tree and benchmark the native dispatcher. Opt-in, no mocks."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("--proof", type=Path, required=True)
parser.add_argument("--files", type=int, default=100_000)
parser.add_argument("--report", type=Path, required=True)
args = parser.parse_args()
if not 1 <= args.files <= 1_000_000:
    parser.error("--files must be between 1 and 1000000")

with tempfile.TemporaryDirectory(prefix="tc-native-benchmark-") as directory:
    root = Path(directory) / "Release"
    started = time.monotonic()
    for i in range(args.files):
        folder = root / f"folder-{i // 1000:03d}"
        folder.mkdir(parents=True, exist_ok=True)
        (folder / f"file-{i:06d}.bin").write_bytes(b"x")
    fixture_ms = (time.monotonic() - started) * 1000
    result = subprocess.run([str(args.proof.resolve())], input=json.dumps({"root": str(root)}),
                            text=True, capture_output=True, timeout=600)
    if result.returncode:
        raise RuntimeError(result.stderr)
    report = json.loads(result.stdout)
    assert report["files"] == args.files
    assert report["canCreate"], "Real native preflight must accept the dataset"
    assert report["maxResponseBytes"] < 1024 * 1024, "Paged native responses must remain below 1 MiB"
    report["fixtureCreationMs"] = fixture_ms
    report["scope"] = "native C++ dispatcher and real filesystem; excludes WebView2 rendering"
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
