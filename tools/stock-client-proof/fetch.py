"""Checksum-pinned official developer clients; never include these in app releases."""
import argparse
import hashlib
from pathlib import Path
import urllib.request

ARTIFACTS = {
    "BiglyBT.jar": (
        "https://github.com/BiglySoftware/BiglyBT/releases/download/v4.1.0.0/GitHub_BiglyBT.jar",
        "ff2a3d1cbcf8d9b816ffd022cde33db1c610fd38857f75a10148dc4a21e83572"),
    "qbittorrent-5.2.4_lt20_x86_64.AppImage": (
        "https://github.com/qbittorrent/qBittorrent/releases/download/release-5.2.4/qbittorrent-5.2.4_lt20_x86_64.AppImage",
        "eda6c0ca39f9befb0cd111b76a3d8b8bf3fd859323f2d4aac8c18f63766c2a1a"),
}


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for name, (url, expected) in ARTIFACTS.items():
        path = args.output / name
        if not path.exists() or sha256(path) != expected:
            pending = path.with_name(name + ".download")
            try:
                with urllib.request.urlopen(url, timeout=60) as source, pending.open("wb") as target:
                    total = 0
                    while data := source.read(1024 * 1024):
                        total += len(data)
                        if total > 150 * 1024 * 1024:
                            raise ValueError("Client artifact exceeds developer download limit")
                        target.write(data)
                if sha256(pending) != expected:
                    raise ValueError(f"Official client checksum mismatch: {name}")
                pending.replace(path)
            finally:
                pending.unlink(missing_ok=True)
        if name.endswith(".AppImage"):
            path.chmod(0o755)
        print(f"Verified {name}: {expected}")


if __name__ == "__main__":
    main()
