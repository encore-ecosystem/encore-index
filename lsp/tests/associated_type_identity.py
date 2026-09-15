#!/usr/bin/env python3
"""Same-named types from different source modules must never share members."""

from pathlib import Path
import subprocess
import sys
import tempfile
import threading

from protocol_features import request, send


def main():
    with tempfile.TemporaryDirectory(prefix='encore-lsp-type-identity-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="identities"\nversion="0.0.0"\ndependencies=[]\n')
        left = root / 'src/left.enq'
        right = root / 'src/right.enq'
        left.write_text('pub struct Box[T] {}\nimpl[T] for Box[T] { pub fn new() -> u32 { ret 1_u32 } pub fn left() -> bool { ret true } fn secret() -> bool { ret true } }\n')
        right.write_text('pub struct Box[T] {}\nimpl[T] for Box[T] { pub fn new() -> str { ret "right" } pub fn right() -> bool { ret true } }\n')
        source = ('import refrain::left::Box as Left\nimport refrain::right::Box as Right\n'
                  'fn main() {\n Left[u32]::new()\n Right[u32]::new()\n refrain::left::Box[u32]::new()\n}\n')
        path = root / 'src/main.enq'
        path.write_text(source)
        process = subprocess.Popen([str(Path(sys.argv[1]).resolve())], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        number = 0

        def call(method, params):
            nonlocal number
            number += 1
            timer = threading.Timer(20, process.kill)
            timer.start()
            try:
                return request(process, number, method, params)
            finally:
                timer.cancel()

        try:
            call('initialize', {'rootUri': root.as_uri()})
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 1, 'text': source}}})
            for line, origin, typ, expected, excluded in [
                (3, left, 'u32', 'left', 'right'),
                (4, right, 'str', 'right', 'left'),
                (5, left, 'u32', 'left', 'right'),
            ]:
                column = source.splitlines()[line].index('::new') + 2
                params = {'textDocument': {'uri': path.as_uri()}, 'position': {'line': line, 'character': column}}
                items = call('textDocument/completion', params)['items']
                members = {item['label']: item for item in items}
                assert expected in members and excluded not in members, (line, members)
                assert 'secret' not in members, members
                assert members['new']['detail'] == f'fn new() -> {typ}', members
                definitions = call('textDocument/definition', params)
                assert len(definitions) == 1 and definitions[0]['uri'] == origin.as_uri(), definitions
                assert f'-> {typ}' in str(call('textDocument/hover', params))
                highlights = call('textDocument/documentHighlight', params)
                assert {item['range']['start']['line'] for item in highlights} == ({3, 5} if line != 4 else {4}), highlights
            # A qualified path must work without a redundant import statement.
            qualified = 'fn main() { refrain::left::Box[u32]::new() }\n'
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 2}, 'contentChanges': [{'text': qualified}]}})
            params['position'] = {'line': 0, 'character': qualified.index('::new') + 2}
            items = call('textDocument/completion', params)['items']
            assert {item['label'] for item in items} == {'new', 'left'}, items
            definitions = call('textDocument/definition', params)
            assert len(definitions) == 1 and definitions[0]['uri'] == left.as_uri(), definitions
            redirected = 'import refrain::right::Box as Left\nfn main() { Left[u32]::new() }\n'
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 3}, 'contentChanges': [{'text': redirected}]}})
            params['position'] = {'line': 1, 'character': redirected.splitlines()[1].index('::new') + 2}
            definitions = call('textDocument/definition', params)
            assert len(definitions) == 1 and definitions[0]['uri'] == right.as_uri(), definitions
            assert '-> str' in str(call('textDocument/hover', params))
            call('shutdown', {})
            send(process, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print('distinct associated type identities, aliases, qualified paths and references: ok')


if __name__ == '__main__':
    main()
