#!/usr/bin/env python3
"""Highlight local identities, not the first same-name module declaration."""

from pathlib import Path
import subprocess
import sys
import tempfile

from protocol_features import request, send
from scheduler import stop


def main():
    source = ('fn value() -> u32 { ret 1 }\n'
              'struct Thing { value: u32 }\n'
              'fn sample(value: u32) -> u32 {\n'
              ' let Thing = value\n'
              ' let value = value\n'
              ' if true { let value = 2 value }\n'
              ' ret value + Thing\n'
              '}\n'
              'impl for Thing { fn read(self: Self) -> u32 { ret self.value } }\n'
              'fn read() -> u32 { ret 0 }\n')
    with tempfile.TemporaryDirectory(prefix='encore-token-scopes-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="scopes"\nversion="0.0.0"\ndependencies=[]\n')
        path = root / 'src/main.enq'
        path.write_text(source)
        process = subprocess.Popen([str(Path(sys.argv[1]).resolve())], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            initialized = request(process, 1, 'initialize', {'rootUri': root.as_uri()})
            legend = initialized['capabilities']['semanticTokensProvider']['legend']['tokenTypes']
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 1, 'text': source}}})
            tokens = request(process, 2, 'textDocument/semanticTokens/full', {
                'textDocument': {'uri': path.as_uri()}})['data']
            roles = {}
            line = column = 0
            for index in range(0, len(tokens), 5):
                delta_line, delta_column, length, kind, modifiers = tokens[index:index + 5]
                line += delta_line
                column = delta_column if delta_line else column + delta_column
                roles[line, column] = (legend[kind], modifiers)
            lines = source.splitlines()
            cases = [(0, 'value', 'function', 3), (2, 'value', 'parameter', 3),
                     (3, 'Thing', 'variable', 3), (3, 'value', 'parameter', 0),
                     (4, 'value', 'variable', 3), (6, 'value', 'variable', 0),
                     (6, 'Thing', 'variable', 0), (8, 'read', 'function', 3),
                     (9, 'read', 'function', 3)]
            for line, name, expected, modifiers in cases:
                column = lines[line].index(name)
                assert roles.get((line, column)) == (expected, modifiers), (line, name, roles.get((line, column)))
            # The initializer of a new shadowing local still reads the parameter.
            assert roles[4, lines[4].rindex('value')] == ('parameter', 0), roles
            assert roles[5, lines[5].rindex('value')] == ('variable', 0), roles
            stop(process)
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
    print('scoped parameters, locals, initializers and repeated declaration modifiers: ok')


if __name__ == '__main__':
    main()
