#!/usr/bin/env python3
"""Member hover retains declaration docs and receiver-specialized signatures."""

from pathlib import Path
import subprocess
import re
import sys
import tempfile

from protocol_features import request, send
from member_navigation import position


def main():
    source = (
        'struct Box[T] {\n /// Stored payload.\n value: T\n}\n'
        'impl[T] for Box[T] {\n /// Reads a matching payload.\n fn read(self: Self, fallback: T) -> T { ret self.value }\n}\n'
        'trait Parent[T] {\n /// Converts the supplied value.\n fn convert(self: Self, input: T) -> T\n}\n'
        'trait Child < Parent[str] {}\n'
        'fn read(unrelated: bool) -> bool { ret unrelated }\n'
        'fn sample(a: Box[str], b: Box[u32], c: dyn Child) {\n'
        ' a.value\n b.value\n a.read("x")\n b.read(1_u32)\n c.convert("x")\n}\n'
        'fn generic[T: Child](item: T) { item.convert("x") }\n'
    )
    if '--long-generics' in sys.argv[2:]:
        source = re.sub(r'\bT\b', 'Element', source)
    with tempfile.TemporaryDirectory(prefix='encore-lsp-member-details-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="details"\nversion="0.0.0"\ndependencies=[]\n')
        path = root / 'src/main.enq'
        path.write_text(source)
        process = subprocess.Popen([str(Path(sys.argv[1]).resolve())], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            request(process, 1, 'initialize', {'rootUri': root.as_uri()})
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': path.as_uri(), 'languageId': 'encore', 'version': 1, 'text': source}}})
            for number, needle, name, signature, documentation in [
                (10, 'a.value', 'value', 'value: str', 'Stored payload.'),
                (11, 'b.value', 'value', 'value: u32', 'Stored payload.'),
                (12, 'a.read', 'read', 'fn read(fallback: str) -> str', 'Reads a matching payload.'),
                (13, 'b.read', 'read', 'fn read(fallback: u32) -> u32', 'Reads a matching payload.'),
                (14, 'c.convert', 'convert', 'fn convert(input: str) -> str', 'Converts the supplied value.'),
                (15, 'item.convert', 'convert', 'fn convert(input: str) -> str', 'Converts the supplied value.'),
            ]:
                result = request(process, number, 'textDocument/hover', {
                    'textDocument': {'uri': path.as_uri()}, 'position': position(source, needle, name)})
                assert result and signature in result['contents']['value'], (needle, result)
                assert documentation in result['contents']['value'], (needle, result)
            for number, needle, argument, expected in [
                (30, 'a.read("x")', '"x"', 'read(fallback: str) -> str'),
                (31, 'b.read(1_u32)', '1_u32', 'read(fallback: u32) -> u32'),
                (32, 'c.convert("x")', '"x"', 'convert(input: str) -> str'),
                (33, 'item.convert("x")', '"x"', 'convert(input: str) -> str'),
            ]:
                result = request(process, number, 'textDocument/signatureHelp', {
                    'textDocument': {'uri': path.as_uri()}, 'position': position(source, needle, argument)})
                assert result['signatures'] and result['signatures'][0]['label'] == expected, (needle, result)
                assert result['activeParameter'] == 0, result
            updated = source.replace('Stored payload.', 'Updated payload documentation.')
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 2}, 'contentChanges': [{'text': updated}]}})
            result = request(process, 20, 'textDocument/hover', {
                'textDocument': {'uri': path.as_uri()}, 'position': position(updated, 'a.value', 'value')})
            assert 'Updated payload documentation.' in result['contents']['value'], result
            header = source[:source.index('fn sample')]
            for version, tail in enumerate(['a.read(', 'a.read("x", ', 'a.read(1 +'], 3):
                incomplete = header + 'fn editing(a: Box[str]) {\n ' + tail
                send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                    'textDocument': {'uri': path.as_uri(), 'version': version},
                    'contentChanges': [{'text': incomplete}]}})
                result = request(process, 40 + version, 'textDocument/signatureHelp', {
                    'textDocument': {'uri': path.as_uri()}, 'position': {
                        'line': incomplete.count('\n'), 'character': len(incomplete.rsplit('\n', 1)[-1])}})
                assert result['signatures'] and result['signatures'][0]['label'] == 'read(fallback: str) -> str', (tail, result)
                assert result['activeParameter'] == 0, result
            request(process, 100, 'shutdown', {})
            send(process, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print('specialized member signatures and declaration documentation: ok')


if __name__ == '__main__':
    main()
