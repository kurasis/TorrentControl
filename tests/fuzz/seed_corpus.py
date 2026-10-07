"""Seed fuzzing with real native torrents, binary bencode and malformed tagged JSON."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--proof', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
metainfo = args.output / 'metainfo'
tagged = args.output / 'tagged_json'
bridge = args.output / 'bridge_input'
urls = args.output / 'network_url'
metainfo.mkdir(parents=True, exist_ok=True)
tagged.mkdir(parents=True, exist_ok=True)
bridge.mkdir(parents=True, exist_ok=True)
urls.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='tc-fuzz-seeds-') as temporary:
    parent = Path(temporary)
    tree = parent / 'Release'
    for i, length in enumerate([0, 1, 16383, 16384, 16385, 65535, 65536, 65537]):
        file = tree / 'данные' / f'{i}.bin'
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(bytes(n % 251 for n in range(length)))
    single = parent / 'single.bin'
    single.write_bytes(bytes(n % 251 for n in range(65537)))
    for shape, source in [('single', single), ('multi', tree)]:
        for format in ['v1', 'v2', 'hybrid']:
            subprocess.run([str(args.proof.resolve()), 'create', '--source', str(source), '--format', format,
                            '--piece-length', '65536', '-o', str((metainfo / f'{shape}-{format}').resolve())],
                           check=True, capture_output=True, timeout=60)
for name, value in {
    'binary': b'd2:\x00\xff3:a\x00\xffe', 'huge-integer': b'i99999999999999999999999999999999999999999e',
    'unsorted': b'd1:bi2e1:ai1ee', 'duplicate': b'd1:ai1e1:ai2ee',
    'negative-zero': b'i-0e', 'length-overflow': b'18446744073709551616:x',
    'depth-limit': b'l' * 65 + b'0:' + b'e' * 65,
    'unsafe-path': b'd4:infod5:filesld6:lengthi1e4:pathl2:..eee4:name1:x12:piece lengthi16384e6:pieces20:aaaaaaaaaaaaaaaaaaaaee',
}.items():
    (metainfo / name).write_bytes(value)
for name, value in {
    'int': {'t':'int','v':'-999999999999999999999999999999'},
    'bytes': {'t':'bytes','hex':'00ff','len':2},
    'dict': {'t':'dict','entries':[{'key':{'t':'bytes','hex':'00ff','len':2},'value':{'t':'list','items':[{'t':'str','utf8':'привет','len':12}]}}]},
    'bad-length': {'t':'str','utf8':'x','len':-1},
    'bad-truncated-type': {'t':'int','v':'1','truncated':'wrong'},
    'bad-hex': {'t':'bytes','hex':'xx'},
    'duplicate': {'t':'dict','entries':[{'key':{'t':'str','utf8':'a'},'value':{'t':'int','v':'1'}}]*2},
    'elided': {'t':'elided'},
}.items():
    (tagged / name).write_text(json.dumps(value, ensure_ascii=False), encoding='utf-8')
request = {'protocolVersion': 1, 'requestId': 'seed', 'operation': 'probe', 'payload': {}}
for name, changes in {
    'healthy': {}, 'revision-zero': {'draftRevision': '0'},
    'revision-max': {'draftRevision': '18446744073709551615'},
    'revision-overflow': {'draftRevision': '18446744073709551616'},
    'revision-negative': {'draftRevision': '-1'}, 'bad-id': {'requestId': 'a/b'},
    'bad-version': {'protocolVersion': 18446744073709551615},
    'bad-payload': {'payload': []}, 'unknown-operation': {'operation': 'unknown'},
    'unicode': {'payload': {'name': 'файл', 'escaped': '[\\"{}]'}},
}.items():
    (bridge / name).write_text(json.dumps({**request, **changes}, ensure_ascii=False), encoding='utf-8')
for name, depth in [('depth-boundary', 510), ('depth-rejected', 511), ('very-deep', 10000)]:
    (bridge / name).write_text('{"protocolVersion":1,"requestId":"deep","operation":"probe",'
                             '"payload":{"value":' + '[' * depth + '0' + ']' * depth + '}}')
for name, value in {'malformed': b'{"x":', 'numeric-overflow': b'{"x":1e99999}',
                    'invalid-utf8': b'{"x":"\xff"}', 'nul': b'{"x":"\x00"}'}.items():
    (bridge / name).write_bytes(value)
for name, value in {
    'https': b'https://example.invalid/path%20name?token=a%2Fb',
    'auth': b'http://user:synthetic-secret@example.invalid:8080/announce?passkey=synthetic',
    'ipv6': b'http://[::1]:8080/path', 'scoped-ipv6': b'http://[fe80::1%25test]:8080/path',
    'udp': b'udp://example.invalid:6969/announce', 'unsupported': b'ftp://example.invalid/path',
    'bad-port': b'http://example.invalid:65536/', 'fragment': b'http://example.invalid/#fragment',
    'host-suffix': b'http://127.0.0.1.evil.invalid/path',
    'nul': b'http://example.invalid/\x00ignored', 'newline': b'http://example.invalid/\r\nX:yes',
    'backslash': b'http://example.invalid\\@evil.invalid/', 'bad-utf8': b'http://example.invalid/\xff',
    'max-length': b'https://example.invalid/' + b'x' * (8192 - len(b'https://example.invalid/')),
    'over-length': b'https://example.invalid/' + b'x' * 8192,
}.items():
    (urls / name).write_bytes(value)
print(json.dumps({name: len(list(folder.iterdir())) for name, folder in
                  [('metainfoSeeds', metainfo), ('taggedJsonSeeds', tagged),
                   ('bridgeInputSeeds', bridge), ('networkUrlSeeds', urls)]}))
