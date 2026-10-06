"""Task-owned Linux kernel/Samba peer, with a private TAP subnet on Windows.

QEMU TCG needs neither Hyper-V nor nested virtualization. Control uses an
independent serial console, so stopping SMB never disables fixture recovery.
The local control mode validates guest startup only; it is not Windows proof.
"""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import secrets
import socket
import subprocess
import threading
import time
import uuid
import urllib.request


class PackageCache:
    """Host fetches fixed official HTTPS assets; guest verifies APK signatures.

    The VM's local transport does not depend on cloud egress/proxy settings.
    There is no general URL proxy or untrusted-package option.
    """
    def __init__(self, folder):
        self.folder = folder
        folder.mkdir(exist_ok=True)
        owner = self
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass
            def do_GET(self):
                match = re.fullmatch(r'/x86_64/(APKINDEX\.tar\.gz|[A-Za-z0-9][A-Za-z0-9_.+-]*\.apk)', self.path)
                if not match:
                    self.send_error(404)
                    return
                name = match.group(1)
                target = owner.folder / name
                try:
                    if not target.exists():
                        url = 'https://dl-cdn.alpinelinux.org/alpine/v3.22/main/x86_64/' + name
                        with urllib.request.urlopen(url, timeout=60) as source:
                            content = source.read(64 * 1024 * 1024 + 1)
                        if len(content) > 64 * 1024 * 1024:
                            raise RuntimeError('APK asset exceeds the fixture download limit')
                        temporary = target.with_suffix(target.suffix + '.download')
                        temporary.write_bytes(content)
                        temporary.replace(target)
                    data = target.read_bytes()
                except Exception:
                    self.send_error(502, 'Official HTTPS APK asset could not be fetched')
                    return
                self.send_response(200)
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def provenance(self):
        return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in self.folder.iterdir() if p.is_file() and not p.name.endswith('.download')}

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)


class Console:
    def __init__(self, connection, log):
        self.connection = connection
        self.log = log.open('w', encoding='utf-8')
        self.condition = threading.Condition()
        self.buffer = ''
        self.closed = False
        self.redactions = []
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def clean(self, text):
        for secret in self.redactions:
            text = text.replace(secret, '[fixture credential redacted]')
        return text

    def _read(self):
        pending = ''
        try:
            while data := self.connection.recv(16384):
                text = data.decode('utf-8', errors='replace').replace('\r', '')
                with self.condition:
                    self.buffer += text
                    self.condition.notify_all()
                # Redact complete lines, including secrets split across reads.
                pending += text
                lines = pending.split('\n')
                pending = lines.pop()
                for line in lines:
                    self.log.write(self.clean(line) + '\n')
                self.log.flush()
        except OSError:
            pass
        finally:
            self.log.write(self.clean(pending))
            self.log.flush()
            with self.condition:
                self.closed = True
                self.condition.notify_all()

    def send(self, text):
        self.connection.sendall(text.encode('utf-8'))

    def expect(self, pattern, start=0, timeout=90):
        deadline = time.monotonic() + timeout
        with self.condition:
            while True:
                match = re.search(pattern, self.buffer[start:])
                if match:
                    return match, start + match.end()
                if self.closed or time.monotonic() >= deadline:
                    raise TimeoutError('Guest console did not reach ' + pattern + ': ' + self.clean(self.buffer[-2000:]))
                self.condition.wait(min(0.1, max(0, deadline - time.monotonic())))

    def command(self, command, timeout=90):
        marker = uuid.uuid4().hex
        with self.condition:
            start = len(self.buffer)
        self.send(f"printf '\\nTC_BEGIN_{marker}\\n'\n{command}\ntc_result=$?; printf '\\nTC_END_{marker}:%s\\n' \"$tc_result\"\n")
        _, begin = self.expect(r'\nTC_BEGIN_' + marker + r'\n', start, timeout)
        match, _ = self.expect(r'\nTC_END_' + marker + r':([0-9]+)\n', begin, timeout)
        with self.condition:
            output = self.buffer[begin:begin + match.start()]
        if int(match.group(1)):
            raise RuntimeError('Guest command failed: ' + self.clean(output[-2000:]))
        return self.clean(output.strip())

    def close(self):
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.connection.close()
        self.reader.join(timeout=5)
        self.log.close()


