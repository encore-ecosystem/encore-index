#!/usr/bin/env python3
"""Local navigation and rename respect declaration identity and shadowing."""

from pathlib import Path
import subprocess
import sys
import tempfile
import time

from protocol_features import read, request, send


def request_conflicting_rename(process, number, params):
    send(process, {'jsonrpc': '2.0', 'id': number, 'method': 'textDocument/rename', 'params': params})
    while True:
        message = read(process)
        if message.get('id') == number:
            assert 'result' not in message, message
            assert message['error']['code'] == -32803, message
            assert 'would change which declaration' in message['error']['message'], message
            return


def main():
    source = (
        'fn first() -> u32 {\n'
        ' let value = 1_u32\n'
        ' if true {\n'
        '  let value = value + 1_u32\n'
        '  let inner = value\n'
        ' }\n'
        ' ret value\n'
        '}\n'
        'fn parameters(\n'
        ' value: u32,\n'
        ' other: u32,\n'
        ') -> u32 {\n'
        ' let copied = value\n'
        ' if true { let value = other }\n'
        ' ret value + copied\n'
        '}\n'
        'fn second() -> u32 {\n'
        ' let value = 3_u32\n'
        ' ret value\n'
        '}\n'
        'struct Box { value: u32 }\n'
        'impl for Box {\n'
        ' fn read(self) -> u32 {\n'
        '  ret self.value\n'
        ' }\n'
        '}\n'
    )
    with tempfile.TemporaryDirectory(prefix='encore-lsp-locals-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="locals"\nversion="0.0.0"\ndependencies=[]\n')
        path = root / 'src/main.enq'
        path.write_text(source)
        uri = path.as_uri()
        p = subprocess.Popen([str(Path(sys.argv[1]).resolve())], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            versioned = '--versioned' in sys.argv[2:]
            request(p, 1, 'initialize', {'rootUri': root.as_uri(), 'capabilities': {
                'workspace': {'workspaceEdit': {'documentChanges': versioned}}}})
            send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': uri, 'version': 1, 'languageId': 'encore', 'text': source}}})
            request(p, 2, 'textDocument/diagnostic', {'textDocument': {'uri': uri}})
            for number, (line, column, expected_line, expected_column) in enumerate([
                (6, 6, 1, 5), (4, 15, 3, 6), (3, 15, 1, 5), (3, 7, 3, 6),
                (12, 15, 9, 1), (14, 6, 9, 1), (9, 2, 9, 1), (13, 24, 10, 1), (18, 6, 17, 5),
                (23, 7, 22, 9),
            ], 10):
                result = request(p, number, 'textDocument/definition', {
                    'textDocument': {'uri': uri}, 'position': {'line': line, 'character': column}})
                assert result and result[0]['uri'] == uri, (line, column, result)
                assert result[0]['range']['start'] == {'line': expected_line, 'character': expected_column}, result
            for number, (line, column, expected) in enumerate([
                (6, 6, {(1, 5), (3, 14), (6, 5)}),
                (4, 15, {(3, 6), (4, 14)}),
                (12, 15, {(9, 1), (12, 14), (14, 5)}),
                (13, 24, {(10, 1), (13, 23)}),
                (18, 6, {(17, 5), (18, 5)}),
            ], 20):
                started = time.monotonic()
                edit = request(p, number, 'textDocument/rename', {
                    'textDocument': {'uri': uri}, 'position': {'line': line, 'character': column},
                    'newName': 'renamed'})
                assert time.monotonic() - started < 2.0, 'local rename scanned unrelated workspace bodies'
                if versioned:
                    assert 'changes' not in edit, edit
                    assert len(edit['documentChanges']) == 1, edit
                    change = edit['documentChanges'][0]
                    assert change['textDocument'] == {'uri': uri, 'version': 1}, edit
                    edits = change['edits']
                else:
                    assert set(edit['changes']) == {uri}, edit
                    edits = edit['changes'][uri]
                actual = {(item['range']['start']['line'], item['range']['start']['character'])
                          for item in edits}
                assert actual == expected, (actual, expected, edit)
            for include, expected in [(True, {(3, 6), (4, 14)}), (False, {(4, 14)})]:
                locations = request(p, 30 + int(include), 'textDocument/references', {
                    'textDocument': {'uri': uri}, 'position': {'line': 4, 'character': 15},
                    'context': {'includeDeclaration': include}})
                actual = {(item['range']['start']['line'], item['range']['start']['character']) for item in locations}
                assert actual == expected, (actual, expected)
            highlights = request(p, 32, 'textDocument/documentHighlight', {
                'textDocument': {'uri': uri}, 'position': {'line': 4, 'character': 15}})
            actual = {(item['range']['start']['line'], item['range']['start']['character']) for item in highlights}
            assert actual == {(3, 6), (4, 14)}, highlights
            for number, new_name in enumerate(['fn', 'let', 'self', 'Self', 'bad name'], 40):
                edit = request(p, number, 'textDocument/rename', {
                    'textDocument': {'uri': uri}, 'position': {'line': 12, 'character': 15},
                    'newName': new_name})
                assert not edit.get('changes') and not edit.get('documentChanges'), edit
            assert request(p, 50, 'textDocument/prepareRename', {
                'textDocument': {'uri': uri}, 'position': {'line': 23, 'character': 7}}) is None
            # Renaming a parameter must not redirect its uses to another parameter,
            # or cause existing uses of a different local to become captured.
            for number, line, column, new_name in [
                (60, 12, 15, 'other'), (61, 13, 24, 'value'), (62, 12, 15, 'copied'),
            ]:
                request_conflicting_rename(p, number, {
                    'textDocument': {'uri': uri}, 'position': {'line': line, 'character': column},
                    'newName': new_name})
            # Identical names in separate functions are not a rename conflict.
            edit = request(p, 63, 'textDocument/rename', {
                'textDocument': {'uri': uri}, 'position': {'line': 18, 'character': 6},
                'newName': 'other'})
            assert edit.get('changes') or edit.get('documentChanges'), edit
            if versioned:
                send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                    'textDocument': {'uri': uri, 'version': 7},
                    'contentChanges': [{'text': source + '\n'}]}})
                edit = request(p, 51, 'textDocument/rename', {
                    'textDocument': {'uri': uri}, 'position': {'line': 6, 'character': 6},
                    'newName': 'updated'})
                assert edit['documentChanges'][0]['textDocument'] == {'uri': uri, 'version': 7}, edit
            for number, (fixture, line, old, new, allowed) in enumerate([
                ('fn external() {}\nfn sample() {\n let local = 1_u32\n external()\n}\n',
                 2, 'local', 'external', False),
                ('fn sample() {\n let value = 1_u32\n if true {\n let inner = 2_u32\n let used = value + inner\n }\n}\n',
                 1, 'value', 'inner', False),
                ('fn sample() {\n if true { let first = 1_u32 first }\n if true { let second = 2_u32 second }\n}\n',
                 1, 'first', 'second', True),
            ], 70):
                send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                    'textDocument': {'uri': uri, 'version': number},
                    'contentChanges': [{'text': fixture}]}})
                params = {
                    'textDocument': {'uri': uri},
                    'position': {'line': line, 'character': fixture.splitlines()[line].index(old) + 1},
                    'newName': new}
                if allowed:
                    edit = request(p, number, 'textDocument/rename', params)
                    assert edit.get('changes') or edit.get('documentChanges'), (fixture, edit)
                else:
                    request_conflicting_rename(p, number, params)
            request(p, 99, 'shutdown', {})
            send(p, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
            assert p.wait(timeout=10) == 0
            assert path.read_text() == source
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()
    print('local declaration navigation, initializer scope and isolated rename: ok')


if __name__ == '__main__':
    main()
