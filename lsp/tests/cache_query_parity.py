#!/usr/bin/env python3
"""Disk-cache hits must not change diagnostics or semantic editor answers."""

from pathlib import Path
import sys
import tempfile

from protocol_features import send
from scheduler import response, start, stop


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='encore-lsp-cache-parity-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text(
            '[project]\nname="parity"\nversion="0.0.0"\ndependencies=[]\n')
        path = root / 'src/main.enq'
        source = 'fn main() -> u32 { let mut value = 1_u32 ret value }\n'
        path.write_text(source)
        answers = []
        for _ in range(2):
            process = start(binary, root, root / 'cache')
            try:
                send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                    'textDocument': {'uri': path.as_uri(), 'version': 1, 'text': source}}})
                current = []
                for number, method, extra in [
                    (2, 'textDocument/diagnostic', {}),
                    (3, 'textDocument/inlayHint', {'range': {
                        'start': {'line': 0, 'character': 0},
                        'end': {'line': 1, 'character': 0}}}),
                    (4, 'textDocument/hover', {'position': {'line': 0, 'character': source.index('value')}}),
                ]:
                    send(process, {'jsonrpc': '2.0', 'id': number, 'method': method,
                        'params': {'textDocument': {'uri': path.as_uri()}, **extra}})
                    result = response(process, number)
                    assert 'error' not in result, result
                    current.append(result['result'])
                assert current[1] and current[1][0]['label'] == ': u32', current
                assert current[2], current
                answers.append(current)
                assert list((root / 'cache').glob('*.json')), 'cache was not populated'
                stop(process)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
        assert answers[0] == answers[1], answers
    print('cold/warm disk-cache diagnostics, inlay and hover parity: ok')


if __name__ == '__main__':
    main()
