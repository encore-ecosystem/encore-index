#!/usr/bin/env python3
"""Hover uses the compiler's inferred safety, including body-only invalidation."""
from pathlib import Path
import subprocess
import sys
import tempfile
from protocol_features import request, send
from member_navigation import position


def main():
    source = ('import mod::helper::imported\n'
              'fn raw() -> u32 { unsafe { ret 1_u32 } }\n'
              '#attr(safe)\nfn trusted() -> u32 { ret raw() }\n'
              'fn transit() -> u32 { ret raw() }\n'
              'fn main() { let a = transit() let b = trusted() let c = imported() }\n')
    with tempfile.TemporaryDirectory(prefix='encore-safety-hover-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text('[project]\nname="safety"\nversion="0.0.0"\ndependencies=[]\n')
        path = root / 'src/main.enq'
        path.write_text(source)
        (root / 'src/helper.enq').write_text('import mod::native::raw\npub fn imported() -> u32 { ret raw() }\n')
        native = root / 'src/native.enq'
        native.write_text('pub fn raw() -> u32 { unsafe { ret 1_u32 } }\n')
        process = subprocess.Popen([str(Path(sys.argv[1]).resolve())], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            request(process, 1, 'initialize', {'rootUri': root.as_uri()})
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': path.as_uri(), 'languageId': 'encore', 'version': 1, 'text': source}}})
            for number, needle, name, expected in [
                (2, 'a = transit()', 'transit', 'Safety: **unsafe**'),
                (3, 'b = trusted()', 'trusted', 'explicit `#attr(safe)`'),
                (6, 'c = imported()', 'imported', 'Safety: **unsafe**'),
            ]:
                result = request(process, number, 'textDocument/hover', {
                    'textDocument': {'uri': path.as_uri()}, 'position': position(source, needle, name)})
                assert expected in result['contents']['value'], result
            updated = source.replace('unsafe { ret 1_u32 }', 'ret 1_u32')
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didChange', 'params': {
                'textDocument': {'uri': path.as_uri(), 'version': 2}, 'contentChanges': [{'text': updated}]}})
            result = request(process, 4, 'textDocument/hover', {
                'textDocument': {'uri': path.as_uri()}, 'position': position(updated, 'a = transit()', 'transit')})
            assert 'Safety: **safe**' in result['contents']['value'], result
            send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': native.as_uri(), 'languageId': 'encore', 'version': 1,
                                 'text': 'pub fn raw() -> u32 { ret 1_u32 }\n'}}})
            for number in range(10, 13):
                result = request(process, number, 'textDocument/hover', {
                    'textDocument': {'uri': path.as_uri()}, 'position': position(updated, 'c = imported()', 'imported')})
                assert 'Safety: **safe**' in result['contents']['value'], result
            request(process, 5, 'shutdown', {})
            send(process, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print('safety hover, trusted boundary and body-only invalidation: ok')


if __name__ == '__main__':
    main()
