#!/usr/bin/env python3
"""Quick fixes use current document versions and UTF-16 edit coordinates."""

from pathlib import Path
import subprocess
import sys
import tempfile

from protocol_features import request, send


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="encore-lsp-actions-") as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="actions"\nversion="0.0.0"\ndependencies=[]\n')
        path = root / 'src/main.enq'
        source = 'fn main() -> str { ret "🌍" }   \n'
        path.write_text(source)
        uri = path.as_uri()
        p = subprocess.Popen([str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            info = request(p, 1, 'initialize', {'rootUri': root.as_uri()})
            assert info['capabilities']['codeActionProvider']
            send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': uri, 'languageId': 'encore', 'version': 7, 'text': source},
            }})
            params = {'textDocument': {'uri': uri}, 'range': {
                'start': {'line': 0, 'character': 0}, 'end': {'line': 0, 'character': 100},
            }, 'context': {'diagnostics': []}}
            diagnostics = request(p, 2, 'textDocument/diagnostic', {'textDocument': {'uri': uri}})
            actions = request(p, 3, 'textDocument/codeAction', params)
            assert len(actions) == 1, (actions, diagnostics)
            change = actions[0]['edit']['documentChanges'][0]
            assert change['textDocument'] == {'uri': uri, 'version': 7}, change
            edit = change['edits'][0]
            expected = len(source.rstrip().encode('utf-16-le')) // 2
            assert edit['range']['start'] == {'line': 0, 'character': expected}, edit
            assert edit['range']['end'] == {'line': 0, 'character': expected + 3}, edit
            assert edit['newText'] == '', edit
            send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': uri, 'version': 8}, 'contentChanges': [{'text': source.rstrip() + '\n'}],
            }})
            request(p, 4, 'textDocument/diagnostic', {'textDocument': {'uri': uri}})
            assert request(p, 5, 'textDocument/codeAction', params) == []
            request(p, 6, 'shutdown', {})
            send(p, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
            assert p.wait(timeout=10) == 0
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()
    print('versioned code actions and Unicode edits: ok')


if __name__ == '__main__':
    main()
