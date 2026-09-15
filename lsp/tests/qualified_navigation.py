#!/usr/bin/env python3
"""Navigation follows the selected path, including on a cold workspace."""

from pathlib import Path
import subprocess
import sys
import tempfile
from urllib.parse import unquote, urlparse

from protocol_features import request, send


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="encore-lsp-navigation-") as directory:
        root = Path(directory)
        (root / "src").mkdir()
        (root / "encore.toml").write_text(
            '[project]\nname="navigation"\nversion="0.0.0"\ndependencies=[]\n')
        path = root / "src/main.enq"
        uri = path.as_uri()
        path.write_text('fn main() -> u32 { ret 0 }\n')
        process = subprocess.Popen([str(binary)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            request(process, 1, "initialize", {"rootUri": root.as_uri()})
            send(process, {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "languageId": "encore", "version": 1, "text": path.read_text()},
            }})
            for version, (imported, qualifier) in enumerate([
                ('std::io', 'io'), ('std::io as console', 'console'),
            ], 2):
                line = f'    {qualifier}::println("hello")'
                source = f'import {imported}\nfn main() -> u32 {{\n{line}\n ret 0\n}}\n'
                source += 'fn println() -> u32 { ret 42 }\n'
                send(process, {"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                    "textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": source}],
                }})
                params = {"textDocument": {"uri": uri}, "position": {
                    "line": 2, "character": line.index('println') + 2,
                }}
                locations = request(process, version * 10, "textDocument/definition", params)
                assert len(locations) == 1, (source, locations)
                target = Path(unquote(urlparse(locations[0]["uri"]).path))
                assert target.name == 'mod.enq' and target.parent.name == 'io', locations
                start = locations[0]['range']['start']
                declared = target.read_text().splitlines()[start['line']]
                assert declared[start['character']:].startswith('println'), (declared, locations)
                hover = request(process, version * 10 + 1, "textDocument/hover", params)
                assert hover and 'println' in str(hover), hover
            # Unqualified aliases use the same declaration identity. A file
            # elsewhere in the project must not make an unimported name visible.
            (root / 'src/other.enq').write_text('pub fn unrelated() -> u32 { ret 1 }\n')
            for version, imported, name in [
                (4, 'import std::io::println as write_line\n', 'write_line'),
                (5, 'import std::io::*\n', 'println'),
                (6, '', 'unrelated'),
            ]:
                line = f'fn main() -> u32 {{ {name}("hello") ret 0 }}'
                source = imported + line + '\n'
                send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                    'textDocument': {'uri': uri, 'version': version},
                    'contentChanges': [{'text': source}],
                }})
                params = {'textDocument': {'uri': uri}, 'position': {
                    'line': imported.count('\n'), 'character': line.index(name) + 2,
                }}
                locations = request(process, version * 10, 'textDocument/definition', params)
                if name == 'unrelated':
                    assert locations == [], locations
                else:
                    assert len(locations) == 1, (source, locations)
                    target = Path(unquote(urlparse(locations[0]['uri']).path))
                    start = locations[0]['range']['start']
                    declared = target.read_text().splitlines()[start['line']]
                    assert declared[start['character']:].startswith('println'), locations
            request(process, 100, "shutdown", {})
            send(process, {"jsonrpc": "2.0", "method": "exit", "params": {}})
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print('qualified and aliased navigation to exact library declaration: ok')


if __name__ == '__main__':
    main()
