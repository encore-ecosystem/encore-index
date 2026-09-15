#!/usr/bin/env python3
"""Rename identities distinguish declarations from explicit import aliases."""

from pathlib import Path
import subprocess
import sys
import tempfile

from protocol_features import request, send


def main():
    sources = {
        'origin': 'pub fn original() -> u32 { ret 1 }\n',
        'facade': 'pub import refrain::origin::original as exported\n',
        'direct': ('import refrain::origin::{original as first, original as second}\n'
                   'fn sample() -> u32 { ret first() + second() }\n'),
        'same': ('import refrain::origin::original as original\n'
                 'fn sample() -> u32 { ret original() }\n'),
        'main': ('import refrain::facade::exported\n'
                 'fn main() -> u32 { ret exported() }\n'),
        'consumer': ('import refrain::facade::exported as local\n'
                     'fn sample() -> u32 { ret local() }\n'),
    }
    with tempfile.TemporaryDirectory(prefix='encore-lsp-import-rename-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="renames"\nversion="0.0.0"\ndependencies=[]\n')
        paths = {name: root / 'src' / f'{name}.enq' for name in sources}
        for name, source in sources.items():
            paths[name].write_text(source)
        p = subprocess.Popen([str(Path(sys.argv[1]).resolve())], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            request(p, 1, 'initialize', {'rootUri': root.as_uri(), 'capabilities': {
                'workspace': {'workspaceEdit': {'documentChanges': True}}}})
            for name, path in paths.items():
                send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                    'textDocument': {'uri': path.as_uri(), 'version': 1,
                                     'languageId': 'encore', 'text': sources[name]}}})
            # Every use still navigates to the real function declaration.
            for number, (name, spelling) in enumerate([
                ('direct', 'first'), ('direct', 'second'), ('same', 'original'),
                ('main', 'exported'), ('consumer', 'local'),
            ], 10):
                line = sources[name].splitlines()[1]
                locations = request(p, number, 'textDocument/definition', {
                    'textDocument': {'uri': paths[name].as_uri()},
                    'position': {'line': 1, 'character': line.index(spelling) + 1}})
                assert locations and locations[0]['uri'] == paths['origin'].as_uri(), (name, locations)
            for number, name, spelling, expected in [
                (30, 'origin', 'original', {'origin': 1, 'facade': 1, 'direct': 2, 'same': 1}),
                (31, 'facade', 'exported', {'facade': 1, 'main': 2, 'consumer': 1}),
                (32, 'direct', 'first', {'direct': 2}),
                (33, 'same', 'original', {'same': 2}),
            ]:
                line = sources[name].splitlines()[0]
                column = line.rindex(spelling) + 1
                edit = request(p, number, 'textDocument/rename', {
                    'textDocument': {'uri': paths[name].as_uri()},
                    'position': {'line': 0, 'character': column}, 'newName': 'updated'})
                assert 'changes' not in edit, edit
                document_changes = edit.get('documentChanges', [])
                for change in document_changes:
                    assert change['textDocument']['version'] == (1 if number == 30 else (number - 1) * 2 + 1), edit
                changes = {change['textDocument']['uri']: change['edits'] for change in document_changes}
                actual = {Path(uri).stem: len(items) for uri, items in changes.items()}
                assert actual == expected, (name, actual, expected, edit)
                for uri, items in changes.items():
                    lines = sources[Path(uri).stem].splitlines()
                    for item in items:
                        start, end = item['range']['start'], item['range']['end']
                        assert start['line'] == end['line'], item
                        assert lines[start['line']][start['character']:end['character']] == spelling, item
                if name == 'origin':
                    item = changes[paths['same'].as_uri()][0]
                    assert item['range']['start']['character'] == sources['same'].index('original'), edit
                modified = dict(sources)
                for uri, items in changes.items():
                    module = Path(uri).stem
                    lines = modified[module].splitlines(keepends=True)
                    for item in sorted(items, key=lambda item: (
                            item['range']['start']['line'], item['range']['start']['character']), reverse=True):
                        start, end = item['range']['start'], item['range']['end']
                        row = lines[start['line']]
                        lines[start['line']] = row[:start['character']] + item['newText'] + row[end['character']:]
                    modified[module] = ''.join(lines)
                for module, source in modified.items():
                    send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                        'textDocument': {'uri': paths[module].as_uri(), 'version': number * 2},
                        'contentChanges': [{'text': source}]}})
                for offset, module in enumerate(['direct', 'same', 'main', 'consumer']):
                    line = modified[module].splitlines()[1]
                    locations = request(p, number * 100 + offset, 'textDocument/definition', {
                        'textDocument': {'uri': paths[module].as_uri()},
                        'position': {'line': 1, 'character': line.index('ret ') + 5}})
                    assert locations and locations[0]['uri'] == paths['origin'].as_uri(), (name, module, modified, locations)
                for module, source in sources.items():
                    send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                        'textDocument': {'uri': paths[module].as_uri(), 'version': number * 2 + 1},
                        'contentChanges': [{'text': source}]}})
            send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didClose', 'params': {
                'textDocument': {'uri': paths['consumer'].as_uri()}}})
            edit = request(p, 80, 'textDocument/rename', {
                'textDocument': {'uri': paths['facade'].as_uri()},
                'position': {'line': 0, 'character': sources['facade'].index('exported') + 1},
                'newName': 'closed_test'})
            versions = {change['textDocument']['uri']: change['textDocument']['version']
                        for change in edit['documentChanges']}
            assert versions == {paths['facade'].as_uri(): 67, paths['main'].as_uri(): 67,
                                paths['consumer'].as_uri(): None}, versions
            request(p, 99, 'shutdown', {})
            send(p, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
            assert p.wait(timeout=10) == 0
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()
    print('independent alias rename identities across grouped imports and re-exports: ok')


if __name__ == '__main__':
    main()
