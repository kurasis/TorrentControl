"""Independent parser/hash audit. This does not assert download-manager import compatibility."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/reference"))
from verify_torrent import bdecode, plain


def encode(value):
    if isinstance(value, bytes):
        return str(len(value)).encode() + b":" + value
    if isinstance(value, int):
        return b"i" + str(value).encode() + b"e"
    if isinstance(value, list):
        return b"l" + b"".join(map(encode, value)) + b"e"
    return b"d" + b"".join(encode(key) + encode(value[key]) for key in sorted(value)) + b"e"


def run(argv, code=0):
    result = subprocess.run(list(map(str, argv)), capture_output=True, text=True, timeout=60)
    assert result.returncode == code, f"exit {result.returncode}: {result.stdout} {result.stderr}"
    return json.loads(result.stdout)


def inventory(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob("*") if p.is_file()}


parser = argparse.ArgumentParser()
parser.add_argument("--proof", required=True, type=Path)
parser.add_argument("--jar", required=True, type=Path)
parser.add_argument("--java", default="java")
parser.add_argument("--report", required=True, type=Path)
args = parser.parse_args()
cases = []
with tempfile.TemporaryDirectory(prefix="tc-bigly-proof-") as directory:
    root = Path(directory)
    parent = root / "payload"
    folder = parent / "Release"
    for i, size in enumerate([0, 1, 16383, 16384, 16385, 65535, 65536, 65537, 200001]):
        path = folder / "данные #1" / f"file-{i}.bin"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(bytes((i + n) % 251 for n in range(size)))
    single = parent / "single.bin"
    single.write_bytes(bytes(n % 251 for n in range(70003)))

    def audit(torrent, code=0):
        # Private disposable config directory; no application Core is created.
        with tempfile.TemporaryDirectory(dir=root, prefix="config-") as config:
            before = inventory(parent)
            result = run([args.java, "--add-opens=java.base/java.net=ALL-UNNAMED",
                          f"-Dazureus.config.path={config}", "-cp", args.jar.resolve(),
                          ROOT / "tools/biglybt-proof/BiglyProof.java", torrent, parent], code)
            assert inventory(parent) == before, "audit changed payload inventory or bytes"
            return result

    for shape, source in [("multifile", folder), ("single-file", single)]:
        for format in ["v1", "v2", "hybrid"]:
            torrent = root / f"{shape}-{format}.torrent"
            created = run([args.proof.resolve(), "create", "--source", source, "--format", format,
                           "--piece-length", "65536", "-o", torrent])["result"]
            run([sys.executable, ROOT / "tools/reference/verify_torrent.py", torrent, "--root", source])
            checked = audit(torrent)
            assert checked["verified"]
            for family in ["v1", "v2"]:
                if f"infohash_{family}" in created:
                    assert checked[f"infohash{family.upper()}"] == created[f"infohash_{family}"]
            target = folder / "данные #1/file-8.bin" if shape == "multifile" else single
            original = target.read_bytes()
            target.write_bytes(bytes([original[0] ^ 1]) + original[1:])
            corrupt = audit(torrent, 5)
            for family in ["V1", "V2"]:
                if checked[f"has{family}"]:
                    assert not corrupt[f"verified{family}"]
            target.unlink()
            assert not audit(torrent, 5)["verified"]
            target.write_bytes(original)

            controls = []
            if format == "hybrid":
                # Mutate metadata ONLY in disposable negative-control files. The healthy torrent stays untouched.
                raw = torrent.read_bytes()
                for family in ["v1", "v2-root", "v2-layer"]:
                    metadata = plain(bdecode(raw))
                    if family == "v1":
                        hashes = metadata[b"info"][b"pieces"]
                        metadata[b"info"][b"pieces"] = bytes([hashes[0] ^ 1]) + hashes[1:]
                    elif family == "v2-layer":
                        layers = metadata[b"piece layers"]
                        key = next(iter(layers))
                        layers[key] = bytes([layers[key][0] ^ 1]) + layers[key][1:]
                    else:
                        def change_root(tree):
                            for key, value in tree.items():
                                if key == b"" and b"pieces root" in value:
                                    h = value[b"pieces root"]
                                    value[b"pieces root"] = bytes([h[0] ^ 1]) + h[1:]
                                    return True
                                if key != b"" and change_root(value):
                                    return True
                            return False
                        assert change_root(metadata[b"info"][b"file tree"])
                    negative = root / "negative.torrent"
                    negative.write_bytes(encode(metadata))
                    result = audit(negative, 5)
                    assert result["verifiedV1"] == (family != "v1")
                    assert result["verifiedV2"] == (family == "v1")
                    controls.append(family)
                assert torrent.read_bytes() == raw
            cases.append({"shape": shape, "format": format, "audit": checked,
                          "corruptionRejected": True, "missingRejected": True,
                          "payloadUnchanged": True, "independentHybridControls": controls})

report = {"engine": "BiglyBT 4.1.0.0", "scope": "parser-and-hash-routines",
          "cases": cases, "allHashAuditsVerified": True,
          "downloadManagerImportVerified": False, "releaseGate": "open"}
args.report.parent.mkdir(parents=True, exist_ok=True)
args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
print(json.dumps(report, indent=2))