class SmbVm:
    def __init__(self, config, folder, local=False):
        self.config, self.folder, self.local = config, folder, local
        folder.mkdir(parents=True, exist_ok=True)
        self.process = self.console = self.error = self.packages = None
        self.username = 'tcfixture'
        self.password = secrets.token_hex(32)
        self.server_ip = '10.0.2.15' if local else config['serverIp']
        self.share = '\\\\' + self.server_ip + '\\payload'
        self.connected = False
        self.server_running = False
        self.metadata = {}

    def start(self):
        self.packages = PackageCache(self.folder / 'packages')
        with socket.socket() as allocation:
            allocation.bind(('127.0.0.1', 0))
            port = allocation.getsockname()[1]
        arguments = [self.config['qemu'], '-accel', 'tcg,thread=multi', '-smp', '2', '-m', '768',
                     '-display', 'none', '-monitor', 'none', '-no-reboot',
                     '-kernel', self.config['kernel'], '-initrd', self.config['initrd'],
                     '-cdrom', self.config['iso'], '-append', 'console=ttyS0 modules=loop,squashfs,sd-mod quiet',
                     '-serial', f'tcp:127.0.0.1:{port},server=on,wait=on',
                     '-netdev', 'user,id=internet', '-device', 'virtio-net-pci,netdev=internet']
        if not self.local:
            arguments += ['-netdev', 'tap,id=private,ifname=' + self.config['adapterName'],
                          '-device', 'virtio-net-pci,netdev=private,mac=52:54:00:54:43:02']
        self.error = (self.folder / 'qemu-stderr.log').open('w', encoding='utf-8')
        self.process = subprocess.Popen(arguments, stdin=subprocess.DEVNULL, stdout=self.error, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 30
        while True:
            try:
                connection = socket.create_connection(('127.0.0.1', port), timeout=1)
                connection.settimeout(None)
                break
            except OSError:
                if self.process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('QEMU serial listener failed: ' + (self.folder / 'qemu-stderr.log').read_text())
                time.sleep(0.1)
        self.console = Console(connection, self.folder / 'guest-console.log')
        self.console.redactions.append(self.password)
        _, end = self.console.expect(r'login: ', timeout=180)
        self.console.send('root\n')
        self.console.expect(r'localhost:.*# ', end, timeout=30)
        self.console.command("stty -echo; unset HISTFILE; PS1=''; PS2=''")
        self.console.command('ip link set eth0 up && udhcpc -i eth0 -q -n -t 5', timeout=60)
        repository = f'http://10.0.2.2:{self.packages.server.server_port}'
        self.console.command(f"printf '%s\\n' '{repository}' > /etc/apk/repositories")
        self.console.command('apk add --no-cache samba-server samba-common-tools samba-client', timeout=240)
        if not self.local:
            self.console.command(f'ip link set eth1 up && ip addr add {self.server_ip}/30 dev eth1')
            links = self.console.command('ip -o link show eth1')
            assert '52:54:00:54:43:02' in links, 'The private adapter must be the second QEMU NIC'
        self.console.command('mkdir -p /srv/tc-payload /tmp/tc-smb/state /tmp/tc-smb/lock /tmp/tc-smb/private /tmp/tc-smb/cache /tmp/tc-smb/pid /tmp/tc-smb/ncalrpc')
        self.console.command('dd if=/dev/zero of=/srv/tc-payload/payload.bin bs=1048576 count=64 && chmod 644 /srv/tc-payload/payload.bin')
        self.console.command('adduser -D -H tcfixture')
        # The console has echo disabled. Its log writer also redacts this
        # ephemeral fixture credential; no user or platform secret is used.
        config = f'''[global]
server role = standalone server
netbios name = TC-SMB-VM
workgroup = TC-FIXTURE
security = user
map to guest = Never
interfaces = {self.server_ip}
bind interfaces only = yes
smb ports = 445
server min protocol = SMB3_00
server max protocol = SMB3_11
disable netbios = yes
load printers = no
printing = bsd
printcap name = /dev/null
smb2 leases = no
state directory = /tmp/tc-smb/state
lock directory = /tmp/tc-smb/lock
private dir = /tmp/tc-smb/private
cache directory = /tmp/tc-smb/cache
pid directory = /tmp/tc-smb/pid
ncalrpc dir = /tmp/tc-smb/ncalrpc
log file = /tmp/tc-smb/samba.log
log level = 2
[payload]
path = /srv/tc-payload
valid users = tcfixture
read only = yes
oplocks = no
level2 oplocks = no
durable handles = no
'''
        self.console.command("cat > /tmp/tc-smb/smb.conf <<'TC_CONFIG'\n" + config + '\nTC_CONFIG\n')
        self.console.command(f"printf '%s\\n%s\\n' '{self.password}' '{self.password}' | smbpasswd -c /tmp/tc-smb/smb.conf -a -s tcfixture")
        self.start_server()
        self.console.command("umask 077; cat > /tmp/tc-smb/client-auth <<'TC_AUTH'\nusername = tcfixture\npassword = " + self.password + '\nTC_AUTH\n')
        self.console.command(f"smbclient -p 445 -t 30 '//{self.server_ip}/payload' -A /tmp/tc-smb/client-auth -c 'get payload.bin /tmp/tc-smb/client-proof.bin'", timeout=90)
        hashes = self.console.command('sha256sum /srv/tc-payload/payload.bin /tmp/tc-smb/client-proof.bin').splitlines()
        assert len(hashes) == 2 and hashes[0].split()[0] == hashes[1].split()[0]
        self.console.command('test "$(wc -c < /tmp/tc-smb/client-proof.bin)" = 67108864 && rm /tmp/tc-smb/client-proof.bin')
        self.metadata = {'kernel': self.console.command('uname -a'),
                         'samba': self.console.command('smbd --version'),
                         'packages': self.console.command("apk info -v | grep -E '^samba-(server|common-tools|client)-'"),
                         'serverIp': self.server_ip, 'privatePeer': not self.local,
                         'payloadSha256': self.console.command('sha256sum /srv/tc-payload/payload.bin').split()[0],
                         'guestSmbPayloadReadVerified': True, 'apkSignaturesEnforced': True,
                         'apkAssetSha256': self.packages.provenance()}
        if not self.local:
            self.connect()

    def start_server(self):
        self.console.command('smbd -D -s /tmp/tc-smb/smb.conf')
        self.console.command('test -s /tmp/tc-smb/pid/smbd.pid && kill -0 "$(cat /tmp/tc-smb/pid/smbd.pid)"')
        self.server_running = True
        # A daemon PID precedes socket readiness. Without this check an early
        # client can fail port 445 and spend its timeout on a fallback port.
        self.console.command('tc_wait=0; until netstat -lnt | grep -q "' + self.server_ip + ':445 "; do tc_wait=$((tc_wait+1)); test "$tc_wait" -lt 30 || break; sleep 1; done; test "$tc_wait" -lt 30', timeout=35)

    def suspend(self):
        self.console.command('kill -STOP -"$(cat /tmp/tc-smb/pid/smbd.pid)"')

    def resume(self):
        if self.server_running:
            self.console.command('kill -CONT -"$(cat /tmp/tc-smb/pid/smbd.pid)"')

    def restore(self):
        if self.server_running:
            self.resume()
        else:
            self.start_server()

    def disconnect(self):
        self.console.command('kill -KILL -"$(cat /tmp/tc-smb/pid/smbd.pid)" && rm -f /tmp/tc-smb/pid/smbd.pid')
        self.server_running = False

    def connect(self):
        import ctypes
        from ctypes import wintypes
        class NetResource(ctypes.Structure):
            _fields_ = [('scope', wintypes.DWORD), ('type', wintypes.DWORD), ('displayType', wintypes.DWORD),
                        ('usage', wintypes.DWORD), ('localName', wintypes.LPWSTR), ('remoteName', wintypes.LPWSTR),
                        ('comment', wintypes.LPWSTR), ('provider', wintypes.LPWSTR)]
        resource = NetResource(type=1, remoteName=self.share)
        add = ctypes.WinDLL('mpr').WNetAddConnection2W
        add.argtypes = [ctypes.POINTER(NetResource), wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD]
        add.restype = wintypes.DWORD
        result = add(ctypes.byref(resource), self.password, 'TC-SMB-VM\\' + self.username, 4)  # CONNECT_TEMPORARY
        if result:
            raise OSError(result, 'Cannot establish the task-owned authenticated SMB connection')
        self.connected = True

    def wait_recovery(self, timeout=90):
        started = time.monotonic()
        while True:
            try:
                source = Path(self.share) / 'payload.bin'
                assert source.stat().st_size == 64 * 1024 * 1024
                with source.open('rb') as stream:
                    assert stream.read(1) == b'\x00'
                return (time.monotonic() - started) * 1000
            except OSError as error:
                if time.monotonic() - started > timeout:
                    raise TimeoutError('Windows SMB connection did not recover') from error
                time.sleep(0.1)

    def close(self):
        errors = []
        if self.console and not self.console.closed:
            try:
                self.resume()
                self.console.command('test ! -f /tmp/tc-smb/samba.log || tail -n 200 /tmp/tc-smb/samba.log', timeout=10)
            except Exception as error:
                errors.append(str(error))
        if self.connected:
            import ctypes
            from ctypes import wintypes
            cancel = ctypes.WinDLL('mpr').WNetCancelConnection2W
            cancel.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.BOOL]
            cancel.restype = wintypes.DWORD
            result = cancel(self.share, 0, True)
            if result:
                errors.append(f'Cannot remove the owned SMB connection: {result}')
            self.connected = False
        if self.process:
            self.process.terminate()
            self.process.wait(timeout=15)
        if self.console:
            self.console.close()
        if self.error:
            self.error.close()
        if self.packages:
            self.packages.close()
        if errors:
            raise RuntimeError('; '.join(errors))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Local guest-control validation, without a Windows SMB claim')
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--folder', type=Path, required=True)
    args = parser.parse_args()
    vm = SmbVm(json.loads(args.config.read_text()), args.folder, local=True)
    try:
        vm.start()
        vm.suspend()
        vm.resume()
        vm.disconnect()
        vm.start_server()
        print(json.dumps(vm.metadata), flush=True)
    finally:
        vm.close()
