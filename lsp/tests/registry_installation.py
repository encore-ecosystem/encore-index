#!/usr/bin/env python3
"""Cursor queries see an index dependency installed while the LSP stays alive.

The cache fixture is deterministic and offline; Pomodoro's separate workflow
also exercises genuine publication and `encore sync` downloads.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from protocol_features import request, send
from scheduler import stop


def main():
    with tempfile.TemporaryDirectory(prefix='encore-lsp-registry-install-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        source = root / 'src/main.enq'
        source.write_text('import sample::')
        manifest = root / 'encore.toml'
        index = 'http://127.0.0.1:9/index'
        manifest.write_text('[project]\nname="installation"\nversion="0.0.0"\n'
                            'dependencies=["index@sample@1.0.0"]\n'
                            f'[registry]\nindex="{index}"\n')
        checksum = 'a' * 64
        (root / 'encore.lock').write_text('version = 2\n[[packages]]\nname = "sample"\n'
            'ref = "index@sample@1.0.0"\nversion = "1.0.0"\n'
            f'checksum = "{checksum}"\narchive = "{index}/sample.tar.gz"\nregistry = "{index}"\n')
        cache = root / 'cache'
        package = cache / 'sources' / (index.encode().hex() + '-index') / 'packages/sample' / ('1.0.0-' + checksum)
        environment = {key: value for key, value in os.environ.items()
                       if key not in ('ENCORE_INDEX_URL', 'ENCORE_DEFAULT_INDEX')}
        environment['ENCORE_REGISTRY_CACHE'] = str(cache)
        process = subprocess.Popen([str(Path(sys.argv[1]).resolve())],
            env=environment,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        pid = process.pid
        number = 0

        def call(method, params):
            nonlocal number
            number += 1
            return request(process, number, method, params)

        cursor = {'textDocument': {'uri': source.as_uri()}, 'position': {'line': 0, 'character': 15}}
        try:
            call('initialize', {'rootUri': root.as_uri(), 'initializationOptions': {'jobs': 1, 'debounceMs': 0, 'cache': False}})
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': source.as_uri(), 'version': 1, 'text': source.read_text()}}})
            before = call('textDocument/completion', cursor)
            assert not any(item['label'] == 'Widget' for item in before['items']), before
            declared = call('textDocument/completion', {**cursor, 'position': {'line': 0, 'character': 9}})
            assert not any(item['label'] == 'sample' for item in declared['items']), declared
            (package / 'src').mkdir(parents=True)
            (package / 'encore.toml').write_text('[project]\nname="sample"\nversion="1.0.0"\ndependencies=[]\n')
            lib = package / 'src/lib.enq'
            lib.write_text('//! Sample package documentation.\npub struct Widget { value: u32 }\n')
            send(process, {'jsonrpc': '2.0', 'method': 'workspace/didChangeWatchedFiles', 'params': {
                'changes': [{'uri': (root / 'encore.lock').as_uri(), 'type': 2}]}})
            # No didChange for the source, no restart, no networking.
            after = call('textDocument/completion', cursor)
            assert any(item['label'] == 'Widget' for item in after['items']), after
            installed = call('textDocument/completion', {**cursor, 'position': {'line': 0, 'character': 9}})
            assert any(item['label'] == 'sample' for item in installed['items']), installed
            package_cursor = {**cursor, 'position': {'line': 0, 'character': 8}}
            definition = call('textDocument/definition', package_cursor)
            assert definition and definition[0]['uri'] == lib.as_uri(), definition
            assert call('textDocument/hover', package_cursor)
            broken = 'import sample::Missing\nfn main() -> u32 { ret 0 }\n'
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': source.as_uri(), 'version': 2},
                'contentChanges': [{'text': broken}]}})
            diagnostics = call('textDocument/diagnostic', {'textDocument': {'uri': source.as_uri()}})
            assert any(item['code'] == 'unresolved-import' for item in diagnostics['items']), diagnostics
            assert not call('textDocument/definition', {**cursor, 'position': {'line': 0, 'character': 17}})
            fixed = 'import sample::Widget\nfn main() -> u32 { ret 0 }\n'
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': source.as_uri(), 'version': 3},
                'contentChanges': [{'text': fixed}]}})
            diagnostics = call('textDocument/diagnostic', {'textDocument': {'uri': source.as_uri()}})
            assert not [item for item in diagnostics['items'] if item.get('severity') == 1], diagnostics
            assert process.pid == pid and process.poll() is None
            stop(process)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print('index dependency installation, import exports and package navigation without restart: ok')


if __name__ == '__main__':
    main()
