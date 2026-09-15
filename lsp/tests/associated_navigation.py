#!/usr/bin/env python3
"""Associated calls must not resolve to unrelated same-named free functions."""

from pathlib import Path
import subprocess
import sys
import tempfile

from protocol_features import request, send


def main():
    source = '''struct Box[Element] { value: Element }
impl[Element] for Box[Element] {
 /// Builds a box.
 fn new(value: Element) -> Self { ret Self { value } } fn read(self: Self) -> Element { ret self.value }
}
fn new() -> str { ret "unrelated" }
fn sample() { let box = Box[u32]::new(1_u32) }
'''
    with tempfile.TemporaryDirectory(prefix='encore-lsp-associated-navigation-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="associated"\nversion="0.0.0"\ndependencies=[]\n')
        path = root / 'src/main.enq'
        path.write_text(source)
        process = subprocess.Popen([str(Path(sys.argv[1]).resolve())], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            request(process, 1, 'initialize', {'rootUri': root.as_uri()})
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': path.as_uri(), 'languageId': 'encore', 'version': 1, 'text': source}}})
            column = source.splitlines()[6].index('::new') + 2
            params = {'textDocument': {'uri': path.as_uri()}, 'position': {'line': 6, 'character': column}}
            definitions = request(process, 2, 'textDocument/definition', params)
            assert definitions and definitions[0]['range']['start'] == {'line': 3, 'character': 4}, definitions
            completions = request(process, 20, 'textDocument/completion', params)['items']
            assert [item['label'] for item in completions] == ['new'], completions
            assert completions[0]['detail'] == 'fn new(u32) -> Box[u32]', completions
            params['position']['character'] = column + 2
            assert request(process, 21, 'textDocument/completion', params)['items'] == completions
            params['position']['character'] = column
            hover = request(process, 3, 'textDocument/hover', params)
            assert 'value: u32' in str(hover) and 'Box[u32]' in str(hover) and 'Builds a box.' in str(hover), hover
            params['position']['character'] = column + 4
            signature = request(process, 4, 'textDocument/signatureHelp', params)
            assert signature['signatures'][0]['label'] == 'new(value: u32) -> Box[u32]', signature
            params['position']['character'] = column
            highlights = request(process, 5, 'textDocument/documentHighlight', params)
            assert {item['range']['start']['line'] for item in highlights} == {3, 6}, highlights
            incomplete = source[:source.index('new(1_u32)', source.index('fn sample')) + 4]
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 2}, 'contentChanges': [{'text': incomplete}]}})
            params['position']['character'] = column + 4
            signature = request(process, 6, 'textDocument/signatureHelp', params)
            assert signature['signatures'][0]['label'] == 'new(value: u32) -> Box[u32]', signature
            model = root / 'src/model.enq'
            model_source = '\n'.join(source.splitlines()[:5]).replace('struct Box', 'pub struct Box').replace(' fn new', ' pub fn new')
            model.write_text(model_source)
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': model.as_uri(), 'languageId': 'encore', 'version': 1, 'text': model_source}}})
            imported = 'import refrain::model::Box as Alias\nfn new() -> bool { ret false }\nfn sample() { let box = Alias[str]::new("value") }\n'
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 3}, 'contentChanges': [{'text': imported}]}})
            imported_column = imported.splitlines()[2].index('::new') + 2
            params['position'] = {'line': 2, 'character': imported_column}
            definitions = request(process, 7, 'textDocument/definition', params)
            assert definitions and definitions[0]['uri'] == model.as_uri(), definitions
            hover = request(process, 8, 'textDocument/hover', params)
            assert 'value: str' in str(hover) and 'Box[str]' in str(hover), hover
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': model.as_uri(), 'version': 2}, 'contentChanges': [
                    {'text': model_source.replace('-> Self { ret Self { value } }', '-> bool { ret true }')} ]}})
            hover = request(process, 9, 'textDocument/hover', params)
            assert 'value: str' in str(hover) and '-> bool' in str(hover), hover
            completions = request(process, 22, 'textDocument/completion', params)['items']
            assert [item['label'] for item in completions] == ['new'], completions
            assert completions[0]['detail'] == 'fn new(str) -> bool', completions
            unfinished = imported[:imported.index('::new') + 2]
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 4}, 'contentChanges': [{'text': unfinished}]}})
            assert request(process, 23, 'textDocument/completion', params)['items'] == completions
            request(process, 10, 'shutdown', {})
            send(process, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print('associated declaration identity, documentation and specialized signature: ok')


if __name__ == '__main__':
    main()
