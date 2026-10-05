"""Headless integration proof (specification section 3.1).

Drives the tc-proof developer tool and checks every produced torrent with the
independent reference verifier in tools/reference/verify_torrent.py.

Environment:
  TC_PROOF     path to the tc-proof executable
  TC_VERIFIER  path to verify_torrent.py
"""

from __future__ import annotations

import hashlib
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import unittest

TC_PROOF = os.environ.get("TC_PROOF", "tc-proof")
TC_VERIFIER = os.environ.get("TC_VERIFIER", os.path.join(os.path.dirname(__file__), "..", "..", "tools", "reference", "verify_torrent.py"))

KIB = 1024


def native(path: str) -> str:
    if os.name == "nt":
        path = os.path.abspath(path)
        if not path.startswith("\\\\?\\"):
            path = "\\\\?\\" + path
    return path


def write(path: str, size: int, seed: int = 0) -> None:
    os.makedirs(native(os.path.dirname(path)), exist_ok=True)
    rng = random.Random(seed)
    with open(native(path), "wb") as fh:
        remaining = size
        while remaining:
            n = min(remaining, 1 << 20)
            fh.write(rng.randbytes(n))
            remaining -= n


def run_json(args: list[str], expect_code: int | None = 0) -> dict:
    proc = subprocess.run(args, capture_output=True)
    out = proc.stdout.decode("utf-8")
    if expect_code is not None and proc.returncode != expect_code:
        raise AssertionError(f"{args[:2]} exited {proc.returncode}\nstdout: {out}\nstderr: {proc.stderr.decode(errors='replace')}")
    try:
        return json.loads(out)
    except json.JSONDecodeError as exc:
        raise AssertionError(f"{args[:2]} printed invalid JSON ({exc})\nstdout: {out}\nstderr: {proc.stderr.decode(errors='replace')}") from exc


def proof(*args: str, expect_code: int | None = 0) -> dict:
    return run_json([TC_PROOF, *args], expect_code)


def verify(torrent: str, root: str | None = None, mapping: dict | None = None) -> dict:
    args = [sys.executable, TC_VERIFIER, torrent]
    if root is not None:
        args += ["--root", root]
    if mapping is not None:
        map_path = torrent + ".map.json"
        with open(map_path, "w", encoding="utf-8") as fh:
            json.dump(mapping, fh, ensure_ascii=False)
        args += ["--map", map_path]
    return run_json(args, expect_code=None)


class ProofTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="tc-proof-")

    def tearDown(self) -> None:
        shutil.rmtree(native(self.tmp), ignore_errors=True)

    def path(self, *parts: str) -> str:
        return os.path.join(self.tmp, *parts)

    def create_and_verify(self, source: str, fmt: str, *extra: str, root: str | None = None) -> tuple[dict, dict]:
        out = self.path(f"out-{fmt}-{len(os.listdir(self.tmp))}.torrent")
        created = proof("create", "--source", source, "--format", fmt, *extra, "-o", out)
        self.assertEqual(created["status"], "succeeded", created)
        checked = verify(out, root=root or source)
        self.assertTrue(checked["ok"], checked)
        self.assertEqual(checked["format"], fmt)
        # Identifiers from the production code must match the independent ones.
        for key in ("infohash_v1", "infohash_v2"):
            self.assertEqual(created["result"].get(key), checked.get(key), key)
        # Single pass: the engine read exactly the payload, once.
        self.assertEqual(created["payload_bytes_read"], created["payload_bytes"])
        self.assertEqual(created["padding_bytes_processed"], created["padding_bytes"])
        return created, checked


