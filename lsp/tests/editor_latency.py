#!/usr/bin/env python3
"""Measure warm navigation and import completion independently of build time."""

import json
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import time

from protocol_features import request, send


def main():
    with tempfile.TemporaryDirectory(prefix='encore-lsp-latency-') as directory:
        root = Path(directory)
        (root / 'src').mkdir()
        (root / 'encore.toml').write_text(
            '[project]\nname="latency"\nversion="0.0.0"\ndependencies=[]\n')
        source = 'import std::io::println\nfn main() -> u32 { println("hello") ret 0 }\n'
        path = root / 'src/main.enq'
        path.write_text(source)
        p = subprocess.Popen([str(Path(sys.argv[1]).resolve())], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            request(p, 1, 'initialize', {'rootUri': root.as_uri()})
            send(p, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
                'textDocument': {'uri': path.as_uri(), 'languageId': 'encore', 'version': 1, 'text': source},
            }})
            request(p, 2, 'textDocument/diagnostic', {'textDocument': {'uri': path.as_uri()}})
            report = {}
            for offset, method, position in [
                (10, 'textDocument/definition', {'line': 1, 'character': 24}),
                (100, 'textDocument/completion', {'line': 0, 'character': 16}),
            ]:
                samples = []
                for index in range(31):
                    start = time.monotonic()
                    result = request(p, offset + index, method, {
                        'textDocument': {'uri': path.as_uri()}, 'position': position,
                    })
                    elapsed = (time.monotonic() - start) * 1000
                    assert result, (method, result)
                    if index:
                        samples.append(elapsed)
                    else:
                        cold = elapsed
                p95 = sorted(samples)[28]
                report[method] = {'cold_ms': round(cold, 2),
                                  'median_ms': round(statistics.median(samples), 2),
                                  'p95_ms': round(p95, 2)}
            print(json.dumps(report))
            assert all(item['p95_ms'] <= 50 for item in report.values()), report
            request(p, 200, 'shutdown', {})
            send(p, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
            assert p.wait(timeout=10) == 0
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()


if __name__ == '__main__':
    main()
