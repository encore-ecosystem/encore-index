#!/usr/bin/env python3
"""Completion uses the same public re-export namespace as navigation."""

from pathlib import Path
import subprocess
import sys
import tempfile

from protocol_features import request, send


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='encore-lsp-exports-') as directory:
        root = Path(directory)
        src = root / 'src'
        src.mkdir()
        (root / 'encore.toml').write_text(
            '[project]\nname="exports"\nversion="0.0.0"\ndependencies=[]\n')
        (src / 'a.enq').write_text('pub import refrain::b::original as renamed\n')
        for name, target in zip('bcde', 'cdef'):
            (src / f'{name}.enq').write_text(f'pub import refrain::{target}::*\n')
        (src / 'f.enq').write_text(
            '/// Original documentation survives re-exports.\n'
            'pub fn original() -> u32 { ret 1 }\n'
            'fn private_function() -> u32 { ret 0 }\n'
            'pub trait Reader { fn read(self: Self) -> u32 }\n')
        path = src / 'main.enq'
        path.write_text('fn main() -> u32 { ret 0 }\n')
        uri = path.as_uri()
        p = subprocess.Popen([str(binary)], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            initialized = request(p, 1, 'initialize', {
                'rootUri': root.as_uri(),
                'capabilities': {'textDocument': {'completion': {'completionItem': {
                    'resolveSupport': {'properties': ['documentation']},
                }}}},
            })
            assert initialized['capabilities']['completionProvider']['resolveProvider']
            send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': uri, 'languageId': 'encore', 'version': 1,
                                 'text': path.read_text()},
            }})
            for version, module, expected in [(2, 'a', {'renamed'}),
                                              (3, 'b', {'original', 'Reader'})]:
                source = f'import refrain::{module}::'
                send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                    'textDocument': {'uri': uri, 'version': version},
                    'contentChanges': [{'text': source}],
                }})
                result = request(p, version * 10, 'textDocument/completion', {
                    'textDocument': {'uri': uri},
                    'position': {'line': 0, 'character': len(source)},
                })
                items = {item['label']: item for item in result['items']}
                assert expected <= items.keys(), (module, items)
                assert not {'private_function', 'read'} & items.keys(), items
                if module == 'a':
                    assert 'documentation' not in items['renamed'], items
                    unresolved = items['renamed']
                    resolved = request(p, 40, 'completionItem/resolve', unresolved)
                    assert 'Original documentation' in str(resolved), resolved
                    assert resolved['label'] == 'renamed', resolved
                    assert resolved['data'] == unresolved['data'], resolved
            # A delayed resolve must not attach documentation from a newer
            # declaration occupying the old coordinates.
            origin = src / 'f.enq'
            send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': origin.as_uri(), 'languageId': 'encore', 'version': 1,
                                 'text': '\n' + origin.read_text()},
            }})
            assert request(p, 41, 'completionItem/resolve', unresolved) == unresolved
            request(p, 90, 'shutdown', {})
            send(p, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
            assert p.wait(timeout=10) == 0
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()
    print('deep and named re-exports, public visibility and documentation: ok')


if __name__ == '__main__':
    main()
