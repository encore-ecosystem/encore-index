#!/usr/bin/env python3
"""Autoimports are explicit edits and never leak unrelated workspace symbols."""

from pathlib import Path
import subprocess
import sys
import tempfile

from protocol_features import request, send


def package(root, name, dependencies='[]'):
    (root / 'src').mkdir(parents=True)
    (root / 'encore.toml').write_text(
        f'[project]\nname="{name}"\nversion="0.0.0"\ndependencies={dependencies}\n')


def main():
    with tempfile.TemporaryDirectory(prefix='encore-lsp-autoimports-') as directory:
        parent = Path(directory)
        root, dependency, unrelated = [parent / name for name in ('app', 'dependency', 'unrelated')]
        package(root, 'app', '["path@../dependency"]')
        package(dependency, 'dependency')
        package(unrelated, 'unrelated')
        (root / 'src/helper.enq').write_text('/// Local helper.\npub struct LocalValue { value: u32 }\n')
        (dependency / 'src/lib.enq').write_text('pub import refrain::inner::Original as RenamedValue\n/// Dependency value.\npub struct RemoteValue { value: u32 }\nstruct RemoteSecret {}\n')
        (dependency / 'src/inner.enq').write_text('/// Re-exported value.\npub struct Original {}\n')
        (unrelated / 'src/lib.enq').write_text('pub struct RemoteUnrelated {}\n')
        path = root / 'src/main.enq'
        path.write_text('fn main() -> u32 { ret 0 }\n')
        p = subprocess.Popen([str(Path(sys.argv[1]).resolve())], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            request(p, 1, 'initialize', {'workspaceFolders': [
                {'uri': item.as_uri(), 'name': item.name} for item in (root, unrelated)]})
            for file in (unrelated / 'src/lib.enq', path):
                send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                    'textDocument': {'uri': file.as_uri(), 'version': 1, 'languageId': 'encore', 'text': file.read_text()}}})
                request(p, 2 if file != path else 3, 'textDocument/diagnostic', {'textDocument': {'uri': file.as_uri()}})
            cases = [
                ('//! 🌍 Module docs.\r\n\r\n/// Item docs.\r\nfn sample(value: Remote|) {}\r\n',
                 'RemoteValue', 'import dependency::RemoteValue\r\n', 2, {'RemoteSecret', 'RemoteUnrelated'}),
                ('fn sample(value: Local|) {}\n', 'LocalValue', 'import refrain::helper::LocalValue\n', 0, set()),
                ('import dependency::RemoteValue\nfn sample(value: Remote|) {}\n',
                 'RemoteValue', None, 0, {'RemoteSecret', 'RemoteUnrelated'}),
                ('struct RemoteValue {}\nfn sample(value: Remote|) {}\n',
                 'RemoteValue', None, 0, {'RemoteSecret', 'RemoteUnrelated'}),
                ('fn sample(value: Renamed|) {}\n', 'RenamedValue', 'import dependency::RenamedValue\n', 0, set()),
            ]
            for version, (marked, name, edit_text, edit_line, excluded) in enumerate(cases, 2):
                before, after = marked.split('|')
                source = before + after
                send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                    'textDocument': {'uri': path.as_uri(), 'version': version}, 'contentChanges': [{'text': source}]}})
                result = request(p, version * 10, 'textDocument/completion', {
                    'textDocument': {'uri': path.as_uri()}, 'position': {
                        'line': before.count('\n'), 'character': len(before.rsplit('\n', 1)[-1].encode('utf-16-le')) // 2}})
                items = {item['label']: item for item in result['items']}
                assert name in items and not excluded & items.keys(), (marked, items)
                edits = items[name].get('additionalTextEdits', [])
                if edit_text is None:
                    assert edits == [], items[name]
                else:
                    assert len(edits) == 1, items[name]
                    assert edits[0]['newText'] == edit_text, edits
                    assert edits[0]['range'] == {
                        'start': {'line': edit_line, 'character': 0},
                        'end': {'line': edit_line, 'character': 0}}, edits
            source = 'fn sample(value: RemoteValue) {}\n'
            send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 10}, 'contentChanges': [{'text': source}]}})
            report = request(p, 80, 'textDocument/diagnostic', {'textDocument': {'uri': path.as_uri()}})
            unknown = next(item for item in report['items'] if item['code'] == 'unknown-type')
            actions = request(p, 81, 'textDocument/codeAction', {
                'textDocument': {'uri': path.as_uri()}, 'range': unknown['range'],
                'context': {'diagnostics': [unknown], 'only': []}})
            action = next(item for item in actions if item['title'] == 'Import dependency::RemoteValue')
            change = action['edit']['documentChanges'][0]
            assert change['textDocument'] == {'uri': path.as_uri(), 'version': 10}, change
            assert change['edits'][0]['newText'] == 'import dependency::RemoteValue\n', change
            source = 'import dependency::RemoteValue\n' + source
            send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 11}, 'contentChanges': [{'text': source}]}})
            actions = request(p, 82, 'textDocument/codeAction', {
                'textDocument': {'uri': path.as_uri()},
                'range': {'start': {'line': 0, 'character': 0}, 'end': {'line': 2, 'character': 0}},
                'context': {'diagnostics': [unknown]}})
            assert not any(item['title'].startswith('Import ') for item in actions), actions
            # Ordinary completion must include lexical bindings even while the
            # background graph is still being prepared, without leaking siblings.
            marked = ('fn local() -> u32 { ret 0 }\nfn sample(arg: u32) {\n'
                      ' let local = "shadow"\n if true { let hidden = 1 }\n loc|\n let later = 2\n}\n')
            before, after = marked.split('|')
            send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 12}, 'contentChanges': [{'text': before + after}]}})
            result = request(p, 83, 'textDocument/completion', {
                'textDocument': {'uri': path.as_uri()}, 'position': {
                    'line': before.count('\n'), 'character': len(before.rsplit('\n', 1)[-1])}})
            items = {item['label']: item for item in result['items']}
            assert items['local']['kind'] == 6 and items['local']['detail'] == 'str', items
            assert 'hidden' not in items and 'later' not in items, items
            request(p, 100, 'shutdown', {})
            send(p, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
            assert p.wait(timeout=10) == 0
            assert path.read_text() == 'fn main() -> u32 { ret 0 }\n'
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()
    print('scoped autoimports, documentation, CRLF and conflicts: ok')


if __name__ == '__main__':
    main()