class FormatTests(ProofTestCase):
    def make_dataset(self) -> str:
        root = self.path("Release")
        write(os.path.join(root, "bin", "app.exe"), 300 * KIB + 7, 1)
        write(os.path.join(root, "readme.txt"), 1000, 2)
        write(os.path.join(root, "empty.dat"), 0)
        write(os.path.join(root, "data", "блок.bin"), 64 * KIB, 3)
        return root

    def test_v1_v2_hybrid_with_explicit_and_auto_piece_sizes(self) -> None:
        root = self.make_dataset()
        for fmt in ("v1", "v2", "hybrid"):
            for piece in ("16384", "65536", "0"):
                with self.subTest(fmt=fmt, piece=piece):
                    self.create_and_verify(root, fmt, "--piece-length", piece)

    def test_v1_single_file_boundary_sizes(self) -> None:  # F01
        piece = 32 * KIB
        for size in (1, piece - 1, piece, piece + 1):
            with self.subTest(size=size):
                f = self.path(f"single-{size}.bin")
                write(f, size, size)
                created, _ = self.create_and_verify(f, "v1", "--piece-length", str(piece))
                self.assertEqual(created["num_pieces"], -(-size // piece))

    def test_v2_and_hybrid_boundary_sizes(self) -> None:  # F03
        piece = 64 * KIB
        root = self.path("Sizes")
        for size in (0, 1, 16383, 16384, 16385, piece - 1, piece, piece + 1, 5 * 16 * KIB + 3):
            write(os.path.join(root, f"s{size:07d}"), size, size)
        for fmt in ("v2", "hybrid"):
            with self.subTest(fmt=fmt):
                self.create_and_verify(root, fmt, "--piece-length", str(piece))

    def test_v1_pieces_cross_file_boundaries(self) -> None:  # F02
        root = self.path("Split")
        for i, size in enumerate((10 * KIB, 30 * KIB, 5, 40 * KIB)):
            write(os.path.join(root, f"part{i}"), size, i)
        self.create_and_verify(root, "v1", "--piece-length", "16384")

    def test_hybrid_tiny_files_and_one_large_file(self) -> None:  # F05
        root = self.path("Mixed")
        for i in range(50):
            write(os.path.join(root, "tiny", f"t{i:02d}"), i + 1, i)
        write(os.path.join(root, "large.bin"), 2 * 1024 * KIB + 99, 7)
        created, checked = self.create_and_verify(root, "hybrid", "--piece-length", "32768")
        pads = [f for f in checked["files"] if f["pad"]]
        self.assertTrue(pads, "hybrid layout should contain BEP 47 padding")
        self.assertGreater(created["padding_bytes"], 0)
        # Padding is virtual: nothing is created beside the user's files.
        self.assertFalse(os.path.exists(os.path.join(root, ".pad")))

    def test_canonical_order_is_independent_of_input_order(self) -> None:
        root = self.path("Order")
        for name in ("b", "A", "ä", "Z", "1", "_x"):
            write(os.path.join(root, name), 20 * KIB, ord(name[0]))
        _, checked = self.create_and_verify(root, "hybrid", "--piece-length", "16384")
        real = [f["path"] for f in checked["files"] if not f["pad"]]
        self.assertEqual(real, sorted(real, key=lambda p: p.encode("utf-8")))


class MappingTests(ProofTestCase):
    def test_virtual_paths_from_unrelated_directories(self) -> None:  # F10
        a = self.path("volume-a", "photos", "img.jpg")
        b = self.path("volume-b", "docs", "deep", "notes.txt")
        write(a, 100 * KIB, 1)
        write(b, 17, 2)
        out = self.path("collection.torrent")
        for fmt in ("v1", "v2", "hybrid"):
            with self.subTest(fmt=fmt):
                created = proof("create", "--name", "Collection", "--format", fmt, "--map", a, "Pictures/img.jpg",
                                "--map", b, "Text/notes.txt", "--replace", "-o", out)
                self.assertEqual(created["status"], "succeeded", created)
                checked = verify(out, mapping={"Collection/Pictures/img.jpg": a, "Collection/Text/notes.txt": b})
                self.assertTrue(checked["ok"], checked)
                self.assertFalse(os.path.exists(self.path("Collection")))

    def test_unicode_and_long_paths(self) -> None:  # W01
        root = self.path("Набор данных 🎵")
        deep = root
        for i in range(10):
            deep = os.path.join(deep, f"очень-длинное-имя-папки-{i:02d}")
        target = os.path.join(deep, "файл с пробелами.bin")
        self.assertGreater(len(target), 260)
        write(target, 70 * KIB, 9)
        write(os.path.join(root, "日本語.txt"), 3, 10)
        for fmt in ("v1", "hybrid"):
            with self.subTest(fmt=fmt):
                created, _ = self.create_and_verify(root, fmt)
                self.assertEqual(created["result"]["name"], "Набор данных 🎵")

    def test_empty_dataset_is_a_specific_error(self) -> None:  # F08
        root = self.path("Empty")
        write(os.path.join(root, "a"), 0)
        result = proof("create", "--source", root, "-o", self.path("e.torrent"), expect_code=2)
        self.assertEqual(result["code"], "EMPTY_PAYLOAD")


class LifecycleTests(ProofTestCase):
    def test_progress_and_cancellation(self) -> None:
        big = self.path("big.bin")
        write(big, 32 * 1024 * KIB, 4)
        out = self.path("cancelled.torrent")
        result = proof("create", "--source", big, "--buffer", str(256 * KIB), "--cancel-after-bytes", str(1024 * KIB),
                       "-o", out, expect_code=3)
        self.assertEqual(result["status"], "cancelled")
        self.assertGreater(result["progress_events"], 1)
        self.assertLess(result["payload_bytes_read"], 32 * 1024 * KIB)
        self.assertFalse(os.path.exists(out))

    def test_outer_only_edit_keeps_raw_info(self) -> None:  # E01
        root = self.path("Edit")
        write(os.path.join(root, "x.bin"), 50 * KIB, 3)
        original = self.path("original.torrent")
        edited = self.path("edited.torrent")
        proof("create", "--source", root, "--format", "hybrid", "--tracker", "udp://a.example:1/announce",
              "--comment", "before", "-o", original)
        result = proof("edit-outer", original, edited, "--set-comment", "after", "--set-announce",
                       "https://b.example/announce", "--remove", "announce-list")
        self.assertTrue(result["raw_info_identical"])
        self.assertEqual(result["before"]["infohash_v1"], result["after"]["infohash_v1"])
        self.assertEqual(result["before"]["infohash_v2"], result["after"]["infohash_v2"])
        # Independent check: identical info slices and a still-valid payload.
        before, after = verify(original, root=root), verify(edited, root=root)
        self.assertTrue(after["ok"], after)
        self.assertEqual(before["infohash_v2"], after["infohash_v2"])
        self.assertEqual(info_slice(original), info_slice(edited))

    def test_reproducible_output(self) -> None:  # E08
        root = self.path("Repro")
        write(os.path.join(root, "a"), 40 * KIB, 1)
        write(os.path.join(root, "b"), 3, 2)
        outs = [self.path("r1.torrent"), self.path("r2.torrent")]
        for out in outs:
            proof("create", "--source", root, "--format", "hybrid", "--creator", "fixed", "-o", out)
        with open(outs[0], "rb") as f1, open(outs[1], "rb") as f2:
            self.assertEqual(f1.read(), f2.read())

    def test_hybrid_magnet_has_both_topics(self) -> None:  # E09
        root = self.path("Magnet & Co")
        write(os.path.join(root, "a"), 20 * KIB, 1)
        out = self.path("m.torrent")
        created = proof("create", "--source", root, "--format", "hybrid", "-o", out)
        checked = verify(out)
        magnet = created["result"]["magnet"]
        self.assertIn("xt=urn:btih:" + checked["infohash_v1"], magnet)
        self.assertIn("xt=urn:btmh:1220" + checked["infohash_v2"], magnet)
        self.assertIn("dn=Magnet%20%26%20Co", magnet)


def flip_byte(path: str, offset: int) -> None:
    with open(native(path), "r+b") as fh:
        fh.seek(offset)
        b = fh.read(1)
        fh.seek(offset)
        fh.write(bytes([b[0] ^ 0xFF]))


def make_junction(link: str, target: str) -> bool:
    """Creates a directory link: a junction on Windows, a symlink elsewhere."""
    try:
        if os.name == "nt":
            proc = subprocess.run(["cmd", "/c", "mklink", "/J", link, target], capture_output=True)
            return proc.returncode == 0
        os.symlink(target, link, target_is_directory=True)
        return True
    except OSError:
        return False


class LayoutFixtureTests(ProofTestCase):
    def test_identical_files_share_layers(self) -> None:  # F06
        root = self.path("Twins")
        for name in ("a.bin", "b.bin", "sub/c.bin"):
            write(os.path.join(root, name), 96 * KIB, 42)
        for fmt in ("v2", "hybrid"):
            with self.subTest(fmt=fmt):
                created, checked = self.create_and_verify(root, fmt, "--piece-length", "16384")
                real = [f for f in checked["files"] if not f["pad"]]
                self.assertEqual(len(real), 3)
                validated = proof("validate", self.path(f"out-{fmt}-{len(os.listdir(self.tmp)) - 1}.torrent"))
                self.assertTrue(validated["valid"], validated)

    def test_empty_files_are_kept(self) -> None:  # F07
        root = self.path("Sparse")
        write(os.path.join(root, "a-empty"), 0)
        write(os.path.join(root, "b-data"), 20 * KIB, 1)
        write(os.path.join(root, "c", "d-empty"), 0)
        write(os.path.join(root, "e-data"), 5, 2)
        for fmt in ("v1", "v2", "hybrid"):
            with self.subTest(fmt=fmt):
                _, checked = self.create_and_verify(root, fmt, "--piece-length", "16384")
                paths = {f["path"] for f in checked["files"] if not f["pad"]}
                self.assertIn("Sparse/a-empty", paths)
                self.assertIn("Sparse/c/d-empty", paths)

    def test_folder_with_one_file_versus_single_file(self) -> None:  # F09
        folder = self.path("Album")
        song = os.path.join(folder, "track.flac")
        write(song, 40 * KIB, 5)
        for fmt in ("v1", "v2", "hybrid"):
            with self.subTest(fmt=fmt, mode="folder"):
                created, checked = self.create_and_verify(folder, fmt)
                self.assertEqual(created["result"]["name"], "Album")
                self.assertEqual([f["path"] for f in checked["files"] if not f["pad"]], ["Album/track.flac"])
            with self.subTest(fmt=fmt, mode="single"):
                created, checked = self.create_and_verify(song, fmt)
                self.assertEqual(created["result"]["name"], "track.flac")
                self.assertEqual([f["path"] for f in checked["files"]], ["track.flac"])

    def test_auto_piece_size_on_many_small_files(self) -> None:  # F11
        root = self.path("Small")
        for i in range(200):
            write(os.path.join(root, f"f{i:03d}"), 100 + i, i)
        created, _ = self.create_and_verify(root, "hybrid")
        self.assertTrue(any("padding" in w["message"] for w in created["warnings"]), created["warnings"])
        self.assertGreater(created["estimated_memory_bytes"], 0)


class MetainfoFixtureTests(ProofTestCase):
    def make_torrent(self, fmt: str = "hybrid") -> tuple[str, str]:
        root = self.path("Payload")
        write(os.path.join(root, "a.bin"), 100 * KIB, 1)
        write(os.path.join(root, "b.bin"), 30 * KIB + 1, 2)
        out = self.path(f"{fmt}.torrent")
        proof("create", "--source", root, "--format", fmt, "--piece-length", "16384", "-o", out)
        return root, out

    def test_info_edit_keeps_payload_hashes(self) -> None:  # E04
        root, original = self.make_torrent()
        edited = self.path("edited.torrent")
        result = proof("edit-info", original, edited, "--set-source", "TRACKER-X", "--set-private")
        self.assertTrue(result["pieces_identical"])
        self.assertEqual(result["problems"], [])
        self.assertNotEqual(result["before"]["infohash_v1"], result["after"]["infohash_v1"])
        self.assertNotEqual(result["before"]["infohash_v2"], result["after"]["infohash_v2"])
        checked = verify(edited, root=root)
        self.assertTrue(checked["ok"], checked)
        self.assertEqual(checked["infohash_v2"], result["after"]["infohash_v2"])
        self.assertTrue(proof("verify", edited, "--root", root)["ok"])

    def test_mutated_piece_layer_is_rejected(self) -> None:  # E05
        _, original = self.make_torrent("v2")
        sys.path.insert(0, os.path.dirname(os.path.abspath(TC_VERIFIER)))
        import verify_torrent  # noqa: E402

        with open(original, "rb") as fh:
            data = fh.read()
        layers = verify_torrent.bdecode(data).value[b"piece layers"]
        layer = next(iter(layers.value.values()))
        # Flip one byte inside the first layer; info (and so the infohash) is untouched.
        pos = layer.end - 1
        broken = data[:pos] + bytes([data[pos] ^ 0xFF]) + data[pos + 1 :]
        mutated = self.path("mutated.torrent")
        with open(mutated, "wb") as fh:
            fh.write(broken)

        before = proof("validate", original)
        after = proof("validate", mutated, expect_code=5)
        self.assertTrue(before["valid"])
        self.assertFalse(after["valid"])
        self.assertEqual(before["infohash_v2"], after["infohash_v2"])
        self.assertTrue(any("pieces root" in p for p in after["problems"]), after["problems"])

    def test_verify_reports_corruption(self) -> None:
        root, torrent = self.make_torrent()
        self.assertTrue(proof("verify", torrent, "--root", root)["ok"])
        flip_byte(os.path.join(root, "a.bin"), 50 * KIB)
        result = proof("verify", torrent, "--root", root, expect_code=5)
        status = {f["path"]: f["status"] for f in result["files"]}
        self.assertEqual(status["Payload/a.bin"], "corrupt")
        self.assertEqual(status["Payload/b.bin"], "ok")


class FilesystemFixtureTests(ProofTestCase):
    def test_output_may_not_overwrite_a_source(self) -> None:  # W06
        root = self.path("Src")
        write(os.path.join(root, "a.bin"), 20 * KIB, 1)
        write(os.path.join(root, "b.bin"), 20 * KIB, 2)
        target = os.path.join(root, "b.bin")
        result = proof("create", "--source", root, "--replace", "-o", target, expect_code=2)
        self.assertEqual(result["code"], "OUTPUT_CONFLICT")
        with open(target, "rb") as fh:
            self.assertEqual(len(fh.read()), 20 * KIB)

        alias = self.path("alias.torrent")
        try:
            os.link(target, alias)
        except OSError as exc:
            self.skipTest(f"hard links unavailable: {exc}")
        result = proof("create", "--source", root, "--replace", "-o", alias, expect_code=2)
        self.assertEqual(result["code"], "OUTPUT_CONFLICT")
        self.assertEqual(os.path.getsize(target), 20 * KIB)

    def test_existing_output_needs_explicit_replace(self) -> None:  # W07
        root = self.path("Src")
        write(os.path.join(root, "a.bin"), 20 * KIB, 1)
        out = self.path("out.torrent")
        with open(out, "wb") as fh:
            fh.write(b"previous")
        result = proof("create", "--source", root, "-o", out, expect_code=2)
        self.assertEqual(result["code"], "OUTPUT_CONFLICT")
        with open(out, "rb") as fh:
            self.assertEqual(fh.read(), b"previous")
        created = proof("create", "--source", root, "--replace", "-o", out)
        self.assertTrue(created["replaced_existing"])
        self.assertTrue(verify(out, root=root)["ok"])
        self.assertEqual([n for n in os.listdir(self.tmp) if ".tc-" in n], [])

    def test_directory_links_are_not_traversed_by_default(self) -> None:  # W02
        root = self.path("Linked")
        outside = self.path("Outside")
        write(os.path.join(root, "a.bin"), 10 * KIB, 1)
        write(os.path.join(outside, "secret.bin"), 10 * KIB, 2)
        if not make_junction(os.path.join(root, "loop"), root) or not make_junction(os.path.join(root, "out"), outside):
            self.skipTest("directory links cannot be created here")

        scanned = proof("scan", root)
        self.assertEqual([e["torrent_path"] for e in scanned["entries"]], ["Linked/a.bin"])
        kinds = {os.path.basename(s["path"]): s["kind"] for s in scanned["skipped"]}
        expected = "junction" if os.name == "nt" else "symbolic-link"
        self.assertEqual(kinds, {"loop": expected, "out": expected})
        self.assertTrue(all(s["reason"] for s in scanned["skipped"]))

        followed = proof("scan", root, "--follow-links")
        kinds = {os.path.basename(s["path"]): s["kind"] for s in followed["skipped"]}
        self.assertEqual(kinds, {"loop": "link-cycle", "out": "outside-root"})
        self.assertEqual([e["torrent_path"] for e in followed["entries"]], ["Linked/a.bin"])

    @unittest.skipUnless(os.name == "nt", "UNC paths are Windows-specific")
    def test_unc_path(self) -> None:  # W01
        root = self.path("Share")
        write(os.path.join(root, "a.bin"), 20 * KIB, 1)
        full = os.path.abspath(root)
        unc = "\\\\localhost\\" + full[0] + "$" + full[2:]
        if not os.path.exists(unc):
            self.skipTest("administrative share is not reachable")
        self.create_and_verify(unc, "hybrid", root=root)


def info_slice(torrent: str) -> bytes:
    sys.path.insert(0, os.path.dirname(os.path.abspath(TC_VERIFIER)))
    import verify_torrent  # noqa: E402

    with open(torrent, "rb") as fh:
        data = fh.read()
    node = verify_torrent.bdecode(data).value[b"info"]
    return data[node.start : node.end]


if __name__ == "__main__":
    unittest.main()
