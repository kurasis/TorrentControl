"""Drive the official GUI release via its loopback Web API, in network=none Docker."""
import argparse
import base64
import hashlib
import http.cookiejar
import json
import os
from pathlib import Path
import socket
import secrets
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--torrent", type=Path, required=True)
    parser.add_argument("--parent", type=Path, required=True)
    args = parser.parse_args()
    args.profile.mkdir(parents=True, exist_ok=False)
    config = args.profile / "qBittorrent/config/qBittorrent.conf"
    config.parent.mkdir(parents=True)
    password = secrets.token_urlsafe(32)
    salt = secrets.token_bytes(16)
    secret = base64.b64encode(salt).decode() + ":" + base64.b64encode(
        hashlib.pbkdf2_hmac("sha512", password.encode(), salt, 100000, 64)).decode()
    config.write_text("""[LegalNotice]
Accepted=true
[BitTorrent]
Session\\DHTEnabled=false
Session\\PeXEnabled=false
Session\\LSDEnabled=false
Session\\AddTorrentStopped=true
[Preferences]
General\\ExitConfirm=false
Connection\\UPnP=false
WebUI\\Enabled=true
WebUI\\Address=127.0.0.1
WebUI\\LocalHostAuth=true
WebUI\\Username=tc-fixture
""" + f'WebUI\\Password_PBKDF2="@ByteArray({secret})"\n', encoding="utf-8")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    # Never inherit host proxy settings for this private loopback API.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
        urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

    def api(operation, fields=None, body=None, content_type=None, raw=False):
        if fields is not None:
            body = urllib.parse.urlencode(fields).encode()
            content_type = "application/x-www-form-urlencoded"
        headers = {"Origin": base, "Referer": base + "/"}
        if content_type:
            headers["Content-Type"] = content_type
        request = urllib.request.Request(base + "/api/v2/" + operation, body, headers)
        with opener.open(request, timeout=10) as response:
            data = response.read().decode("utf-8")
        return data if raw else json.loads(data)

    for name in ("cache", "runtime"):
        (args.profile / name).mkdir(mode=0o700)
    env = {**os.environ, "QT_XCB_GL_INTEGRATION": "none",
           "XDG_CACHE_HOME": str(args.profile / "cache"), "XDG_RUNTIME_DIR": str(args.profile / "runtime")}
    with (args.profile / "client.log").open("w", encoding="utf-8") as log:
        process = subprocess.Popen(["xvfb-run", "-a", "--server-args=-screen 0 1280x800x24 -nolisten tcp",
            str(args.app), "--no-splash", "--confirm-legal-notice", f"--profile={args.profile}",
            f"--webui-port={port}"], stdout=log, stderr=log, env=env)
        ready = False
        try:
            deadline = time.monotonic() + 60
            while True:
                if process.poll() is not None:
                    raise RuntimeError(f"qBittorrent exited during startup: {process.returncode}; see client.log")
                try:
                    # 5.2 acknowledges successful commands with an empty HTTP 200.
                    # The protected version request also verifies the login cookie.
                    api("auth/login", {"username": "tc-fixture", "password": password}, raw=True)
                    version = api("app/version", raw=True)
                    assert version == "v5.2.4", version
                    ready = True
                    break
                except urllib.error.HTTPError as error:
                    raise RuntimeError(f"qBittorrent startup API returned HTTP {error.code}") from error
                except (urllib.error.URLError, ConnectionError, TimeoutError):
                    if time.monotonic() > deadline:
                        raise TimeoutError("qBittorrent Web API startup deadline exceeded")
                    time.sleep(0.05)
            api("app/setPreferences", {"json": json.dumps({"dht": False, "pex": False, "lsd": False,
                "upnp": False, "queueing_enabled": False, "auto_tmm_enabled": False})}, raw=True)
            boundary = "tc-stock-import-boundary"
            fields = {"savepath": str(args.parent), "stopped": "true", "skip_checking": "false",
                      "autoTMM": "false", "contentLayout": "Original"}
            parts = [f'--{boundary}\r\nContent-Disposition: form-data; name="{key}"\r\n\r\n{value}\r\n'.encode()
                     for key, value in fields.items()]
            parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="torrents"; filename="input.torrent"\r\nContent-Type: application/x-bittorrent\r\n\r\n'.encode()
                         + args.torrent.read_bytes() + b"\r\n")
            parts.append(f"--{boundary}--\r\n".encode())
            api("torrents/add", body=b"".join(parts), content_type=f"multipart/form-data; boundary={boundary}", raw=True)
            deadline = time.monotonic() + 30
            while True:
                torrents = api("torrents/info")
                if len(torrents) == 1 and not torrents[0]["state"].startswith("checking"):
                    torrent = torrents[0]
                    break
                if time.monotonic() > deadline:
                    raise TimeoutError("qBittorrent import deadline exceeded")
                time.sleep(0.02)
            key = torrent["hash"]
            files = api("torrents/files?" + urllib.parse.urlencode({"hash": key}))
            print(json.dumps({"ready": True, "client": version, "build": api("app/buildInfo"),
                "infohashV1": torrent.get("infohash_v1") or None, "infohashV2": torrent.get("infohash_v2") or None,
                "files": [{"path": str(args.parent / f["name"]), "length": f["size"], "padding": False} for f in files]}), flush=True)
            for line in sys.stdin:
                command = line.strip()
                if command == "quit":
                    break
                if command != "check":
                    raise ValueError("Unknown command")
                api("torrents/recheck", {"hashes": key}, raw=True)
                began, checking_seen = time.monotonic(), False
                while True:
                    value = api("torrents/info?" + urllib.parse.urlencode({"hashes": key}))[0]
                    checking = value["state"].startswith("checking")
                    checking_seen |= checking
                    # The GUI's libtorrent status cache refreshes asynchronously
                    # after the checked/stopped alerts. A stopped state alone can
                    # still carry the previous progress during that interval.
                    if not checking and time.monotonic() - began >= 3:
                        break
                    if time.monotonic() - began > 120:
                        raise TimeoutError("qBittorrent force recheck deadline exceeded")
                    time.sleep(0.02)
                print(json.dumps({"complete": value["progress"] == 1 and value["amount_left"] == 0,
                    "progress": value["progress"], "bytesMissing": value["amount_left"], "state": value["state"],
                    "receivedBytes": value["downloaded"], "sentBytes": value["uploaded"], "checkingObserved": checking_seen}), flush=True)
        finally:
            if ready and process.poll() is None:
                api("app/shutdown", {}, raw=True)
                if process.wait(timeout=20) != 0:
                    raise RuntimeError("qBittorrent normal shutdown failed")
            elif process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
        print(json.dumps({"normalExit": True}), flush=True)


if __name__ == "__main__":
    main()
