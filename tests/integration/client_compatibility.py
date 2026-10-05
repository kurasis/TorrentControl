"""Offline client compatibility with independent payload/hash verification."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--proof", type=Path, required=True)
parser.add_argument("--client", type=Path, required=True)
parser.add_argument("--report", type=Path, required=True)
parser.add_argument("--require-complete", action="store_true", help="Fail if any client compatibility gate remains open")
args = parser.parse_args()
proof, client = str(args.proof.resolve()), str(args.client.resolve())
reference = Path(__file__).resolve().parents[2] / "tools/reference/verify_torrent.py"


def run(argv, code=0):
    result = subprocess.run(list(map(str, argv)), capture_output=True, text=True, timeout=60)
    assert result.returncode == code, f"{argv[0]} exited {result.returncode}: {result.stderr} {result.stdout}"
    return json.loads(result.stdout)


def digest(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob("*") if p.is_file()}


reports = []
with tempfile.TemporaryDirectory(prefix="tc-client-compat-") as temporary:
    parent = Path(temporary)
    folder = parent / "Release"
    for i, size in enumerate([0, 1, 16383, 16384, 16385, 65535, 65536, 65537, 200001]):
        file = folder / "данные #1" / f"file-{i}.bin"
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(bytes((i + n) % 251 for n in range(size)))
    single = parent / "single.bin"
    single.write_bytes(bytes(n % 251 for n in range(70003)))
    for shape, source in [("multifile", folder), ("single-file", single)]:
        for format in ["v1", "v2", "hybrid"]:
            torrent = parent / f"{shape}-{format}.torrent"
            created = run([proof, "create", "--source", source, "--format", format,
                           "--piece-length", "65536", "-o", torrent])
            run([proof, "validate", torrent])
            run([sys.executable, reference, torrent, "--root", source])
            before = digest(parent)
            if format == "v2":
                checked = run([client, torrent, parent], code=6)
                assert not checked["supported"] and checked["metadataValidated"]
                assert digest(parent) == before
                reports.append({"shape": shape, "format": format, "client": checked,
                                "payloadVerification": "unsupported; independent client gate remains open"})
                continue
            if shape == "single-file" and format == "hybrid":
                checked = run([client, torrent, parent], code=5)
                assert not checked["verified"] and checked["bytesMissing"] == source.stat().st_size
                assert digest(parent) == before
                reports.append({"shape": shape, "format": format, "client": checked,
                                "clientImportLimitation": "Client resolves name/name for libtorrent single-file hybrid layout"})
                continue
            checked = run([client, torrent, parent])
            assert checked["verified"] and checked["bytesMissing"] == 0
            for suffix in ["v1", "v2"]:
                if f"infohash_{suffix}" in created:
                    assert checked[f"infohash{suffix.upper()}"] == created[f"infohash_{suffix}"]
            assert digest(parent) == before, "Independent client changed the payload or created files"
            # A negative control must fail even though imported metadata still parses.
            target = source / "данные #1/file-8.bin" if shape == "multifile" else source
            original = target.read_bytes()
            target.write_bytes(bytes([original[0] ^ 1]) + original[1:])
            corrupt = run([client, torrent, parent], code=5)
            assert not corrupt["verified"] and corrupt["bytesMissing"] > 0
            target.write_bytes(original)
            target.unlink()
            missing = run([client, torrent, parent], code=5)
            assert not missing["verified"]
            target.write_bytes(original)
            reports.append({"shape": shape, "format": format, "client": checked,
                            "corruptionRejected": True, "missingRejected": True, "payloadUnchanged": True})

args.report.parent.mkdir(parents=True, exist_ok=True)
complete = all(case["client"].get("verified", False) for case in reports)
report = {"cases": reports, "allClientsVerified": complete, "releaseGate": "passed" if complete else "open"}
args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
print(json.dumps(report, indent=2))
if args.require_complete and not complete:
    sys.exit(5)
