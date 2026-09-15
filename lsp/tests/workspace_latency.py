#!/usr/bin/env python3
"""Measure real-workspace LSP queries without modifying source files."""

import argparse
import json
import os
from pathlib import Path
import signal
import statistics
import subprocess
import time

from protocol_features import request, send


def process_usage(pid):
    root = Path('/proc') / str(pid)
    if not root.exists():
        return {}
    status = dict(line.split(':', 1) for line in (root / 'status').read_text().splitlines() if ':' in line)
    stat = (root / 'stat').read_text().split(') ', 1)[1].split()
    return {'rss_mib': round(int(status['VmRSS'].split()[0]) / 1024, 2),
            'peak_mib': round(int(status['VmHWM'].split()[0]) / 1024, 2),
            'cpu_s': round((int(stat[11]) + int(stat[12])) / os.sysconf('SC_CLK_TCK'), 3)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('root', type=Path)
    parser.add_argument('--samples', type=int, default=5)
    parser.add_argument('--max-warm-ms', type=float)
    parser.add_argument('--settle-seconds', type=float, default=0,
                        help='allow initial background work before measuring queries')
    parser.add_argument('--idle-samples', type=int, default=1,
                        help='number of two-second idle CPU/RSS samples after queries')
    args = parser.parse_args()
    if args.samples < 1 or args.idle_samples < 1:
        parser.error('sample counts must be positive')
    if not 0 <= args.settle_seconds <= 60:
        parser.error('settle delay must be between zero and 60 seconds')
    root = args.root.resolve()
    path = root / 'src/analyzer/mod.enq'
    source = path.read_text()
    process = subprocess.Popen([str(args.binary.resolve())], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    number = 0

    def call(method, params):
        nonlocal number
        number += 1
        signal.alarm(120)
        try:
            return request(process, number, method, params)
        finally:
            signal.alarm(0)

    def timeout(_signal, _frame):
        raise TimeoutError('LSP query exceeded 120 seconds')

    signal.signal(signal.SIGALRM, timeout)
    try:
        start = time.monotonic()
        call('initialize', {'rootUri': root.as_uri()})
        print(json.dumps({'phase': 'initialize', 'ms': round((time.monotonic() - start) * 1000, 2),
                          **process_usage(process.pid)}), flush=True)
        send(process, {'jsonrpc': '2.0', 'method': 'textDocument/didOpen', 'params': {
            'textDocument': {'uri': path.as_uri(), 'languageId': 'encore', 'version': 1, 'text': source}}})
        if args.settle_seconds:
            time.sleep(args.settle_seconds)
            print(json.dumps({'phase': 'settled', **process_usage(process.pid)}), flush=True)
        for name, method, needle, word in [
            ('import_definition', 'textDocument/definition', 'import std::vec::Vec', 'Vec'),
            ('field_hover', 'textDocument/hover', 'document.tokens', 'tokens'),
            ('field_definition', 'textDocument/definition', 'document.tokens', 'tokens'),
            ('method_hover', 'textDocument/hover', 'config.level(', 'level'),
            ('field_highlights', 'textDocument/documentHighlight', 'document.tokens', 'tokens'),
            ('visible_inlays', 'textDocument/inlayHint', 'let usage_tokens', 'usage_tokens'),
            ('associated_hover', 'textDocument/hover', 'Vec[AnalyzerDiagnostic]::new', 'new'),
            ('associated_completion', 'textDocument/completion', 'Vec[AnalyzerDiagnostic]::new', 'new'),
        ]:
            offset = source.index(needle) + needle.index(word)
            prefix = source[:offset]
            position = {'line': prefix.count('\n'), 'character': len(prefix.rsplit('\n', 1)[-1])}
            params = {'textDocument': {'uri': path.as_uri()}, 'position': position}
            if method == 'textDocument/inlayHint':
                params = {'textDocument': {'uri': path.as_uri()}, 'range': {
                    'start': {'line': max(0, position['line'] - 10), 'character': 0},
                    'end': {'line': position['line'] + 10, 'character': 0}}}
            samples = []
            for sample in range(args.samples + 1):
                started = time.monotonic()
                result = call(method, params)
                elapsed = (time.monotonic() - started) * 1000
                assert result, (name, result)
                if name == 'visible_inlays':
                    labels = {hint['label'] for hint in result}
                    positions = [(hint['position']['line'], hint['position']['character']) for hint in result]
                    assert len(positions) == len(set(positions)), result
                    assert ': Vec[AnalyzerDiagnostic]' in labels, labels
                    assert ': Vec[Token]' in labels, labels
                if name == 'associated_hover':
                    assert 'new() -> Vec[AnalyzerDiagnostic]' in str(result), result
                if name == 'associated_completion':
                    members = {item['label']: item for item in result['items']}
                    assert {'new', 'singleton', 'with_capacity'} <= members.keys(), members
                    assert not {'push', 'len', 'get'} & members.keys(), members
                    assert members['new']['detail'] == 'fn new() -> Vec[AnalyzerDiagnostic]', members
                if sample:
                    samples.append(elapsed)
                else:
                    print(json.dumps({'phase': name, 'cold_ms': round(elapsed, 2),
                                      **process_usage(process.pid)}), flush=True)
            print(json.dumps({'phase': name, 'median_ms': round(statistics.median(samples), 2),
                              'max_ms': round(max(samples), 2), **process_usage(process.pid)}), flush=True)
            if args.max_warm_ms is not None:
                assert statistics.median(samples) <= args.max_warm_ms, (name, samples)
        for sample in range(args.idle_samples):
            before = process_usage(process.pid)
            time.sleep(2)
            after = process_usage(process.pid)
            print(json.dumps({'phase': 'idle_2s', 'sample': sample + 1,
                              'cpu_delta_s': round(after.get('cpu_s', 0) - before.get('cpu_s', 0), 3),
                              **after}), flush=True)
        call('shutdown', {})
        send(process, {'jsonrpc': '2.0', 'method': 'exit', 'params': {}})
        assert process.wait(timeout=10) == 0
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


if __name__ == '__main__':
    main()
