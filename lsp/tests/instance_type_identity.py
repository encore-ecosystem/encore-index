#!/usr/bin/env python3
"""Instance receiver facts retain type origins through aliases and call results."""

from pathlib import Path
import subprocess
import sys
import tempfile
import threading

from protocol_features import request, send


def main():
    with tempfile.TemporaryDirectory(prefix='encore-lsp-instance-identity-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="instances"\nversion="0.0.0"\ndependencies=[]\n')
        for name, typ, value in [('left', 'u32', '1_u32'), ('right', 'str', '"right"')]:
            (root / f'src/{name}.enq').write_text(
                f'pub struct Item {{ value: {typ} }}\n'
                f'impl for Item {{ pub fn read(self: Self) -> {typ} {{ ret self.value }} }}\n'
                f'pub fn make() -> Item {{ ret Item {{ {value} }} }}\n')
        source = ('import refrain::left::{Item as Left, make as make_left}\n'
                  'import refrain::right::{Item as Right, make as make_right}\n'
                  'fn sample(left: Left, right: Right) {\n'
                  ' let a = left.read()\n let b = right.read()\n'
                  ' let c = make_left().read()\n let d = make_right().read()\n}\n')
        path = root / 'src/main.enq'
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

        try:
            call('initialize', {'rootUri': root.as_uri()})
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 1, 'text': source}}})
            failures = []
            for line, origin, typ in [(3, 'left', 'u32'), (4, 'right', 'str'),
                                       (5, 'left', 'u32'), (6, 'right', 'str')]:
                column = source.splitlines()[line].index('.read') + 1
                params = {'textDocument': {'uri': path.as_uri()}, 'position': {'line': line, 'character': column}}
                definitions = call('textDocument/definition', params)
                expected = (root / f'src/{origin}.enq').as_uri()
                if len(definitions) != 1 or definitions[0]['uri'] != expected:
                    failures.append(('definition', line, definitions))
                hover = call('textDocument/hover', params)
                if f'-> {typ}' not in str(hover):
                    failures.append(('hover', line, hover))
            items = call('textDocument/inlayHint', {'textDocument': {'uri': path.as_uri()},
                'range': {'start': {'line': 0, 'character': 0}, 'end': {'line': 10, 'character': 0}}})
            if {item['position']['line']: item['label'] for item in items} != {
                3: ': u32', 4: ': str', 5: ': u32', 6: ': str'}:
                failures.append(('inlay', items))
            assert not failures, failures
            # Import-only edits leave the function body text unchanged. Cached
            # parameter identities and factory signatures must both refresh.
            redirected = source.replace('refrain::left::', 'refrain::right::')
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 2},
                'contentChanges': [{'text': redirected}]}})
            for line in (3, 4, 5, 6):
                column = redirected.splitlines()[line].index('.read') + 1
                params = {'textDocument': {'uri': path.as_uri()},
                          'position': {'line': line, 'character': column}}
                definitions = call('textDocument/definition', params)
                assert len(definitions) == 1 and definitions[0]['uri'] == (root / 'src/right.enq').as_uri(), definitions
                assert '-> str' in str(call('textDocument/hover', params))
            redirected_hints = call('textDocument/inlayHint', {'textDocument': {'uri': path.as_uri()},
                'range': {'start': {'line': 0, 'character': 0}, 'end': {'line': 10, 'character': 0}}})
            assert {item['position']['line']: item['label'] for item in redirected_hints} == {
                3: ': str', 4: ': str', 5: ': str', 6: ': str'}, redirected_hints
            # The language lexer currently accepts ASCII identifiers. Exercise
            # supported Unicode source text, without inventing identifier syntax.
            unicode_source = source.replace(' let a =', ' let label = "Пример 😀"\n let a =')
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 3},
                'contentChanges': [{'text': unicode_source}]}})
            unicode_hints = call('textDocument/inlayHint', {'textDocument': {'uri': path.as_uri()},
                'range': {'start': {'line': 0, 'character': 0}, 'end': {'line': 10, 'character': 0}}})
            assert {item['position']['line']: item['label'] for item in unicode_hints} == {
                3: ': str', 4: ': u32', 5: ': str', 6: ': u32', 7: ': str'}, unicode_hints
            call('shutdown', {})
            send(process, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print('instance aliases, return-type provenance and typed hints: ok')


if __name__ == '__main__':
    main()
