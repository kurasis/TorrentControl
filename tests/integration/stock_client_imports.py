"""Real stock download-manager imports and rechecks; no metadata/path repair."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/stock-client-proof"))
from fetch import ARTIFACTS, sha256


def run(argv):
    return subprocess.run(list(map(str, argv)), capture_output=True, text=True, check=True, timeout=120).stdout


def inventory(root):
    return {str(p.relative_to(root)): sha256(p) for p in root.rglob("*") if p.is_file()}


class Client:
    def __init__(self, command, log):
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=log, text=True, encoding="utf-8")
        self.responses = queue.Queue()
        def read():
            for line in self.process.stdout:
                self.responses.put(line)
            self.responses.put(None)
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()

    def receive(self, timeout=150):
        try:
            value = self.responses.get(timeout=timeout)
        except queue.Empty as error:
            raise TimeoutError("Stock client response deadline exceeded") from error
        if value is None:
            raise RuntimeError(f"Stock client exited before a response: {self.process.poll()}")
        return json.loads(value)

    def check(self):
        self.process.stdin.write("check\n"); self.process.stdin.flush()
        return self.receive()

    def finish(self):
        self.process.stdin.write("quit\n"); self.process.stdin.flush()
        assert self.receive()["normalExit"]
        self.process.stdin.close()
        assert self.process.wait(timeout=30) == 0
        self.reader.join(timeout=5)
        self.process.stdout.close()

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=10)
        if not self.process.stdin.closed:
            self.process.stdin.close()
        self.reader.join(timeout=5)
        self.process.stdout.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--proof", type=Path, required=True)
    parser.add_argument("--clients", type=Path, required=True)
    parser.add_argument("--image", default="tc-stock-clients:local")
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--require-complete", action="store_true",
                        help="Fail if any documented stock-client compatibility gate remains open")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("This stock-client fixture runs on Linux with Docker and Xvfb")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    details = args.report.parent / (args.report.stem + "-details")
    details.mkdir(exist_ok=False)
    profiles, logs, fixture = (details / name for name in ("profiles", "logs", "fixture"))
    for folder in (profiles, logs, fixture): folder.mkdir()
    payload = fixture / "payload"; payload.mkdir()
    folder = payload / "Release"
    for i, size in enumerate([0, 1, 16383, 16384, 16385, 65535, 65536, 65537, 200001]):
        path = folder / "данные #1" / f"file-{i}.bin"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(bytes((i + n) % 251 for n in range(size)))
    single = payload / "single.bin"
    single.write_bytes(bytes(n % 251 for n in range(70003)))
    torrents = fixture / "torrents"; torrents.mkdir()
    container = "tc-stock-" + uuid.uuid4().hex
    active = False
    app_directory = tempfile.TemporaryDirectory(prefix="tc-stock-app-", dir=args.report.parent)
    report = {"scope": "Official qBittorrent GUI binary via Web API and unmodified BiglyBT Core/GlobalManager; Linux, original metainfo and natural save paths",
        "clients": {}, "cases": [], "networkMode": "none", "readOnlyClientInputs": True,
        "limitations": "No peer downloads, tracker connectivity, Windows client UI or universal client compatibility claim. Hybrid rechecks do not by themselves prove both hash families; the independent BiglyBT hash audit is a separate gate.",
        "passed": False, "releaseGate": "open"}
    try:
        for name, (url, expected) in ARTIFACTS.items():
            path = args.clients.resolve() / name
            assert sha256(path) == expected, f"Stock artifact checksum mismatch: {name}"
            report["clients"][name] = {"url": url, "sha256": expected}
        app = Path(app_directory.name)
        appimage = args.clients.resolve() / "qbittorrent-5.2.4_lt20_x86_64.AppImage"
        # Fresh extraction from the verified vendor file prevents patched cache reuse.
        with (logs / "appimage-extraction.log").open("w") as log:
            subprocess.run([str(appimage), "--appimage-extract"], cwd=app, stdout=log,
                           stderr=subprocess.STDOUT, check=True, timeout=60)
        mounts = [(ROOT, "/repo", True), (fixture.resolve(), "/fixture", True),
                  (profiles.resolve(), "/profiles", False), (app / "squashfs-root", "/clients/qbt", True),
                  (args.clients.resolve() / "BiglyBT.jar", "/clients/BiglyBT.jar", True)]
        command = ["docker", "run", "--detach", "--name", container, "--network", "none", "--read-only",
            "--user", f"{os.getuid()}:{os.getgid()}", "--cap-drop", "ALL", "--security-opt", "no-new-privileges",
            "--tmpfs", "/tmp:rw,mode=1777,size=512m", "--workdir", "/tmp"]
        for source, target, readonly in mounts:
            command += ["--mount", f"type=bind,source={source},target={target}" + (",readonly" if readonly else "")]
        command.append(args.image)
        run(command); active = True
        assert run(["docker", "inspect", container, "--format", "{{.HostConfig.NetworkMode}}"]).strip() == "none"
        interfaces = json.loads(run(["docker", "exec", container, "python3", "-c", "import os,json; print(json.dumps(os.listdir('/sys/class/net')))"]))
        assert interfaces == ["lo"], interfaces
        report["interfaces"] = interfaces
        report["runtimeImage"] = run(["docker", "image", "inspect", args.image, "--format", "{{.Id}}"]).strip()
        for shape, source in (("multifile", folder), ("single-file", single)):
            expected_files = {"/fixture/payload/" + str(p.relative_to(payload)): p.stat().st_size
                              for p in (source.rglob("*") if source.is_dir() else [source]) if p.is_file()}
            target = folder / "данные #1/file-8.bin" if source.is_dir() else source
            original = target.read_bytes()
            for fmt in ("v1", "v2", "hybrid"):
                torrent = torrents / f"{shape}-{fmt}.torrent"
                created = json.loads(run([args.proof.resolve(), "create", "--source", source, "--format", fmt,
                                          "--piece-length", "65536", "-o", torrent]))
                run([sys.executable, ROOT / "tools/reference/verify_torrent.py", torrent, "--root", source])
                raw_digest = sha256(torrent)
                for client_name in ("BiglyBT", "qBittorrent"):
                    key = f"{client_name}-{shape}-{fmt}"
                    profile = "/profiles/" + key
                    imported = "/fixture/torrents/" + torrent.name
                    if client_name == "BiglyBT":
                        adapter = ["java", "-Xmx256m", "--add-opens=java.base/java.net=ALL-UNNAMED",
                            "-cp", "/clients/BiglyBT.jar", "/repo/tools/stock-client-proof/BiglyStock.java", profile, imported, "/fixture/payload"]
                    else:
                        adapter = ["python3", "/repo/tools/stock-client-proof/qbittorrent_stock.py", "--app", "/clients/qbt/AppRun",
                            "--profile", profile, "--torrent", imported, "--parent", "/fixture/payload"]
                    case = {"client": client_name, "shape": shape, "format": fmt, "imported": False,
                            "nativeInfohashes": created["result"], "torrentSHA256": raw_digest, "checks": {}}
                    report["cases"].append(case)
                    with (logs / (key + ".log")).open("w", encoding="utf-8") as log:
                        client = Client(["docker", "exec", "-i", container, *adapter], log)
                        try:
                            header = client.receive()
                            assert header["ready"], header
                            for family in ("v1", "v2"):
                                assert header["infohash" + family.upper()] == created["result"].get("infohash_" + family), header
                            actual_files = {f["path"]: f["length"] for f in header["files"] if not f.get("padding", False)}
                            assert actual_files == expected_files, (expected_files, actual_files)
                            case.update(imported=True, importEvidence=header, filesLocated=len(expected_files), referenceVerified=True)
                            for phase in ("healthy", "corrupt", "missing", "restored"):
                                if phase == "corrupt": target.write_bytes(bytes([original[0] ^ 1]) + original[1:])
                                elif phase == "missing": target.unlink()
                                elif phase == "restored": target.write_bytes(original)
                                before = inventory(payload)
                                result = client.check()
                                case["checks"][phase] = result
                                # Pinned BiglyBT includes a leading empty file in
                                # the first v2 piece map. Its checker then hashes
                                # that piece with file length zero. Keep this
                                # exact stock failure visible, never repair it.
                                known_empty = key == "BiglyBT-multifile-v2"
                                healthy = phase in ("healthy", "restored")
                                assert result["complete"] == (healthy and not known_empty), (key, phase, result)
                                if known_empty and healthy:
                                    assert result["completedPermille"] == 909 and result["error"] == "", result
                                    real_files = [f for f in result["files"] if not f["padding"]]
                                    assert all(f["downloaded"] == (0 if f["path"].endswith("/file-1.bin") else f["length"])
                                               for f in real_files), result
                                    case["knownLimitation"] = "BiglyBT 4.1.0.0: first pure-v2 piece fails when its file map starts with an empty file"
                                elif known_empty:
                                    changed = next(f for f in result["files"] if f["path"].endswith("/file-8.bin"))
                                    assert result["error"] or changed["downloaded"] < changed["length"], result
                                assert result["receivedBytes"] == 0 and result["sentBytes"] == 0, result
                                assert inventory(payload) == before, "Stock client changed source inventory or bytes"
                                assert sha256(torrent) == raw_digest, "Stock client changed the original torrent"
                            client.finish()
                            case.update(normalExit=True, clientPayloadUnchanged=True, originalTorrentUnchanged=True)
                        finally:
                            target.write_bytes(original)
                            client.close()
                    print(json.dumps({"client": client_name, "shape": shape, "format": fmt, "regressionChecksPassed": True}), flush=True)
        assert len(report["cases"]) == 12
        complete = all(c["checks"]["healthy"]["complete"] and c["checks"]["restored"]["complete"] for c in report["cases"])
        report.update(passed=complete, regressionChecksPassed=True, allClientsVerified=complete, releaseGate="passed" if complete else "open",
                      realDownloadManagerImports=12, forceRechecks=48,
                      fullyVerifiedCases=sum(c["checks"]["healthy"]["complete"] and c["checks"]["restored"]["complete"] for c in report["cases"]))
    except BaseException as error:
        report["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        try:
            if active:
                run(["docker", "rm", "--force", container])
                report["containerRemoved"] = subprocess.run(["docker", "inspect", container], capture_output=True).returncode != 0
                assert report["containerRemoved"], "Owned stock-client container survived cleanup"
        except BaseException as error:
            report.update(passed=False, releaseGate="open", cleanupError=f"{type(error).__name__}: {error}")
            raise
        finally:
            app_directory.cleanup()
            args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.require_complete and not report["allClientsVerified"]:
        raise SystemExit(5)


if __name__ == "__main__":
    main()
