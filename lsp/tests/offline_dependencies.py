#!/usr/bin/env python3
"""Editor graphs never install direct, transitive, or Git dependencies."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile

from protocol_features import request, send


def run_case(binary, root, dependency, *, transitive=False, cached=False):
    root.mkdir()
    (root / 'src').mkdir()
    registry = root / 'registry'
    checksum = 'a' * 64
    package = registry / 'packages' / 'editor_fixture' / ('1.0.0-' + checksum)
    if cached:
        (package / 'src').mkdir(parents=True)
        (package / 'encore.toml').write_text(
            '[project]\nname="editor_fixture"\nversion="1.0.0"\ndependencies=[]\n')
        (package / 'src/lib.enq').write_text('pub struct Value { item: u32 }\n')
    if dependency.startswith('index@'):
        (root / 'encore.lock').write_text(
            'version = 2\n[[packages]]\nname = "editor_fixture"\n'
            'ref = "index@editor_fixture"\nversion = "1.0.0"\n'
            'archive = "https://invalid.example/editor_fixture.tar.gz"\n'
            f'checksum = "{checksum}"\n')
    if transitive:
        nested = root / 'nested'
        (nested / 'src').mkdir(parents=True)
        (nested / 'encore.toml').write_text(
            '[project]\nname="nested"\nversion="0.0.0"\n'
            f'dependencies=["{dependency}"]\n')
        (nested / 'src/lib.enq').write_text('pub struct Local { item: u32 }\n')
        dependency = 'path@nested'
    (root / 'encore.toml').write_text(
        '[project]\nname="offline_editor"\nversion="0.0.0"\n'
        f'dependencies=["{dependency}"]\n')
    source = 'fn main() -> u32 { ret 0 }\n'
    path = root / 'src/main.enq'
    path.write_text(source)
    marker = root / 'external-command'
    commands = root / 'commands'
    commands.mkdir()
    for name in ('curl', 'git'):
        command = commands / name
        command.write_text('#!/bin/sh\nprintf attempted > "$EDITOR_COMMAND_MARKER"\nexit 91\n')
        command.chmod(0o755)
    env = {**os.environ, 'PATH': str(commands) + os.pathsep + os.environ['PATH'],
           'ENCORE_REGISTRY_CACHE': str(registry), 'EDITOR_COMMAND_MARKER': str(marker)}
    before = {p.relative_to(root): p.read_bytes() for p in root.rglob('*') if p.is_file()}
    process = subprocess.Popen([str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, env=env)
    try:
        request(process, 1, 'initialize', {'rootUri': root.as_uri()})
        send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
            'textDocument': {'uri': path.as_uri(), 'languageId': 'encore', 'version': 1, 'text': source}}})
        report = request(process, 2, 'textDocument/diagnostic', {
            'textDocument': {'uri': path.as_uri()}})
        messages = [item['message'] for item in report['items']]
        missing = any('Dependency is not installed:' in message for message in messages)
        assert missing != cached, (cached, messages)
        if missing:
            assert any('encore sync' in message for message in messages), messages
        # A missing package must not kill or stall subsequent editor queries.
        result = request(process, 3, 'textDocument/documentSymbol', {
            'textDocument': {'uri': path.as_uri()}})
        assert any(item['name'] == 'main' for item in result), result
        request(process, 4, 'shutdown', {})
        send(process, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
        assert process.wait(timeout=10) == 0
        assert not marker.exists(), 'LSP attempted a download or clone'
        for relative, contents in before.items():
            assert (root / relative).read_bytes() == contents, relative
        if not cached:
            assert not registry.exists(), 'LSP created the dependency cache'
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='encore-lsp-offline-') as temporary:
        root = Path(temporary)
        run_case(binary, root / 'missing', 'index@editor_fixture')
        run_case(binary, root / 'transitive', 'index@editor_fixture', transitive=True)
        run_case(binary, root / 'git', 'git@https://invalid.example/encore-editor-fixture/not-installed.git')
        run_case(binary, root / 'installed', 'index@editor_fixture', cached=True)
        run_case(binary, root / 'installed-transitive', 'index@editor_fixture', transitive=True, cached=True)
    print('offline dependency graphs: 5 scenarios passed')


if __name__ == '__main__':
    main()
