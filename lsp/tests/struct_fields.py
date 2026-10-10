#!/usr/bin/env python3
"""Named declaration fields have individual lines; other colons stay intact."""
from pathlib import Path
import sys
import unittest
import subprocess
import editor_matrix
from editor_matrix import EditorMatrix


class StructFields(EditorMatrix):
    def test_fields(self):
        self.open('pub struct ModuleId { raw: u64 key: str }\n'
                  'pub struct SymbolId { raw: u64 key: str }\n'
                  'struct Generic[T] { first: (u32, str) second: T }\n'
                  'fn add(a:u32,b:u32)->u32{ret a+b}\n')
        params = {'textDocument': {'uri': self.path.as_uri()},
                  'options': {'tabSize': 4, 'insertSpaces': True}}
        text = self.call('textDocument/formatting', params)[0]['newText']
        self.assertEqual(text.count('    raw: u64\n    key: str\n'), 2)
        self.assertIn('\n    second: T\n', text)
        self.assertIn('fn add(a: u32, b: u32)', text)
        if len(sys.argv) > 2:
            fixture = self.root / 'formatter.enq'
            fixture.write_text(self.source)
            subprocess.run([str(Path(sys.argv[2]).resolve()), 'format', str(fixture)],
                           check=True, timeout=30, capture_output=True)
            self.assertEqual(fixture.read_text(), text)
        self.change(text)
        self.assertFalse(self.call('textDocument/formatting', params))


if __name__ == '__main__':
    editor_matrix.BINARY = Path(sys.argv[1]).resolve()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite([
        StructFields('test_fields')]))
    raise SystemExit(not result.wasSuccessful())
