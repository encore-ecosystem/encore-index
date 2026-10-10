#!/usr/bin/env python3
"""Sequential ranged edits use UTF-16 and invalidate all dependent queries."""

from pathlib import Path
import subprocess
import sys
import tempfile
import threading

from protocol_features import request, send


def point(source, offset):
    prefix = source[:offset]
    return {'line': prefix.count('\n'),
            'character': len(prefix.rsplit('\n', 1)[-1].encode('utf-16-le')) // 2}


def main():
    original = ('fn sample() {\r\n let banner: str = "😀"\r\n'
                ' let value = 1_u32\r\n value\r\n}\r\n')
    with tempfile.TemporaryDirectory(prefix='encore-ranged-edits-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="edits"\nversion="0.0.0"\ndependencies=[]\n')
        path = root / 'src/main.enq'
        path.write_bytes(original.encode())
        process = subprocess.Popen([str(Path(sys.argv[1]).resolve())], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        number = 0
        source = original

        def call(method, params):
            nonlocal number
            number += 1
            timer = threading.Timer(30, process.kill)
            timer.start()
            try:
                return request(process, number, method, params)
            finally:
                timer.cancel()

        def changes(version, replacements):
            nonlocal source
            edits = []
            for needle, old, new in replacements:
                start = source.index(needle) + needle.index(old)
                end = start + len(old)
                edits.append({'range': {'start': point(source, start), 'end': point(source, end)},
                              'rangeLength': len(old.encode('utf-16-le')) // 2, 'text': new})
                source = source[:start] + new + source[end:]
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': version}, 'contentChanges': edits}})

        def check(name, typ):
            params = {'textDocument': {'uri': path.as_uri()},
                      'position': point(source, source.rindex(name))}
            hover = call('textDocument/hover', params)
            assert hover and typ in str(hover), (name, hover)
            definitions = call('textDocument/definition', params)
            declaration = point(source, source.index('let ' + name) + 4)
            assert definitions[0]['range']['start'] == declaration, definitions
            refs = call('textDocument/references', {**params, 'context': {'includeDeclaration': True}})
            assert len(refs) == 2, refs
            hints = call('textDocument/inlayHint', {'textDocument': {'uri': path.as_uri()}, 'range': {
                'start': {'line': 2, 'character': 0}, 'end': {'line': 3, 'character': 0}}})
            assert len(hints) == 1 and hints[0]['label'] == ': ' + typ, hints
            return call('textDocument/semanticTokens/full', {'textDocument': {'uri': path.as_uri()}})['data']

        try:
            call('initialize', {'rootUri': root.as_uri(), 'initializationOptions': {
                'jobs': 2, 'debounceMs': 0, 'cache': False}})
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 1, 'text': source}}})
            before = check('value', 'u32')
            changes(2, [('1_u32', '1_u32', '"text"')])
            assert check('value', 'str') != before
            changes(3, [('"😀"', '😀', '😀🌍'), ('let value', 'value', 'count'),
                        ('\n value\r', 'value', 'count')])
            changed = check('count', 'str')
            # A range after astral characters must not split a surrogate pair
            # or consume the closing quote/CRLF in the next edit.
            changes(4, [('"😀🌍"', '🌍', 'Привет'), ('"text"', '"text"', 'false')])
            assert check('count', 'bool') != changed
            assert path.read_bytes().decode() == original
            call('shutdown', {})
            send(process, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
    print('sequential ranged UTF-16 edits, CRLF, inference, navigation, references, inlays and tokens: ok')


if __name__ == '__main__':
    main()
