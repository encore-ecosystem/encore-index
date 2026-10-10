#!/usr/bin/env python3
"""Relative wildcard imports resolve without manifest dependencies."""
from pathlib import Path
import sys
import subprocess
import unittest
import editor_matrix
from editor_matrix import EditorMatrix


class RelativeWildcard(EditorMatrix):
    def test_relative_imports_and_format(self):
        helper = self.root / 'src/helper.enq'
        helper.write_text('pub fn answer() -> u32 { ret 42 }\n')
        for source in ('pub import mod::helper:: *\n',
                       'import mod::helper::answer\n',
                       'pub import mod::helper::{answer}\n'):
            if self.version:
                self.change(source)
            else:
                self.open(source)
            diagnostics = self.call('textDocument/diagnostic', {
                'textDocument': {'uri': self.path.as_uri()}})
            self.assertFalse([d for d in diagnostics['items']
                              if d.get('code') == 'unresolved-import'], diagnostics)
        (self.root / 'src/lib.enq').write_text('pub fn unrelated() {}\n')
        self.change('pub import mod::missing::*\n')
        diagnostics = self.call('textDocument/diagnostic', {
            'textDocument': {'uri': self.path.as_uri()}})
        self.assertTrue(any(d.get('code') == 'unresolved-import' for d in diagnostics['items']))
        self.change('pub import mod::helper:: *\nfn multiply(a:u32,b:u32)->u32{ret a*b}\n')
        edits = self.call('textDocument/formatting', {
            'textDocument': {'uri': self.path.as_uri()},
            'options': {'tabSize': 4, 'insertSpaces': True}})
        text = edits[0]['newText']
        self.assertIn('pub import mod::helper::*\n', text)
        self.assertIn('ret a * b', text)
        if len(sys.argv) > 2:
            fixture = self.root / 'formatter.enq'
            fixture.write_text(self.source)
            subprocess.run([str(Path(sys.argv[2]).resolve()), 'format', str(fixture)],
                           check=True, timeout=30, capture_output=True)
            self.assertEqual(fixture.read_text(), text)
        self.change(text)
        self.assertFalse(self.call('textDocument/formatting', {
            'textDocument': {'uri': self.path.as_uri()},
            'options': {'tabSize': 4, 'insertSpaces': True}}))


if __name__ == '__main__':
    editor_matrix.BINARY = Path(sys.argv[1]).resolve()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite([
        RelativeWildcard('test_relative_imports_and_format')]))
    raise SystemExit(not result.wasSuccessful())
