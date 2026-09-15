#!/usr/bin/env python3
"""Member navigation uses receiver identity and exact declaration spans."""

from pathlib import Path
import subprocess
import sys
import tempfile

from protocol_features import request, send


def position(source, needle, name):
    offset = source.index(needle) + needle.index(name)
    before = source[:offset]
    return {'line': before.count('\n'), 'character': len(before.rsplit('\n', 1)[-1])}


def main():
    model = (
        'pub struct First { value: u32 }\n'
        'pub struct Second { value: str }\n'
        'impl for First { pub fn read(self: Self) -> u32 { ret self.value } } '
        'impl for Second { pub fn read(self: Self) -> str { ret self.value } }\n'
        'pub trait Parent[T] { fn get(self: Self) -> T }\n'
        'pub trait Child[X] < Parent[X] {}\n'
        'pub fn make() -> First { ret First { 1_u32 } }\n'
    )
    source = (
        'import refrain::model::{First, Second, Child, make}\n'
        'fn value() -> u32 { ret 0 }\n'
        'fn read() -> u32 { ret 0 }\n'
        'fn missing() -> u32 { ret 0 }\n'
        'fn sample(a: First, b: Second, c: dyn Child[First]) {\n'
        ' a.value\n b.value\n a.read()\n b.read()\n c.get().value\n make().value\n'
        ' a.missing()\n unknown.value\n}\n'
        'fn generic[T: Child[First]](item: T) { item.get().value }\n'
    )
    with tempfile.TemporaryDirectory(prefix='encore-lsp-member-navigation-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="navigation"\nversion="0.0.0"\ndependencies=[]\n')
        path = root / 'src/main.enq'
        target = root / 'src/model.enq'
        path.write_text(source)
        target.write_text(model)
        process = subprocess.Popen([str(Path(sys.argv[1]).resolve())], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            request(process, 1, 'initialize', {'rootUri': root.as_uri()})
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': path.as_uri(), 'languageId': 'encore', 'version': 1, 'text': source}}})
            cases = [
                ('a.value', 'value', 'First { value', 'value'),
                ('b.value', 'value', 'Second { value', 'value'),
                ('a.read()', 'read', 'First { pub fn read', 'read'),
                ('b.read()', 'read', 'Second { pub fn read', 'read'),
                ('c.get()', 'get', 'Parent[T] { fn get', 'get'),
                ('c.get().value', 'value', 'First { value', 'value'),
                ('make().value', 'value', 'First { value', 'value'),
                ('item.get()', 'get', 'Parent[T] { fn get', 'get'),
                ('item.get().value', 'value', 'First { value', 'value'),
            ]
            for number, (needle, name, declaration, declared_name) in enumerate(cases, 10):
                params = {'textDocument': {'uri': path.as_uri()}, 'position': position(source, needle, name)}
                result = request(process, number, 'textDocument/definition', params)
                assert len(result) == 1 and result[0]['uri'] == target.as_uri(), (needle, result)
                assert result[0]['range']['start'] == position(model, declaration, declared_name), (needle, result)
                assert result[0]['range']['end']['character'] - result[0]['range']['start']['character'] == len(name), result
            for number, needle, name in [(30, 'a.missing()', 'missing'), (31, 'unknown.value', 'value')]:
                result = request(process, number, 'textDocument/definition', {
                    'textDocument': {'uri': path.as_uri()}, 'position': position(source, needle, name)})
                assert result == [], (needle, result)
            # Opening an edited dependency must move spans without touching the caller.
            moved = '\n\n' + model
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': target.as_uri(), 'languageId': 'encore', 'version': 1, 'text': moved}}})
            for number, (needle, name, declaration, declared_name) in enumerate(cases[:4], 40):
                result = request(process, number, 'textDocument/definition', {
                    'textDocument': {'uri': path.as_uri()}, 'position': position(source, needle, name)})
                assert len(result) == 1 and result[0]['range']['start'] == position(moved, declaration, declared_name), result
            # Clicking a declaration uses exactly the same location as clicking a use.
            result = request(process, 50, 'textDocument/definition', {
                'textDocument': {'uri': target.as_uri()}, 'position': position(moved, 'Second { pub fn read', 'read')})
            assert len(result) == 1 and result[0]['range']['start'] == position(moved, 'Second { pub fn read', 'read'), result
            params = {'textDocument': {'uri': path.as_uri()}, 'position': position(source, 'a.value', 'value')}
            references = request(process, 51, 'textDocument/references', {
                **params, 'context': {'includeDeclaration': True}})
            observed = {(item['uri'], item['range']['start']['line'], item['range']['start']['character']) for item in references}
            for needle in ['a.value', 'c.get().value', 'make().value', 'item.get().value']:
                expected = position(source, needle, 'value')
                assert (path.as_uri(), expected['line'], expected['character']) in observed, (needle, references)
            for needle in ['b.value', 'fn value']:
                excluded = position(source, needle, 'value')
                assert (path.as_uri(), excluded['line'], excluded['character']) not in observed, (needle, references)
            # Do not expose unsafe partial member renames while trait-impl and
            # owner-collision validation is still pending.
            assert request(process, 52, 'textDocument/prepareRename', params) is None
            highlights = request(process, 53, 'textDocument/documentHighlight', {
                'textDocument': {'uri': target.as_uri()}, 'position': position(moved, 'First { value', 'value')})
            observed = {(item['range']['start']['line'], item['range']['start']['character']) for item in highlights}
            declaration = position(moved, 'First { value', 'value')
            usage = position(moved, 'First { pub fn read(self: Self) -> u32 { ret self.value', 'value')
            assert observed == {(declaration['line'], declaration['character']), (usage['line'], usage['character'])}, highlights
            request(process, 100, 'shutdown', {})
            send(process, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print('member declarations, inherited origins, exact spans and dependency edits: ok')


if __name__ == '__main__':
    main()
