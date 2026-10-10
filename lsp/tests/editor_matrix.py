#!/usr/bin/env python3
"""Twenty independently isolated editor regressions over the real LSP protocol."""

from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest

from protocol_features import request, send
from incremental_unicode import point


BINARY = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None


class EditorMatrix(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix='encore-editor-matrix-')
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        (self.root / 'src').mkdir()
        (self.root / 'encore.toml').write_text(
            '[project]\nname="matrix"\nversion="0.0.0"\ndependencies=[]\n')
        self.path = self.root / 'src/main.enq'
        self.process = subprocess.Popen([str(BINARY)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.cleanup_process)
        self.sequence = 0
        self.version = 0
        self.call('initialize', {'rootUri': self.root.as_uri(), 'initializationOptions': {
            'jobs': 2, 'debounceMs': 0, 'cache': False}})

    def cleanup_process(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait()
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()

    def call(self, method, params):
        self.sequence += 1
        timer = threading.Timer(30, self.process.kill)
        timer.start()
        try:
            return request(self.process, self.sequence, method, params)
        finally:
            timer.cancel()

    def open(self, source):
        self.source = source
        self.path.write_text(source)
        self.version = 1
        send(self.process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
            'textDocument': {'uri': self.path.as_uri(), 'version': 1,
                             'languageId': 'encore', 'text': source}}})

    def change(self, source):
        self.source = source
        self.version += 1
        send(self.process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
            'textDocument': {'uri': self.path.as_uri(), 'version': self.version},
            'contentChanges': [{'text': source}]}})

    def at(self, marker):
        return {'textDocument': {'uri': self.path.as_uri()},
                'position': point(self.source, self.source.rindex(marker))}

    def definition(self, marker, declaration):
        result = self.call('textDocument/definition', self.at(marker))
        self.assertTrue(result, marker)
        self.assertEqual(result[0]['uri'], self.path.as_uri())
        self.assertEqual(result[0]['range']['start'], point(self.source, declaration))

    def test_01_parameter_shadows_function(self):
        self.open('fn item() -> u32 { ret 0 }\nfn run(item: u32) -> u32 { ret item }\n')
        self.definition('item', self.source.index('item:'))

    def test_02_initializer_reads_outer_binding(self):
        self.open('fn run(item: u32) -> u32 { let item = item ret item }\n')
        result = self.call('textDocument/definition', {
            **self.at('item'), 'position': point(self.source, self.source.index('= item') + 2)})
        self.assertEqual(result[0]['range']['start'], point(self.source, self.source.index('item:')))

    def test_03_inner_block_does_not_escape(self):
        self.open('fn run(item: u32) -> u32 { if true { let item = 2 item } ret item }\n')
        self.definition('item', self.source.index('item:'))

    def test_04_separate_functions_keep_identity(self):
        self.open('fn first(item: u32) -> u32 { ret item }\nfn second(item: str) -> str { ret item }\n')
        self.definition('item', self.source.index('item: str'))

    def test_05_comment_is_not_reference(self):
        self.open('fn run(item: u32) -> u32 {\n // item\n ret item\n}\n')
        refs = self.call('textDocument/references', {**self.at('item'), 'context': {'includeDeclaration': True}})
        self.assertEqual(len(refs), 2)

    def test_06_plain_string_is_not_reference(self):
        self.open('fn run(item: u32) -> u32 { let text = "item" ret item }\n')
        refs = self.call('textDocument/references', {**self.at('item'), 'context': {'includeDeclaration': False}})
        self.assertEqual(len(refs), 1)

    def test_07_fstring_multiple_references(self):
        self.open('fn run(item: u32) { let text = f"{item} / {item}" }\n')
        refs = self.call('textDocument/references', {**self.at('item'), 'context': {'includeDeclaration': True}})
        self.assertEqual(len(refs), 3)

    def test_08_crlf_definition(self):
        self.open('fn run(item: u32) -> u32 {\r\n ret item\r\n}\r\n')
        self.definition('item', self.source.index('item:'))

    def test_09_astral_fstring_definition(self):
        self.open('fn run(item: u32) { let text = f"😀🌍 {item}" }\n')
        self.definition('item', self.source.index('item:'))

    def test_10_multiline_fstring_definition(self):
        self.open('fn run(item: u32) { let text = f"first\nsecond {item}" }\n')
        self.definition('item', self.source.index('item:'))

    def test_11_unicode_prefix_rename_ranges(self):
        self.open('fn run(item: u32) { let text = f"😀 {item}" }\n')
        edits = self.call('textDocument/rename', {**self.at('item'), 'newName': 'count'})
        changes = edits['changes'][self.path.as_uri()]
        self.assertEqual(len(changes), 2)
        self.assertEqual({e['newText'] for e in changes}, {'count'})
        self.assertIn(point(self.source, self.source.rindex('item')), [e['range']['start'] for e in changes])

    def test_12_unsaved_type_change(self):
        self.open('fn run() { let item = 1_u32 item }\n')
        self.assertIn('u32', str(self.call('textDocument/hover', self.at('item'))))
        self.change('fn run() { let item = "hello" item }\n')
        self.assertIn('str', str(self.call('textDocument/hover', self.at('item'))))
        self.assertIn('1_u32', self.path.read_text())

    def test_13_unsaved_declaration_move(self):
        self.open('fn run(item: u32) -> u32 { ret item }\n')
        self.definition('item', self.source.index('item:'))
        self.change('\n\n' + self.source)
        self.definition('item', self.source.index('item:'))
        symbols = self.call('textDocument/documentSymbol', {
            'textDocument': {'uri': self.path.as_uri()}})
        self.assertEqual([s['name'] for s in symbols], ['run'])
        self.assertEqual(symbols[0]['selectionRange']['start']['line'], 2)

    def test_14_removed_symbol_loses_definition(self):
        self.open('fn value() -> u32 { ret 1 }\nfn run() { value() }\n')
        self.definition('value', self.source.index('value'))
        self.change('fn run() { value() }\n')
        self.assertFalse(self.call('textDocument/definition', self.at('value')))

    def test_15_added_symbol_recovers_definition(self):
        self.open('fn run() { value() }\n')
        self.assertFalse(self.call('textDocument/definition', self.at('value')))
        self.change('fn value() -> u32 { ret 1 }\n' + self.source)
        self.definition('value', self.source.index('value'))

    def test_16_explicit_type_has_no_inlay(self):
        self.open('fn run() { let item: u32 = 1 item }\n')
        hints = self.call('textDocument/inlayHint', {'textDocument': {'uri': self.path.as_uri()},
            'range': {'start': {'line': 0, 'character': 0}, 'end': {'line': 1, 'character': 0}}})
        self.assertEqual(hints, [])
        self.change('fn run() { let item: us }\n')
        params = self.at('us')
        params['position']['character'] += 2
        completion = self.call('textDocument/completion', params)
        self.assertIn('usize', [item['label'] for item in completion['items']])

    def test_17_inferred_boolean_inlay(self):
        self.open('fn run() { let item = true item }\n')
        hints = self.call('textDocument/inlayHint', {'textDocument': {'uri': self.path.as_uri()},
            'range': {'start': {'line': 0, 'character': 0}, 'end': {'line': 1, 'character': 0}}})
        self.assertEqual([h['label'] for h in hints], [': bool'])

    def test_18_repeated_semantic_tokens_stable(self):
        self.open('fn add(left: u32, right: u32) -> u32 { ret left + right }\n'
                  'fn run(item: u32) { let text = f"😀 {add(item, 1_u32)}" }\n')
        params = {'textDocument': {'uri': self.path.as_uri()}}
        first = self.call('textDocument/semanticTokens/full', params)
        self.assertTrue(first['data'])
        self.assertEqual(first, self.call('textDocument/semanticTokens/full', params))
        signature = self.call('textDocument/signatureHelp', self.at('1_u32'))
        self.assertEqual(signature['signatures'][0]['label'],
                         'add(left: u32, right: u32) -> u32')

    def test_19_empty_document_recovery(self):
        self.open('')
        self.assertEqual(self.call('textDocument/semanticTokens/full', {
            'textDocument': {'uri': self.path.as_uri()}})['data'], [])
        self.change('fn run(item: u32) -> u32 { ret item }\n')
        self.definition('item', self.source.index('item:'))

    def test_20_reopen_discards_old_overlay(self):
        original = 'fn run(item: u32) -> u32 { ret item }\n'
        self.open(original)
        self.change('fn run(item: str) -> str { ret item }\n')
        self.assertIn('str', str(self.call('textDocument/hover', self.at('item'))))
        send(self.process, {'jsonrpc': '2.0', 'method': 'textDocument/didClose',
            'params': {'textDocument': {'uri': self.path.as_uri()}}})
        self.open(original)
        self.assertIn('u32', str(self.call('textDocument/hover', self.at('item'))))


if __name__ == '__main__':
    if BINARY is None:
        raise SystemExit('usage: editor_matrix.py <lsp-binary> [unittest options]')
    unittest.main(argv=[sys.argv[0], *sys.argv[2:]], verbosity=2)
