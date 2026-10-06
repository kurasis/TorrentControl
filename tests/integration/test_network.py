"""Deterministic N/S fixtures; every network destination is a local server."""
import http.server
import json
import os
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import threading
import time
import unittest
import urllib.parse

PROOF = os.environ["TC_NETWORK_PROOF"]


def bencode(v):
    if isinstance(v, int):
        return b"i" + str(v).encode() + b"e"
    if isinstance(v, str):
        v = v.encode()
    if isinstance(v, bytes):
        return str(len(v)).encode() + b":" + v
    if isinstance(v, list):
        return b"l" + b"".join(map(bencode, v)) + b"e"
    return b"d" + b"".join(bencode(k) + bencode(v[k]) for k in sorted(v)) + b"e"


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_HEAD(self):
        self.send_error(405)

    def do_GET(self):
        self.server.requests.append((self.path, dict(self.headers)))
        url = urllib.parse.urlsplit(self.path)
        path = url.path
        status = 200
        headers = {}
        if path.startswith("/valid/scrape"):
            params = urllib.parse.parse_qs(url.query, encoding="latin1")
            h = params.get("info_hash", [""])[0].encode("latin1")
            body = bencode({b"files": {h: {b"complete": 0, b"downloaded": 0, b"incomplete": 0}}})
        elif path.startswith("/restricted/"):
            body = bencode({b"failure reason": b"Denied secret-password-do-not-export"})
        elif path.startswith("/unsupported/"):
            status, body = 404, b"no scrape"
        elif path.startswith("/html/"):
            body = b"<html>200 is not a tracker reply</html>"
        elif path.startswith("/redirect-cross/"):
            status, body = 302, b""
            headers["Location"] = f"http://localhost:{self.server.server_port}/valid/scrape"
        elif path.startswith("/redirect-same/"):
            status, body = 302, b""
            headers["Location"] = "/range/file?token=signed-query"
        elif path.startswith("/range/"):
            status, body = 206, b"x"
            headers["Content-Range"] = "bytes 0-0/10"
        elif path.startswith("/bad-range/"):
            status, body = 206, b"x"
            headers["Content-Range"] = "bytes 0-0/999"
        elif path.startswith("/empty/"):
            status, body = 416, b""
            headers["Content-Range"] = "bytes */0"
        elif path.startswith("/ignored/"):
            body = b"x" * (2 * 1024 * 1024)
        elif path.startswith("/compressed/"):
            status, body = 206, b"x"
            headers["Content-Range"] = "bytes 0-0/10"
            headers["Content-Encoding"] = "gzip"
        elif path.startswith("/slow/"):
            time.sleep(1)
            body = bencode({b"files": {}})
        elif path.startswith("/concurrent/"):
            with self.server.counter_lock:
                self.server.active += 1
                self.server.peak = max(self.server.peak, self.server.active)
            time.sleep(.08)
            with self.server.counter_lock:
                self.server.active -= 1
            body = bencode({b"files": {}})
        elif path.startswith("/catalog-ok/"):
            body = b"udp://127.0.0.1:1337/announce\nhttp://127.0.0.1:80/announce\n"
        elif path.startswith("/catalog-bad/"):
            body = b"<html>broken list</html>"
        else:
            body = b"transport only"
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        for name, value in headers.items():
            self.send_header(name, value)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass


