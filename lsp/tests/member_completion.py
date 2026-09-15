#!/usr/bin/env python3
"""Receiver completion respects generic types and nested lexical scopes."""

from pathlib import Path
import subprocess
import sys
import tempfile

from protocol_features import request, send


def main():
    declarations = (
        'struct Inner { label: str }\n'
        'struct Box[T] { value: T inner: Inner }\n'
        'impl[T] for Box[T] {\n'
        ' fn read(self: Self) -> T { ret self.value }\n'
        ' fn create() -> u32 { ret 0 }\n'
        '}\n'
        'fn make() -> Box[u32] { ret Box[u32] { 1_u32, Inner { "x" } } }\n'
        'trait Root { fn read(self: Self) -> u32 fn static_call() -> u32 }\n'
        'trait Middle < Root {}\n'
        'trait Leaf < Middle { fn own(self: Self) -> str }\n'
        'trait Get[ResultType] { fn get(self: Self) -> ResultType }\n'
        'trait Identity { fn identity(self: Self) -> Self }\n'
        'trait Parent[A, B] { fn left(self: Self) -> A fn right(self: Self) -> B fn again(self: Self) -> Self }\n'
        'trait Swap[A, B] < Parent[B, A] {}\n'
        'trait Final[X] < Swap[u32, X] {}\n'
        'struct Holder[T] { item: T }\n'
        'impl for Box[u32] { fn number(self: Self) -> u32 { ret self.value } }\n'
        'impl[Element] for Box[Box[Element]] { fn nested(self: Self) -> Element { ret self.value.value } }\n'
    )
    cases = [
        ('fn sample() {\n Box[u32]::|', {'create'}, {'read', 'number', 'value', 'inner'}),
        ('fn sample() {\n Box[u32]::cr|', {'create'}, {'read', 'number'}),
        ('fn sample() {\n Box[u32]::missing|', set(), {'create', 'read', 'fn'}),
        ('fn sample(value: Box[str]) {\n value.|', {'inner'}, {'number', 'nested'}),
        ('fn sample(value: Box[u32]) {\n value.|', {'number'}, {'nested'}),
        ('fn sample(value: Box[Box[Inner]]) {\n value.nested().|', {'label'}, {'number', 'nested', 'read'}),
        ('fn sample(value: dyn Final[Inner]) {\n value.left().|', {'label'}, {'read', 'get'}),
        ('fn sample[T: Final[Inner]](value: T) {\n value.left().|', {'label'}, {'read', 'get'}),
        ('fn sample[T: Final[Inner]](value: T) {\n value.again().left().|', {'label'}, {'read', 'get'}),
        ('fn sample[T: Root](value: T) {\n value.|', {'read'}, {'static_call', 'value', 'get'}),
        ('fn sample[T: Leaf](value: T) {\n value.|', {'read', 'own'}, {'static_call', 'value'}),
        ('fn sample[T: Get[Inner]](value: T) {\n value.get().|', {'label'}, {'read', 'get'}),
        ('fn sample[T: Root + Identity](value: T) {\n value.identity().|', {'read', 'identity'}, {'get', 'label'}),
        ('fn sample[T](value: T) {\n value.|', set(), {'read', 'own', 'get', 'label'}),
        ('impl[T: Root] for Holder[T] {\n fn sample(self: Self) {\n self.item.|\n }\n}', {'read'}, {'get', 'label'}),
        ('fn sample(value: dyn Leaf) {\n value.|', {'read', 'own'}, {'static_call', 'value'}),
        ('fn sample() {\n make().|', {'value', 'inner', 'read'}, {'label', 'create'}),
        ('fn sample() {\n make().in|', {'inner'}, {'value', 'read'}),
        ('fn sample() {\n make().inner.|', {'label'}, {'value', 'read'}),
        ('fn sample(items: [Inner; 2]) {\n items[0_usize].|', {'label'}, {'value', 'read'}),
        ('fn sample(pair: (Inner, u32)) {\n pair.0.|', {'label'}, {'value', 'read'}),
        ('fn sample(pair: (Inner, u32)) {\n pair.|', {'0', '1'}, {'label', 'read'}),
        ('fn sample(value: Box[Inner]) {\n value.read().|', {'label'}, {'value', 'read'}),
        ('fn sample(items: [Inner; 2]) {\n items[0_usize].la|\n}', {'label'}, {'value', 'read'}),
        ('fn sample(value: Box[u32]) {\n (value).|', {'value', 'read'}, {'label'}),
        ('fn sample(value: Box[u32]) {\n value.|', {'value', 'inner', 'read'}, {'label', 'create'}),
        ('fn sample(value: Box[u32]) {\n value.re|', {'read'}, {'value', 'inner'}),
        ('fn sample(value: Box[u32]) {\n value.inner.|', {'label'}, {'read', 'value'}),
        ('fn sample(value: Box[u32]) {\n if true {\n let value = Inner { "x" }\n value.|\n }\n}',
         {'label'}, {'read', 'value'}),
        ('fn sample(value: Box[u32]) {\n if true { let value = Inner { "x" } }\n value.|\n}',
         {'value', 'read'}, {'label'}),
        ('fn sample() {\n later.|\n let later = Inner { "x" }\n}', set(), {'label', 'read', 'value'}),
    ]
    with tempfile.TemporaryDirectory(prefix='encore-lsp-members-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="members"\nversion="0.0.0"\ndependencies=[]\n')
        path = root / 'src/main.enq'
        path.write_text('fn main() -> u32 { ret 0 }\n')
        p = subprocess.Popen([str(Path(sys.argv[1]).resolve())], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            request(p, 1, 'initialize', {'rootUri': root.as_uri()})
            send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': path.as_uri(), 'languageId': 'encore', 'version': 1, 'text': path.read_text()},
            }})
            for version, (marked, expected, excluded) in enumerate(cases, 2):
                before, after = (declarations + marked).split('|')
                source = before + after
                send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                    'textDocument': {'uri': path.as_uri(), 'version': version}, 'contentChanges': [{'text': source}],
                }})
                result = request(p, version * 10, 'textDocument/completion', {
                    'textDocument': {'uri': path.as_uri()}, 'position': {
                        'line': before.count('\n'), 'character': len(before.rsplit('\n', 1)[-1]),
                    },
                })
                items = {item['label']: item for item in result['items']}
                assert expected <= items.keys() and not excluded & items.keys(), (marked, items)
                if 'read' in expected:
                    assert items['read']['kind'] == 2 and 'u32' in items['read']['detail'], items
                if 'value' in expected:
                    assert items['value']['detail'] == 'u32', items
            for version, header, expected, excluded in [
                (40, declarations, {'value', 'read'}, {'label'}),
                (41, declarations.replace(
                    'fn make() -> Box[u32] { ret Box[u32] { 1_u32, Inner { "x" } } }',
                    'fn make() -> Inner { ret Inner { "new" } }'), {'label'}, {'value', 'read'}),
            ]:
                source = header + 'fn sample() {\n make().'
                send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                    'textDocument': {'uri': path.as_uri(), 'version': version},
                    'contentChanges': [{'text': source}]}})
                result = request(p, version * 10, 'textDocument/completion', {
                    'textDocument': {'uri': path.as_uri()},
                    'position': {'line': source.count('\n'), 'character': 8}})
                labels = {item['label'] for item in result['items']}
                assert expected <= labels and not excluded & labels, (version, labels)
            request(p, 100, 'shutdown', {})
            send(p, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
            assert p.wait(timeout=10) == 0
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()
    print('expression receivers, generic methods, tuples, arrays and lexical shadowing: ok')


if __name__ == '__main__':
    main()
