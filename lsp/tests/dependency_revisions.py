#!/usr/bin/env python3
"""Imported buffer edits invalidate diagnostics; closing restores disk state."""

from pathlib import Path
import subprocess
import sys
import tempfile

from protocol_features import request, send
from scheduler import response, stop


def main():
    with tempfile.TemporaryDirectory(prefix='encore-lsp-dependency-revision-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="revisions"\nversion="0.0.0"\ndependencies=[]\n')
        model = root / 'src/model.enq'
        main = root / 'src/main.enq'
        helper = root / 'src/helper.enq'
        helper.write_text('pub fn make() -> str { ret "helper" }\n')
        original = 'import refrain::helper::make as helper_make\npub fn make() -> u32 { helper_make() ret 1 }\n'
        changed = 'pub fn make() -> str { ret "changed" }\n'
        source = 'import refrain::model::make\nfn main() -> u32 { ret make() }\n'
        model.write_text(original)
        main.write_text(source)
        caller = root / 'src/caller.enq'
        caller_text = 'import refrain::model::make\nfn caller() { let value = make() }\n'
        caller.write_text(caller_text)
        process = subprocess.Popen([str(Path(sys.argv[1]).resolve())], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        number = 0

        def diagnostics():
            nonlocal number
            number += 1
            send(process, {'jsonrpc': '2.0', 'id': number, 'method': 'textDocument/diagnostic',
                           'params': {'textDocument': {'uri': main.as_uri()}}})
            result = response(process, number)
            assert 'result' in result, result
            return result['result']['items']

        def open_document(path, text):
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 1, 'text': text}}})

        try:
            request(process, 0, 'initialize', {'rootUri': root.as_uri(),
                    'initializationOptions': {'jobs': 1, 'debounceMs': 0, 'cache': False}})
            open_document(main, source)
            open_document(model, original)
            open_document(caller, caller_text)
            value_position = {'textDocument': {'uri': caller.as_uri()},
                              'position': {'line': 1, 'character': caller_text.splitlines()[1].index('value') + 1}}
            value_hover = request(process, 60, 'textDocument/hover', value_position)
            assert 'u32' in str(value_hover), value_hover
            assert not diagnostics()
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': model.as_uri(), 'version': 2},
                'contentChanges': [{'text': changed}]}})
            findings = diagnostics()
            assert any(item.get('code') == 'return-type-mismatch' for item in findings), findings
            value_hover = request(process, 61, 'textDocument/hover', value_position)
            assert 'str' in str(value_hover) and 'u32' not in str(value_hover), value_hover
            # No didChange for the importing file: its cached diagnostics must
            # follow the imported signature, including after an unsaved close.
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didClose',
                           'params': {'textDocument': {'uri': model.as_uri()}}})
            assert not diagnostics()
            hover = request(process, 50, 'textDocument/hover', {
                'textDocument': {'uri': main.as_uri()}, 'position': {'line': 1, 'character': 24}})
            assert 'u32' in str(hover) and 'str' not in str(hover), hover
            # A topology cache must invalidate a changed child re-export even
            # when the importing parent is untouched and was queried already.
            open_document(model, 'pub import mod::helper::make\n')
            findings = diagnostics()
            assert any(item.get('code') == 'return-type-mismatch' for item in findings), findings
            definition = request(process, 51, 'textDocument/definition', {
                'textDocument': {'uri': main.as_uri()}, 'position': {'line': 1, 'character': 24}})
            assert definition and definition[0]['uri'] == helper.as_uri(), definition
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didClose',
                           'params': {'textDocument': {'uri': model.as_uri()}}})
            assert not diagnostics()
            stop(process)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print('dependency revisions, stale semantic results and unsaved close: ok')


if __name__ == '__main__':
    main()
