#!/usr/bin/env python3
"""Independent reference verifier for BitTorrent v1, v2 and hybrid metainfo.

This script deliberately shares no code with the C++ core or libtorrent. It is
written directly from BEP 3, BEP 47 and BEP 52 using only the Python standard
library, so golden results do not come from the production encoder checking
itself (specification section 17).

Usage:
  verify_torrent.py TORRENT --root PATH        # PATH is the torrent root dir
                                               # (or the file in single-file mode)
  verify_torrent.py TORRENT --map MAP.json     # {"name/sub/file": "/native/path"}
  verify_torrent.py TORRENT                    # metainfo checks only

Prints a JSON report and exits 0 when every check passes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from dataclasses import dataclass, field

BLOCK = 16 * 1024


class BencodeError(ValueError):
    pass


@dataclass
class Node:
    value: object
    start: int
    end: int


def bdecode(data: bytes) -> Node:
    """Strict decoder that records the byte range of every value."""

    def parse(i: int, depth: int) -> Node:
        if depth > 128:
            raise BencodeError("nesting too deep")
        if i >= len(data):
            raise BencodeError("truncated")
        c = data[i : i + 1]
        if c == b"i":
            end = data.index(b"e", i)
            text = data[i + 1 : end].decode("ascii")
            if text in ("", "-", "-0") or (text.lstrip("-").startswith("0") and text.lstrip("-") != "0"):
                raise BencodeError(f"invalid integer at {i}")
            return Node(int(text), i, end + 1)
        if c.isdigit():
            colon = data.index(b":", i)
            length_text = data[i:colon]
            if len(length_text) > 1 and length_text.startswith(b"0"):
                raise BencodeError(f"invalid length at {i}")
            length = int(length_text)
            start = colon + 1
            if start + length > len(data):
                raise BencodeError("string past end")
            return Node(data[start : start + length], i, start + length)
        if c == b"l":
            items, j = [], i + 1
            while data[j : j + 1] != b"e":
                child = parse(j, depth + 1)
                items.append(child)
                j = child.end
            return Node(items, i, j + 1)
        if c == b"d":
            entries: dict[bytes, Node] = {}
            j, previous = i + 1, None
            while data[j : j + 1] != b"e":
                key = parse(j, depth + 1)
                if not isinstance(key.value, bytes):
                    raise BencodeError("non-string key")
                if key.value in entries:
                    raise BencodeError("duplicate key")
                if previous is not None and key.value < previous:
                    raise BencodeError("unsorted keys")
                previous = key.value
                val = parse(key.end, depth + 1)
                entries[key.value] = val
                j = val.end
            return Node(entries, i, j + 1)
        raise BencodeError(f"unexpected byte at {i}")

    root = parse(0, 0)
    if root.end != len(data):
        raise BencodeError("trailing data")
    return root


def plain(node: Node):
    v = node.value
    if isinstance(v, list):
        return [plain(x) for x in v]
    if isinstance(v, dict):
        return {k: plain(x) for k, x in v.items()}
    return v


def native(path: str) -> str:
    """Extended-length form on Windows so long paths work without registry opt-in."""
    if os.name == "nt":
        path = os.path.abspath(path)
        if not path.startswith("\\\\?\\"):
            path = "\\\\?\\UNC\\" + path[2:] if path.startswith("\\\\") else "\\\\?\\" + path
    return path


def merkle_root(leaves: list[bytes], width: int) -> bytes:
    layer = leaves + [bytes(32)] * (width - len(leaves))
    while len(layer) > 1:
        layer = [hashlib.sha256(layer[k] + layer[k + 1]).digest() for k in range(0, len(layer), 2)]
    return layer[0]


def next_pow2(n: int) -> int:
    p = 1
    while p < n:
        p *= 2
    return p


@dataclass
class FileEntry:
    path: str  # "name/sub/file" or "name" in single-file mode
    length: int
    pad: bool = False
    pieces_root: bytes | None = None


@dataclass
class Report:
    ok: bool = True
    errors: list[str] = field(default_factory=list)
    checks: list[str] = field(default_factory=list)

    def fail(self, message: str) -> None:
        self.ok = False
        self.errors.append(message)


def v1_files(info: dict, name: str) -> list[FileEntry]:
    if b"files" not in info:
        return [FileEntry(name, info[b"length"], pad=b"p" in info.get(b"attr", b""))]
    out = []
    for f in info[b"files"]:
        parts = [p.decode("utf-8") for p in f[b"path"]]
        out.append(FileEntry("/".join([name] + parts), f[b"length"], pad=b"p" in f.get(b"attr", b"")))
    return out


def v2_files(tree: dict, name: str, single: bool) -> list[FileEntry]:
    out: list[FileEntry] = []

    def walk(node: dict, prefix: list[str]) -> None:
        for key in sorted(node):
            child = node[key]
            if key == b"":
                continue
            if b"" in child:
                props = child[b""]
                parts = prefix + [key.decode("utf-8")]
                out.append(FileEntry("/".join(parts), props[b"length"], pieces_root=props.get(b"pieces root")))
            else:
                walk(child, prefix + [key.decode("utf-8")])

    walk(tree, [])
    if single and len(out) == 1:
        out[0].path = name
    else:
        for f in out:
            f.path = name + "/" + f.path
    return out


def is_single_file(info: dict, has_v1: bool) -> bool:
    if has_v1:
        return b"files" not in info
    # Pure v2: a single-file torrent's tree holds exactly one file named `name`.
    tree = info[b"file tree"]
    return len(tree) == 1 and info[b"name"] in tree and b"" in tree[info[b"name"]]


def read_file(path: str, expected: int):
    total = 0
    with open(native(path), "rb") as fh:
        while True:
            chunk = fh.read(1 << 20)
            if not chunk:
                break
            total += len(chunk)
            yield chunk
    if total != expected:
        raise ValueError(f"length mismatch for {path}: {total} != {expected}")


def verify(torrent_path: str, mapping: dict[str, str] | None) -> dict:
    with open(native(torrent_path), "rb") as fh:
        data = fh.read()
    report = Report()
    root = bdecode(data)
    top = root.value
    info_node = top[b"info"]
    raw_info = data[info_node.start : info_node.end]
    info = plain(info_node)
    name = info[b"name"].decode("utf-8")
    piece_length = info[b"piece length"]
    has_v1 = b"pieces" in info
    has_v2 = info.get(b"meta version") == 2
    fmt = "hybrid" if has_v1 and has_v2 else ("v2" if has_v2 else "v1")

    result: dict = {"format": fmt, "name": name, "piece_length": piece_length}
    if has_v1:
        result["infohash_v1"] = hashlib.sha1(raw_info).hexdigest()
    if has_v2:
        result["infohash_v2"] = hashlib.sha256(raw_info).hexdigest()

    if piece_length < BLOCK or piece_length & (piece_length - 1):
        if has_v2:
            report.fail("v2 piece length must be a power of two >= 16 KiB")

    files1 = v1_files(info, name) if has_v1 else []
    files2 = v2_files(info[b"file tree"], name, is_single_file(info, has_v1)) if has_v2 else []

    if has_v1 and has_v2:
        real1 = [(f.path, f.length) for f in files1 if not f.pad]
        real2 = [(f.path, f.length) for f in files2]
        if real1 != real2:
            report.fail("hybrid v1 and v2 file lists differ")
        else:
            report.checks.append("hybrid layouts match")
        offset = 0
        for f in files1:
            if not f.pad and f is not files1[-1] and offset % piece_length != 0:
                report.fail(f"hybrid file not piece aligned: {f.path}")
            offset += f.length

    piece_layers = plain(top[b"piece layers"]) if b"piece layers" in top else {}
    result["files"] = [{"path": f.path, "length": f.length, "pad": f.pad} for f in (files1 or files2)]

    if mapping is None:
        result.update(ok=report.ok, errors=report.errors, checks=report.checks)
        return result

    if has_v1:
        pieces = info[b"pieces"]
        expected_pieces = [pieces[k : k + 20] for k in range(0, len(pieces), 20)]
        h, fill, index = hashlib.sha1(), 0, 0

        def feed(chunk: bytes) -> None:
            nonlocal h, fill, index
            while chunk:
                take = min(len(chunk), piece_length - fill)
                h.update(chunk[:take])
                fill += take
                chunk = chunk[take:]
                if fill == piece_length:
                    if index >= len(expected_pieces) or h.digest() != expected_pieces[index]:
                        report.fail(f"v1 piece {index} mismatch")
                    h, fill, index = hashlib.sha1(), 0, index + 1

        for f in files1:
            if f.pad:
                feed(bytes(f.length))
                continue
            for chunk in read_file(mapping[f.path], f.length):
                feed(chunk)
        if fill:
            if index >= len(expected_pieces) or h.digest() != expected_pieces[index]:
                report.fail(f"v1 piece {index} mismatch")
            index += 1
        if index != len(expected_pieces):
            report.fail(f"v1 piece count {index} != {len(expected_pieces)}")
        else:
            report.checks.append(f"v1: {index} pieces verified")

    if has_v2:
        blocks_per_piece = piece_length // BLOCK
        seen_layers = set()
        for f in files2:
            if f.length == 0:
                if f.pieces_root is not None:
                    report.fail(f"empty file has pieces root: {f.path}")
                continue
            leaves = []
            buf = b""
            for chunk in read_file(mapping[f.path], f.length):
                buf += chunk
                while len(buf) >= BLOCK:
                    leaves.append(hashlib.sha256(buf[:BLOCK]).digest())
                    buf = buf[BLOCK:]
            if buf:
                leaves.append(hashlib.sha256(buf).digest())
            root_hash = merkle_root(leaves, next_pow2(len(leaves)))
            if root_hash != f.pieces_root:
                report.fail(f"v2 pieces root mismatch: {f.path}")
                continue
            if f.length > piece_length:
                layer = b"".join(
                    merkle_root(leaves[k : k + blocks_per_piece], blocks_per_piece)
                    for k in range(0, len(leaves), blocks_per_piece)
                )
                if piece_layers.get(root_hash) != layer:
                    report.fail(f"v2 piece layer mismatch: {f.path}")
                seen_layers.add(root_hash)
        extra = set(piece_layers) - seen_layers
        if extra:
            report.fail(f"{len(extra)} unexpected piece layer entries")
        report.checks.append(f"v2: {len(files2)} file roots verified")

    result.update(ok=report.ok, errors=report.errors, checks=report.checks)
    return result


def build_mapping(args, torrent_path: str) -> dict[str, str] | None:
    if args.map:
        with open(args.map, encoding="utf-8") as fh:
            return json.load(fh)
    if not args.root:
        return None
    report = verify(torrent_path, None)
    name = report["name"]
    mapping = {}
    for f in report["files"]:
        if f["pad"]:
            continue
        if f["path"] == name and len(report["files"]) == 1:
            mapping[f["path"]] = args.root
        else:
            rel = f["path"][len(name) + 1 :]
            mapping[f["path"]] = os.path.join(args.root, *rel.split("/"))
    return mapping


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("torrent")
    parser.add_argument("--root")
    parser.add_argument("--map")
    args = parser.parse_args()
    try:
        result = verify(args.torrent, build_mapping(args, args.torrent))
    except (BencodeError, KeyError, ValueError, OSError) as exc:
        result = {"ok": False, "errors": [f"{type(exc).__name__}: {exc}"]}
    # Always UTF-8, independent of the console code page (cp1252 on Windows
    # cannot encode non-ASCII file names).
    text = json.dumps(result, indent=2, ensure_ascii=False) + "\n"
    sys.stdout.buffer.write(text.encode("utf-8"))
    sys.stdout.flush()
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
