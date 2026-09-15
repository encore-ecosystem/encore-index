#!/usr/bin/env python3
"""Variant navigation uses owner identity and exact declaration spans."""

from pathlib import Path
import subprocess
import sys
import tempfile
import threading

from protocol_features import request, send


def main():
    with tempfile.TemporaryDirectory(prefix='encore-lsp-enum-navigation-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="variants"\nversion="0.0.0"\ndependencies=[]\n')
        model = root / 'src/model.enq'
        model_source = ('pub enum Choice[T] {\n /// Carries the selected value.\n Value(T), Empty\n}\n'
                        'pub enum Other { Value, Empty }\n')
        model.write_text(model_source)
        (root / 'src/api.enq').write_text('pub import refrain::model::Choice as Exported\n')
        path = root / 'src/main.enq'
        source = ('import refrain::api::Exported as Alias\nimport refrain::model::Other\n'
                  'fn sample() {\n let a = Alias[u32]::Value(1_u32)\n let b = Other::Value\n}\n')
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

        def open_document(file, text):
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': file.as_uri(), 'version': 1, 'text': text}}})

        try:
            call('initialize', {'rootUri': root.as_uri()})
            open_document(path, source)
            for line, declaration_line, declaration_column in [(3, 2, 1), (4, 4, 17)]:
                column = source.splitlines()[line].index('::Value') + 2
                params = {'textDocument': {'uri': path.as_uri()},
                          'position': {'line': line, 'character': column}}
                locations = call('textDocument/definition', params)
                assert len(locations) == 1, locations
                assert locations[0]['uri'] == model.as_uri(), locations
                assert locations[0]['range'] == {
                    'start': {'line': declaration_line, 'character': declaration_column},
                    'end': {'line': declaration_line, 'character': declaration_column + 5}}, locations
                if line == 3:
                    hover = call('textDocument/hover', params)
                    assert 'Alias[u32]::Value' in str(hover) and 'Carries the selected value.' in str(hover), hover
                completions = call('textDocument/completion', params)['items']
                assert {item['label'] for item in completions} == {'Value', 'Empty'}, completions
                assert all(item['kind'] == 20 for item in completions), completions
                params['position']['character'] = column + 2
                filtered = call('textDocument/completion', params)['items']
                assert [item['label'] for item in filtered] == ['Value'], filtered
            open_document(model, model_source)
            declaration = {'textDocument': {'uri': model.as_uri()},
                           'position': {'line': 2, 'character': 1}}
            own = call('textDocument/definition', declaration)
            assert len(own) == 1 and own[0]['range']['start'] == declaration['position'], own
            references = call('textDocument/references', {**declaration, 'context': {'includeDeclaration': True}})
            assert {(item['uri'], item['range']['start']['line']) for item in references} == {
                (model.as_uri(), 2), (path.as_uri(), 3)}, references
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': model.as_uri(), 'version': 2},
                'contentChanges': [{'text': model_source.replace('Value(T), Empty', 'Empty')}]}})
            removed = {'textDocument': {'uri': path.as_uri()}, 'position': {
                'line': 3, 'character': source.splitlines()[3].index('::Value') + 2}}
            assert call('textDocument/definition', removed) == []
            assert [item['label'] for item in call('textDocument/completion', removed)['items']] == ['Empty']
            call('shutdown', {})
            send(process, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print('enum variant aliases, distinct owners, documentation and exact spans: ok')


if __name__ == '__main__':
    main()
