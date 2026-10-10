#!/usr/bin/env python3
"""Every import word navigates to its own module or exact declaration."""

from pathlib import Path
import sys
from editor_matrix import EditorMatrix
import unittest
import editor_matrix


class ImportSegments(EditorMatrix):
    def test_segments(self):
        frontend = self.root / 'src/frontend'
        frontend.mkdir()
        (frontend / 'mod.enq').write_text('//! Frontend.\n')
        syntax = frontend / 'syntax.enq'
        syntax.write_text('//! Syntax.\n\npub struct SyntaxDocument { value: u32 }\n')
        for source in (
            'import refrain::frontend::syntax::SyntaxDocument\n',
            'import refrain::frontend::{syntax::SyntaxDocument}\n',
            'import refrain::{frontend::{syntax::{SyntaxDocument}}}\n',
            'import refrain::frontend::syntax::SyntaxDocument as Document\n',
            'import refrain::frontend::{\n syntax::{\n SyntaxDocument,\n },\n}\n',
            '//! 😀 Unicode header.\r\nimport refrain::frontend::{syntax::SyntaxDocument}\r\n',
        ):
            if self.version:
                self.change(source)
            else:
                self.open(source)
            for word, path, line, column in (
                ('frontend', frontend / 'mod.enq', 0, 0),
                ('syntax', syntax, 0, 0),
                ('SyntaxDocument', syntax, 2, 11),
            ):
                with self.subTest(source=source, word=word):
                    for offset in (0, len(word) - 1):
                        params = self.at(word)
                        params['position']['character'] += offset
                        locations = self.call('textDocument/definition', params)
                        self.assertEqual(len(locations), 1, locations)
                        self.assertEqual(locations[0]['uri'], path.as_uri())
                        self.assertEqual(locations[0]['range']['start'],
                                         {'line': line, 'character': column})
            links = self.call('textDocument/documentLink', {
                'textDocument': {'uri': self.path.as_uri()}})
            self.assertTrue(links)
            lines = source.splitlines()
            seen = []
            for link in links:
                start, end = link['range']['start'], link['range']['end']
                self.assertEqual(start['line'], end['line'])
                word = lines[start['line']][start['character']:end['character']]
                self.assertIn(word, ('frontend', 'syntax', 'SyntaxDocument', 'Document'))
                expected = frontend / 'mod.enq' if word == 'frontend' else syntax
                self.assertEqual(link['target'], expected.as_uri())
                seen.append(word)
            for word in ('frontend', 'syntax', 'SyntaxDocument'):
                self.assertEqual(seen.count(word), 1, links)
        self.change('import refrain::frontend::missing::Unknown\n')
        for word in ('missing', 'Unknown'):
            self.assertFalse(self.call('textDocument/definition', self.at(word)))
        links = self.call('textDocument/documentLink', {
            'textDocument': {'uri': self.path.as_uri()}})
        self.assertEqual(len(links), 1, links)

    def test_targetspec_word_range(self):
        target = self.root / 'src/target.enq'
        target.write_text('pub struct TargetSpec { value: u32 }\n'
                          'pub fn host_target() {}\npub fn cfg_active_with_features() {}\n')
        source = 'import refrain::target::{TargetSpec, host_target, cfg_active_with_features}\n'
        self.open(source)
        links = self.call('textDocument/documentLink', {
            'textDocument': {'uri': self.path.as_uri()}})
        words = []
        for link in links:
            start, end = link['range']['start'], link['range']['end']
            self.assertEqual(start['line'], 0)
            self.assertEqual(end['line'], 0)
            words.append(source[start['character']:end['character']])
            self.assertEqual(link['target'], target.as_uri())
        self.assertEqual(words, ['target', 'TargetSpec', 'host_target', 'cfg_active_with_features'])


if __name__ == '__main__':
    editor_matrix.BINARY = Path(sys.argv[1]).resolve()
    # Do not rerun inherited matrix cases here.
    suite = unittest.TestSuite([ImportSegments('test_segments'),
                               ImportSegments('test_targetspec_word_range')])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
