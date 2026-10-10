#!/usr/bin/env python3
"""Cancellation, revision-aware cursor reuse and binary cache identity."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile

from protocol_features import request, send
from scheduler import response, stop


def main():
    binary = Path(sys.argv[1]).resolve()
    version = subprocess.check_output([str(binary), '--version'], text=True, timeout=5).strip()
    assert version.startswith('encore-lsp '), version
    with tempfile.TemporaryDirectory(prefix='encore-lsp-lifecycle-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text(
            '[project]\nname="lifecycle"\nversion="0.0.0"\ndependencies=[]\n')
        source = 'fn main() -> u32 { let value = 1_u32 ret value }\n'
        path = root / 'src/main.enq'
        path.write_text(source)
        cache = root / 'cache'
        process = subprocess.Popen([str(binary)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            request(process, 1, 'initialize', {'rootUri': root.as_uri(),
                    'initializationOptions': {'jobs': 1, 'debounceMs': 400, 'cacheDir': str(cache)}})
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 1, 'text': source}}})
            send(process, {'jsonrpc': '2.0', 'id': 'cancel-me', 'method': 'textDocument/diagnostic',
                           'params': {'textDocument': {'uri': path.as_uri()}}})
            send(process, {'jsonrpc': '2.0', 'method': '$/cancelRequest', 'params': {'id': 'cancel-me'}})
            cancelled = response(process, 'cancel-me')
            assert cancelled['error']['code'] == -32800, cancelled
            # Unknown cancellations do not create unsolicited responses.
            send(process, {'jsonrpc': '2.0', 'method': '$/cancelRequest', 'params': {'id': 999}})
            observed = []
            send(process, {'jsonrpc': '2.0', 'id': 2, 'method': 'textDocument/diagnostic',
                           'params': {'textDocument': {'uri': path.as_uri()}}})
            assert 'result' in response(process, 2, observed)
            assert not any(item.get('id') in ('cancel-me', 999) for item in observed), observed
            params = {'textDocument': {'uri': path.as_uri()},
                      'position': {'line': 0, 'character': source.rindex('value')}}
            baseline = request(process, 3, 'textDocument/hover', params)
            for number in range(4, 24):
                assert request(process, number, 'textDocument/hover', params) == baseline
            assert 'u32' in str(baseline), baseline
            changed = source.replace('1_u32', '1_u64').replace('-> u32', '-> u64')
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 2},
                'contentChanges': [{'text': changed}]}})
            request(process, 24, 'textDocument/diagnostic', {'textDocument': {'uri': path.as_uri()}})
            updated = request(process, 25, 'textDocument/hover', params)
            assert 'u64' in str(updated) and updated != baseline, updated
            for entry in cache.glob('*.json'):
                data = json.loads(entry.read_text())
                assert len(data['binary']) == 64, data
                assert data['schema'] == 'encore-lsp-analysis-v3', data
            assert list(cache.glob('*.json')), 'missing disk cache'
            # Many independent malformed JSON frames must not accumulate stack frames.
            for _ in range(100):
                process.stdin.write(b'Content-Length: 1\r\n\r\n{')
            process.stdin.flush()
            request(process, 26, 'textDocument/hover', params)
            stop(process)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print('request cancellation, cursor invalidation, version and build-aware cache: ok')


if __name__ == '__main__':
    main()
