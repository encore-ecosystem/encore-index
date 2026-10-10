#!/usr/bin/env python3
"""Implementation lookup must distinguish nominal identities and import aliases."""

from pathlib import Path
import subprocess
import sys
import tempfile

from protocol_features import request, send
from member_navigation import position


def main():
    with tempfile.TemporaryDirectory(prefix='encore-lsp-impl-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text(
            '[project]\nname="implementations"\nversion="0.0.0"\ndependencies=[]\n')
        left = 'pub struct Item { value: u32 }\npub trait Read { fn read(self: Self) -> u32 }\n'
        right = 'pub struct Item { value: str }\n'
        implementation = ('import refrain::left::{Item as Left, Read}\n'
                          'import refrain::right::Item as Right\n'
                          'impl for Left { pub fn get(self: Self) -> u32 { ret self.value } }\n'
                          'impl for Right { pub fn get(self: Self) -> str { ret self.value } }\n'
                          'impl Read for Left { pub fn read(self: Self) -> u32 { ret self.value } }\n')
        source = ('import refrain::left::{Item as A, Read as Reader}\n'
                  'import refrain::right::Item as B\n'
                  'import refrain::implementation\n'
                  'fn sample(a: A, b: B, c: dyn Reader) { a.get() b.get() c.read() }\n')
        files = {'left.enq': left, 'right.enq': right, 'implementation.enq': implementation, 'main.enq': source}
        for name, text in files.items():
            (root / 'src' / name).write_text(text)
        process = subprocess.Popen([str(Path(sys.argv[1]).resolve())], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            request(process, 1, 'initialize', {'rootUri': root.as_uri()})
            for name, text in files.items():
                send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                    'textDocument': {'uri': (root / 'src' / name).as_uri(), 'version': 1, 'text': text}}})
            cases = [('sample(a: A', 'A', {2, 4}), ('b: B', 'B', {3}),
                     ('dyn Reader', 'Reader', {4}), ('c.read()', 'read', {4})]
            for number, (needle, name, expected) in enumerate(cases, 10):
                result = request(process, number, 'textDocument/implementation', {
                    'textDocument': {'uri': (root / 'src/main.enq').as_uri()},
                    'position': position(source, needle, name)})
                assert {item['range']['start']['line'] for item in result} == expected, (needle, result)
                assert all(item['uri'] == (root / 'src/implementation.enq').as_uri() for item in result), result
                assert len(result) == len(expected), result
            request(process, 30, 'shutdown', {})
            send(process, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print('nominal implementation lookup, aliases and trait method locations: ok')


if __name__ == '__main__':
    main()
