"""Loopback-only WebView2 diagnostics fixture; no public tracker traffic."""
import argparse
import http.server
from pathlib import Path
import urllib.parse

parser = argparse.ArgumentParser()
parser.add_argument("--port-file", type=Path, required=True)
parser.add_argument("--root", type=Path, required=True)
args = parser.parse_args()


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        headers = {}
        if url.path == "/scrape":
            params = urllib.parse.parse_qs(url.query, encoding="latin1")
            raw_hash = params.get("info_hash", [""])[0].encode("latin1")
            if len(raw_hash) != 20 or "peer_id" in params:
                self.send_error(400)
                return
            body = b"d5:filesd20:" + raw_hash + b"d8:completei0e10:downloadedi0e10:incompletei0eeee"
            status = 200
        elif url.path.startswith("/seed/"):
            relative = Path(urllib.parse.unquote(url.path[len("/seed/"):]))
            root = (args.root / "payload").resolve()
            file = (root / relative).resolve()
            if not file.is_relative_to(root) or not file.is_file() or self.headers.get("Range") != "bytes=0-0":
                self.send_error(400)
                return
            with file.open("rb") as stream:
                body = stream.read(1)
            status = 206
            headers["Content-Range"] = f"bytes 0-0/{file.stat().st_size}"
        else:
            self.send_error(404)
            return
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        for key, value in headers.items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
# The PowerShell launcher treats the published path as the readiness signal.
# Publish it only after the complete port value is written and closed.
pending_port_file = args.port_file.with_suffix(".tmp")
pending_port_file.write_text(str(server.server_port), encoding="ascii")
pending_port_file.replace(args.port_file)
server.serve_forever()