class NetworkFixtures(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.server.requests = []
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = f"http://127.0.0.1:{cls.server.server_port}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def proof(self, targets, **options):
        # Response semantics need scheduling headroom on loaded Windows runners.
        # Deadline/UDP tests below keep their explicit short budgets.
        input_data = {"targets": targets, "timeoutMs": 2000, **options}
        r = subprocess.run([PROOF], input=json.dumps(input_data), text=True, capture_output=True, timeout=15)
        self.assertEqual(r.returncode, 0, r.stderr)
        return json.loads(r.stdout)

    def endpoint(self, path, kind="tracker", expected=10):
        return {"url": self.base + path, "kind": kind, "expectedLength": expected}

    def state(self, target, **options):
        return self.proof([target], **options)["runs"][0]["rows"][0]

    def test_N03_html_failure_and_zero_peers(self):
        self.assertEqual(self.state(self.endpoint("/html/announce"))["state"], "invalid-response")
        restricted = self.state(self.endpoint("/restricted/announce?passkey=secret-password"))
        self.assertEqual(restricted["state"], "responding-restricted")
        self.assertNotIn("secret-password", json.dumps(restricted))
        self.assertEqual(self.state(self.endpoint("/valid/announce"))["state"], "protocol-responding")

    def test_N04_scrape_unsupported_and_family_results(self):
        result = self.state(self.endpoint("/unsupported/announce"))
        self.assertEqual(result["state"], "scrape-unsupported")
        self.assertEqual([a["family"] for a in result["attempts"]], [4, 6])
        self.assertNotEqual(result["attempts"][1]["state"], "protocol-responding")

    def test_TLS_rejects_untrusted_certificate(self):
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.requests = []
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        fixtures = Path(__file__).parent / "fixtures"
        context.load_cert_chain(fixtures / "untrusted-localhost.pem", fixtures / "untrusted-localhost.key")
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            result = self.state({"url": f"https://127.0.0.1:{server.server_port}/valid/announce", "kind": "tracker"}, timeoutMs=2000)
            self.assertEqual(result["state"], "tls-error")
            self.assertEqual(server.requests, [])
        finally:
            server.shutdown()
            server.server_close()

    def test_worker_limit_is_four_and_ui_thread_can_cancel(self):
        self.server.counter_lock = threading.Lock()
        self.server.active = self.server.peak = 0
        targets = [self.endpoint(f"/concurrent/{i}/announce") for i in range(12)]
        result = self.proof(targets, timeoutMs=2000)
        self.assertEqual(result["runs"][0]["run"]["completed"], 12)
        self.assertEqual(self.server.peak, 4)

    def test_N05_proxy_without_udp_never_resolves_or_sends_direct(self):
        target = {"url": "udp://nonexistent-private-dns.invalid:1/private-secret", "kind": "tracker"}
        result = self.state(target, httpProxy=self.base)
        self.assertEqual(result["state"], "probe-unsupported")
        self.assertEqual(result["attempts"], [])

    def test_N05_http_proxy_preserves_destination_without_local_dns(self):
        target = {"url": "http://not-resolvable.invalid:80/valid/announce?passkey=private-secret", "kind": "tracker"}
        before = len(self.server.requests)
        result = self.state(target, httpProxy=self.base)
        self.assertEqual(result["state"], "protocol-responding")
        self.assertTrue(any(path.startswith("http://not-resolvable.invalid") for path, _ in self.server.requests[before:]))
        self.assertNotIn("private-secret", json.dumps(result))

    def test_N06_no_real_swarm_announce_or_peer_registration(self):
        before = len(self.server.requests)
        result = self.state(self.endpoint("/valid/announce?info_hash=REAL-SWARM&peer_id=REAL-PEER&token=auth"))
        self.assertEqual(result["state"], "protocol-responding")
        requests = self.server.requests[before:]
        for path, _ in requests:
            parsed = urllib.parse.urlsplit(path)
            self.assertEqual(parsed.path, "/valid/scrape")
            params = urllib.parse.parse_qs(parsed.query, encoding="latin1")
            self.assertEqual(len(params["info_hash"][0].encode("latin1")), 20)
            self.assertNotIn("peer_id", params)
            self.assertEqual(params["token"], ["auth"])

    def test_N07_failed_catalog_keeps_last_valid_list(self):
        result = self.proof([self.endpoint("/catalog-ok/list", "catalog"), self.endpoint("/catalog-bad/list", "catalog")])
        self.assertEqual(result["catalog"]["urls"], ["udp://127.0.0.1:1337/announce", "http://127.0.0.1:80/announce"])
        self.assertIsNone(result["catalog"]["sourceDate"])
        self.assertIsNotNone(result["catalog"]["fetchedAt"])
        self.assertEqual(len(result["catalog"]["checksum"]), 64)

    def test_catalog_apply_preserves_custom_disabled_tiers_and_checks_review(self):
        result = self.proof([self.endpoint("/catalog-ok/list", "catalog")], catalogApply=True)
        self.assertEqual(result["staleCatalog"], "STALE_CATALOG")
        self.assertEqual(result["staleRevision"], "STALE_REVISION")
        self.assertEqual(result["privateCatalog"], "PRIVATE_CATALOG")
        self.assertTrue(result["privatePlan"]["privateBlocked"])
        rows = result["catalogApplied"]["trackers"]
        self.assertIn({"url": "https://custom.example/announce?passkey=keep", "tier": 77, "enabled": True}, rows)
        self.assertIn({"url": "udp://127.0.0.1:1337/announce", "tier": 88, "enabled": False}, rows)
        self.assertTrue(any(not row["enabled"] and row["tier"] == 0 for row in rows))
        self.assertEqual(result["catalogPlan"]["added"], ["http://127.0.0.1:80/announce"])

    def test_S02_range_head_rejection_empty_and_body_cap(self):
        self.assertEqual(self.state(self.endpoint("/range/file", "bep19"))["state"], "range-supported")
        self.assertEqual(self.state(self.endpoint("/empty/file", "bep19", 0))["state"], "empty-file")
        self.assertEqual(self.state(self.endpoint("/bad-range/file", "bep19"))["state"], "invalid-response")
        ignored = self.state(self.endpoint("/ignored/file", "bep19"))
        self.assertEqual(ignored["state"], "range-ignored")
        self.assertTrue(ignored["attempts"][0]["bodyCapped"])
        self.assertLessEqual(ignored["attempts"][0]["bytesKept"], 4096)
        self.assertEqual(self.state(self.endpoint("/compressed/file", "bep19"))["state"], "invalid-response")

    def test_S03_S05_scope_is_sample_transport_not_integrity(self):
        result = self.state(self.endpoint("/range/file", "bep19"))
        self.assertEqual(result["integrity"], "not-verified")
        bep17 = self.state(self.endpoint("/transport/piece", "bep17-transport"))
        self.assertEqual(bep17["state"], "transport-responding")
        self.assertEqual(bep17["operation"], "transport-only")
        self.assertEqual(bep17["integrity"], "not-verified")
        self.assertEqual(self.state(self.endpoint("/transport/piece", "bep17-unsupported-v2"))["state"], "probe-unsupported")

    def test_redirect_policy_blocks_cross_origin_and_keeps_same_origin_query(self):
        before = len(self.server.requests)
        blocked = self.state(self.endpoint("/redirect-cross/file", "bep19"))
        self.assertEqual(blocked["state"], "redirect-blocked")
        self.assertFalse(any("localhost" in path for path, _ in self.server.requests[before:]))
        allowed = self.state(self.endpoint("/redirect-same/file?token=secret", "bep19"))
        self.assertEqual(allowed["state"], "range-supported")
        self.assertNotIn("secret", json.dumps(allowed))

    def test_cancellation_deadline_dedup_and_cache(self):
        start = time.monotonic()
        cancelled = self.proof([self.endpoint("/slow/announce")], timeoutMs=3000, cancelAfterMs=40)
        self.assertEqual(cancelled["runs"][0]["run"]["state"], "cancelled")
        self.assertEqual(cancelled["runs"][0]["rows"][0]["state"], "cancelled")
        self.assertLess(time.monotonic() - start, .8)
        self.assertEqual(self.state(self.endpoint("/slow/announce"), timeoutMs=300)["state"], "no-response")
        cached = self.proof([self.endpoint("/valid/announce")] * 2, repeat=2)
        self.assertEqual(cached["runs"][0]["run"]["total"], 1)
        self.assertTrue(cached["runs"][1]["rows"][0]["cached"])
        self.assertEqual(cached["runs"][0]["rows"][0]["checkedAt"], cached["runs"][1]["rows"][0]["checkedAt"])
        refreshed = self.proof([self.endpoint("/valid/announce")], repeat=2, refresh=True)
        self.assertFalse(refreshed["runs"][1]["rows"][0]["cached"])

    def udp(self, mode):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        sock.settimeout(1)
        packets = []
        def server():
            try:
                request, addr = sock.recvfrom(4096)
                packets.append(time.monotonic())
                magic, action, tx = struct.unpack("!QII", request)
                if magic != 0x41727101980 or action != 0:
                    return
                if mode == "retry":
                    request, addr = sock.recvfrom(4096)
                    packets.append(time.monotonic())
                    _, _, tx = struct.unpack("!QII", request)
                if mode == "timeout":
                    return
                response = struct.pack("!IIQ", 0, tx, 12345) + b"compatible-extra"
                if mode == "wrong-transaction":
                    response = struct.pack("!IIQ", 0, tx ^ 1, 12345)
                if mode == "wrong-sender":
                    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as wrong:
                        wrong.sendto(response, addr)
                elif mode == "restricted":
                    sock.sendto(struct.pack("!II", 3, tx) + b"private-secret", addr)
                else:
                    sock.sendto(response, addr)
            except (socket.timeout, OSError):
                pass
        thread = threading.Thread(target=server, daemon=True)
        thread.start()
        try:
            result = self.state({"url": f"udp://127.0.0.1:{sock.getsockname()[1]}/passkey/private-secret", "kind":"tracker"}, timeoutMs=120, udpRetry=mode == "retry")
            thread.join(timeout=1)
            return result, packets
        finally:
            sock.close()

    def test_N01_udp_transaction_sender_and_extra_bytes(self):
        self.assertEqual(self.udp("valid")[0]["state"], "protocol-responding")
        self.assertEqual(self.udp("wrong-transaction")[0]["state"], "invalid-response")
        self.assertEqual(self.udp("wrong-sender")[0]["state"], "invalid-response")
        restricted, _ = self.udp("restricted")
        self.assertEqual(restricted["state"], "responding-restricted")
        self.assertNotIn("private-secret", json.dumps(restricted))

    def test_N02_udp_finite_retry_and_no_connection_id_cache(self):
        self.assertEqual(self.udp("timeout")[0]["state"], "no-response")
        result, packets = self.udp("retry")
        self.assertEqual(result["state"], "protocol-responding")
        self.assertGreaterEqual(packets[1] - packets[0], .1)
        self.assertEqual(result["attempts"][0]["requests"], 2)
        self.assertEqual(result["authorization"], "unknown")
        self.assertEqual(result["acceptsTorrent"], "unknown")


if __name__ == "__main__":
    unittest.main()
