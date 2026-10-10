#!/usr/bin/env python3
"""F-string expressions retain physical identities, UTF-16 and token roles."""

from pathlib import Path
import subprocess
import sys
import tempfile
import threading

from protocol_features import request, send


def at(source, needle, name):
    offset = source.index(needle) + needle.index(name)
    prefix = source[:offset]
    return {'line': prefix.count('\n'),
            'character': len(prefix.rsplit('\n', 1)[-1].encode('utf-16-le')) // 2}


def main():
    source = ('fn value() -> bool { ret true }\n'
              'struct Item { value: u32 }\n'
              'impl for Item { fn read(self: Self) -> u32 { ret self.value } }\n'
              'fn sample(value: u32, item: Item) -> str {\n'
              ' let literal = "value item read"\n'
              ' let text = f"😀 Привет {value}, {item.value}, {item.read()}, {value + 1_u32}"\n'
              ' ret text\n}\n')
    with tempfile.TemporaryDirectory(prefix='encore-fstrings-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="strings"\nversion="0.0.0"\ndependencies=[]\n')
        path = root / 'src/main.enq'
        path.write_text(source)
        process = subprocess.Popen([str(Path(sys.argv[1]).resolve())], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        number = 0

        def call(method, params):
            nonlocal number
            number += 1
            timer = threading.Timer(30, process.kill)
            timer.start()
            try:
                return request(process, number, method, params)
            finally:
                timer.cancel()

        def query(method, needle, name, **extra):
            return call(method, {'textDocument': {'uri': path.as_uri()},
                                 'position': at(source, needle, name), **extra})

        def change(text, version):
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': version},
                'contentChanges': [{'text': text}]}})

        try:
            initialized = call('initialize', {'rootUri': root.as_uri(),
                'initializationOptions': {'jobs': 2, 'cache': False, 'debounceMs': 0}})
            legend = initialized['capabilities']['semanticTokensProvider']['legend']['tokenTypes']
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 1, 'text': source}}})
            for needle, name, target, target_name in [
                ('{value}', 'value', 'sample(value:', 'value'),
                ('{value +', 'value', 'sample(value:', 'value'),
                ('{item.value}', 'item', 'item: Item', 'item'),
                ('{item.value}', 'value', 'Item { value:', 'value'),
                ('{item.read()}', 'read', 'fn read(', 'read'),
            ]:
                definitions = query('textDocument/definition', needle, name)
                assert len(definitions) == 1 and definitions[0]['uri'] == path.as_uri(), (needle, definitions)
                assert definitions[0]['range']['start'] == at(source, target, target_name), (needle, definitions)
                hover = query('textDocument/hover', needle, name)
                assert hover and ('u32' if name != 'item' else 'Item') in str(hover), (needle, hover)
            completion = query('textDocument/completion', '{value}', 'value')
            local = next(item for item in completion['items'] if item['label'] == 'value')
            assert local['kind'] == 6 and local['detail'] == 'u32', local
            signature = query('textDocument/signatureHelp', '{item.read()}', ')')
            assert signature['signatures'][0]['label'] == 'read() -> u32', signature
            references = query('textDocument/references', '{value}', 'value',
                               context={'includeDeclaration': True})
            actual = {(item['range']['start']['line'], item['range']['start']['character']) for item in references}
            expected = {tuple(at(source, needle, 'value').values()) for needle in
                        ('sample(value:', '{value}', '{value +')}
            assert actual == expected, (actual, expected)
            edit = query('textDocument/rename', '{value}', 'value', newName='count')
            edits = edit['changes'][path.as_uri()]
            assert {(item['range']['start']['line'], item['range']['start']['character']) for item in edits} == expected, edit
            for needle, name in [('"value item', 'value'), ('Привет', 'Привет')]:
                assert not query('textDocument/definition', needle, name), needle
            params = {'textDocument': {'uri': path.as_uri()}}
            baseline = call('textDocument/semanticTokens/full', params)['data']
            roles = {}
            line = column = 0
            for index in range(0, len(baseline), 5):
                dl, dc, length, kind, modifiers = baseline[index:index + 5]
                line += dl
                column = dc if dl else column + dc
                roles[line, column] = (legend[kind], length, modifiers)
            for needle, name, role in [('{value}', 'value', 'parameter'),
                                       ('{item.value}', 'item', 'parameter'),
                                       ('{item.value}', 'value', 'property'),
                                       ('{item.read()}', 'read', 'method')]:
                pos = at(source, needle, name)
                assert roles[pos['line'], pos['character']] == (role, len(name), 0), roles
            # Full semantic mode must still color the literal text itself.
            literal_position = at(source, '😀 Привет', '😀')
            assert any(row == literal_position['line'] and start <= literal_position['character'] < start + length
                       and role == 'string' for (row, start), (role, length, _) in roles.items()), roles
            # No semantic string span may paint over an interpolation.
            for needle in ('{value}', '{item.value}', '{item.read()}', '{value +'):
                pos = at(source, needle, needle[1:].split('}')[0].split('.')[0].split()[0])
                for (row, start), (role, length, _) in roles.items():
                    assert not (row == pos['line'] and start <= pos['character'] < start + length and role == 'string'), roles
            assert call('textDocument/semanticTokens/full', params)['data'] == baseline
            # Multiple placeholders, multiline text, CRLF and astral Unicode
            # must preserve the parameter's original declaration position.
            multiline = source.replace('😀 Привет {value}', '😀 Привет\nnext {value}').replace('\n', '\r\n')
            change(multiline, 2)
            source = multiline
            assert query('textDocument/definition', '{value}', 'value')[0]['range']['start'] == at(source, 'sample(value:', 'value')
            assert 'u32' in str(query('textDocument/hover', '{value}', 'value'))
            assert call('textDocument/semanticTokens/full', params)['data'] != baseline
            change('fn main() { let text = f"unfinished {value', 3)
            assert call('textDocument/semanticTokens/full', params)['data'] != baseline
            source = 'fn sample(value: u32) -> str { ret f"😀 Привет {missing}" }\n'
            change(source, 4)
            report = call('textDocument/diagnostic', params)
            unknown = next(item for item in report['items'] if 'missing' in item['message'])
            assert unknown['range']['start'] == at(source, '{missing}', 'missing'), unknown
            call('shutdown', {})
            send(process, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
    print('f-string UTF-16 identities, members, references, rename, multiline tokens and recovery: ok')


if __name__ == '__main__':
    main()
