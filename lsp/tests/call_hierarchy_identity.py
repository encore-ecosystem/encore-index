#!/usr/bin/env python3
"""Call hierarchy groups physical declarations, not equally spelled names."""

from pathlib import Path
import subprocess
import sys
import tempfile

from protocol_features import request, send
from member_navigation import position
from scheduler import stop


def main():
    with tempfile.TemporaryDirectory(prefix='encore-lsp-call-identity-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="calls"\nversion="0.0.0"\ndependencies=[]\n')
        main = ('import refrain::left::run as first\nimport refrain::right::run as second\n'
                'struct Left {} struct Right {}\n'
                'impl for Left { fn caller(self: Self) -> u32 { ret first() } } '
                'impl for Right { fn caller(self: Self) -> u32 { ret second() } }\n'
                'fn caller() -> u32 { ret first() + first() + second() }\n')
        files = {'left': 'pub fn run() -> u32 { ret 1 }\n',
                 'right': 'pub fn run() -> u32 { ret 2 }\n', 'main': main}
        paths = {name: root / 'src' / f'{name}.enq' for name in files}
        for name, text in files.items():
            paths[name].write_text(text)
        process = subprocess.Popen([str(Path(sys.argv[1]).resolve())], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        number = 0

        def call(method, params):
            nonlocal number
            number += 1
            return request(process, number, method, params)

        def prepare(name, needle, symbol):
            result = call('textDocument/prepareCallHierarchy', {
                'textDocument': {'uri': paths[name].as_uri()},
                'position': position(files[name], needle, symbol)})
            assert len(result) == 1, result
            return result[0]

        try:
            call('initialize', {'rootUri': root.as_uri()})
            for name, text in files.items():
                send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                    'textDocument': {'uri': paths[name].as_uri(), 'version': 1, 'text': text}}})
            caller = prepare('main', 'fn caller()', 'caller')
            outgoing = call('callHierarchy/outgoingCalls', {'item': caller})
            assert {item['to']['uri']: len(item['fromRanges']) for item in outgoing} == {
                paths['left'].as_uri(): 2, paths['right'].as_uri(): 1}, outgoing
            first = prepare('main', 'ret first()', 'first')
            assert first['uri'] == paths['left'].as_uri(), first
            incoming = call('callHierarchy/incomingCalls', {'item': first})
            assert sorted(len(item['fromRanges']) for item in incoming) == [1, 2], incoming
            assert len({str(item['from']['selectionRange']) for item in incoming}) == 2, incoming
            right = prepare('right', 'fn run()', 'run')
            incoming = call('callHierarchy/incomingCalls', {'item': right})
            assert len(incoming) == 2 and all(len(item['fromRanges']) == 1 for item in incoming), incoming
            method = prepare('main', 'Right { fn caller', 'caller')
            outgoing = call('callHierarchy/outgoingCalls', {'item': method})
            assert len(outgoing) == 1 and outgoing[0]['to']['uri'] == paths['right'].as_uri(), outgoing
            stop(process)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print('call hierarchy aliases, distinct declarations, same-line methods and grouped ranges: ok')


if __name__ == '__main__':
    main()
