"""Fetch a checksum-pinned, developer-only BiglyBT jar; never ship it with the app."""
import argparse
import hashlib
from pathlib import Path
import urllib.request

URL = "https://github.com/BiglySoftware/BiglyBT/releases/download/v4.1.0.0/GitHub_BiglyBT.jar"
SHA256 = "ff2a3d1cbcf8d9b816ffd022cde33db1c610fd38857f75a10148dc4a21e83572"

parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
args = parser.parse_args()
if args.output.exists() and hashlib.sha256(args.output.read_bytes()).hexdigest() == SHA256:
    raise SystemExit(0)
with urllib.request.urlopen(URL, timeout=60) as response:
    data = response.read(40 * 1024 * 1024 + 1)
if hashlib.sha256(data).hexdigest() != SHA256:
    raise SystemExit("BiglyBT jar checksum mismatch")
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_bytes(data)
