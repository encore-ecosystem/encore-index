#!/usr/bin/env python3
"""Worker diagnostics distinguish an empty aggregate from an unloaded type."""

from pathlib import Path
import subprocess
import sys
import tempfile

from protocol_features import request, send


def main():
    with tempfile.TemporaryDirectory(prefix='encore-lsp-empty-type-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="empty_type"\nversion="0.0.0"\ndependencies=[]\n')
        path = root / 'src/main.enq'
        source = 'struct Empty {}\nfn sample(data: Empty) { let value = data.missing }\n'
        path.write_text(source)
        p = subprocess.Popen([str(Path(sys.argv[1]).resolve())], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            request(p, 1, 'initialize', {'rootUri': root.as_uri()})
            send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 1, 'languageId': 'encore', 'text': source}}})
            diagnostics = request(p, 2, 'textDocument/diagnostic', {'textDocument': {'uri': path.as_uri()}})['items']
            fields = [item for item in diagnostics if item['code'] == 'unknown-field']
            assert len(fields) == 1, diagnostics
            assert fields[0]['range']['start']['line'] == 1, fields
            changed = 'struct Empty { missing: u32 }\nfn sample(data: Empty) { let value = data.missing }\n'
            send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 2}, 'contentChanges': [{'text': changed}]}})
            diagnostics = request(p, 3, 'textDocument/diagnostic', {'textDocument': {'uri': path.as_uri()}})['items']
            assert not any(item['code'] == 'unknown-field' for item in diagnostics), diagnostics
            changed = 'fn sample(data: NotLoaded) { let value = data.missing }\n'
            send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 3}, 'contentChanges': [{'text': changed}]}})
            diagnostics = request(p, 4, 'textDocument/diagnostic', {'textDocument': {'uri': path.as_uri()}})['items']
            assert any(item['code'] == 'unknown-type' for item in diagnostics), diagnostics
            assert not any(item['code'] == 'unknown-field' for item in diagnostics), diagnostics
            request(p, 5, 'shutdown', {})
            send(p, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
            assert p.wait(timeout=10) == 0
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()
    print('empty aggregate diagnostics and recovery after adding a field: ok')


if __name__ == '__main__':
    main()
