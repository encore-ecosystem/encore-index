#!/usr/bin/env python3
"""Inlay types share semantic inference, scopes, dependency edits and recovery."""

from pathlib import Path
import subprocess
import sys
import tempfile
import threading

from protocol_features import request, send


def main():
    library = 'pub struct Box[Element] { value: Element }\nimpl[Element] for Box[Element] { pub fn read(self: Self) -> Element { ret self.value } }\npub fn make() -> str { ret "x" }\n'
    library += 'impl[Element] for Box[Element] { pub fn new(value: Element) -> Self { ret Self { value } } pub fn label() -> str { ret "box" } }\n'
    source = '''import refrain::api::{Box, make}
fn sample(item: Box[str]) {
 let outer = 1_u32
 if true { let outer = "inside" }
 let copy = outer
 let field = item.value
 let method = item.read()
 let compare = 1_u32 < 2_u32
 let imported = make()
 let mut mutable = true
 let explicit: u32 = 1_u32
}
fn other() { let missing = outer }
fn unicode() { let emoji = "😀" let width = 1_u16 }
fn associated() { let boxed = Box[u32]::new(1_u32) let label = Box[u32]::label() let read = boxed.read() }
'''
    with tempfile.TemporaryDirectory(prefix='encore-lsp-inlays-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="inlays"\nversion="0.0.0"\ndependencies=[]\n')
        path = root / 'src/main.enq'
        model = root / 'src/model.enq'
        path.write_text(source)
        model.write_text(library)
        (root / 'src/api.enq').write_text('pub import refrain::model::{Box, make}\n')
        process = subprocess.Popen([str(Path(sys.argv[1]).resolve())], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        number = 0

        def call(method, params):
            nonlocal number
            number += 1
            deadline = threading.Timer(20, process.kill)
            deadline.start()
            try:
                return request(process, number, method, params)
            finally:
                deadline.cancel()

        def open_document(file, text, version=1):
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': file.as_uri(), 'languageId': 'encore', 'version': version, 'text': text}}})

        def hints(start=0, end=100, start_column=0, end_column=0):
            return call('textDocument/inlayHint', {'textDocument': {'uri': path.as_uri()},
                'range': {'start': {'line': start, 'character': start_column},
                          'end': {'line': end, 'character': end_column}}})

        def check(items, line, name, typ):
            text = source.splitlines()[line]
            column = len(text[:text.index(name) + len(name)].encode('utf-16-le')) // 2
            matching = [item for item in items if item['position'] == {'line': line, 'character': column}]
            assert len(matching) == 1 and matching[0]['label'] == ': ' + typ, (line, name, typ, matching, items)

        try:
            call('initialize', {'rootUri': root.as_uri()})
            open_document(path, source)
            items = hints()
            for line, name, typ in [(2, 'outer', 'u32'), (3, 'outer', 'str'), (4, 'copy', 'u32'),
                                    (5, 'field', 'str'), (6, 'method', 'str'), (7, 'compare', 'bool'),
                                    (8, 'imported', 'str'), (9, 'mutable', 'bool'), (13, 'width', 'u16'),
                                    (14, 'boxed', 'Box[u32]'), (14, 'label', 'str'), (14, 'read', 'u32')]:
                check(items, line, name, typ)
            assert not any(item['position']['line'] in (10, 12) for item in items), items
            assert hints() == items, 'warm queries changed their result'
            narrow = hints(5, 7)
            assert narrow and all(5 <= item['position']['line'] < 7 for item in narrow), narrow
            assert hints(4, 4, 0, 9) == [], 'end position must be exclusive'
            check(hints(4, 4, 9, 10), 4, 'copy', 'u32')
            assert hints(4, 4, 10, 20) == [], 'start position must be respected'
            # Viewport chunks may overlap and arrive repeatedly. Each response
            # must remain a pure, range-filtered snapshot, not accumulated hints.
            for start, end in [(0, 8), (4, 15), (0, 15), (4, 15), (15, 15), (8, 4)]:
                ranged = hints(start, end)
                expected = [item for item in items if start <= item['position']['line'] < end]
                assert ranged == expected, (start, end, ranged, expected)
            for _ in range(3):
                send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didClose', 'params': {
                    'textDocument': {'uri': path.as_uri()}}})
                open_document(path, source)
                assert hints() == items, 'reopening a document changed or duplicated hints'
            open_document(model, library.replace('make() -> str { ret "x" }', 'make() -> bool { ret true }'))
            check(hints(), 8, 'imported', 'bool')
            # An incomplete subsequent function must not erase existing hints.
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 2},
                'contentChanges': [{'text': source + 'fn unfinished() { let pending ='}]}})
            check(hints(), 6, 'method', 'str')
            # Adding an explicit annotation removes the old inferred hint;
            # restoring the declaration must bring back exactly one hint.
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 3},
                'contentChanges': [{'text': source.replace('let copy =', 'let copy: u32 =')}]}})
            assert not any(item['position']['line'] == 4 for item in hints())
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 4},
                'contentChanges': [{'text': source}]}})
            check(hints(), 4, 'copy', 'u32')
            call('shutdown', {})
            send(process, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print('semantic inlay types, lexical scopes, dependency overlays and recovery: ok')


if __name__ == '__main__':
    main()
