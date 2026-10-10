#!/usr/bin/env python3
"""Audit real system-library safety effects and re-exported console helpers."""

from pathlib import Path
import subprocess
import sys
import tempfile

from member_navigation import position
from protocol_features import request, send


def main():
    index = Path(__file__).resolve().parents[2]
    source = ('import std::io::{println, print, io_write}\n'
              'fn main() { print("hello") println("world") io_write(1_i32, "!") }\n')
    with tempfile.TemporaryDirectory(prefix="encore-library-safety-") as directory:
        root = Path(directory)
        (root / "src").mkdir()
        (root / "encore.toml").write_text(
            '[project]\nname="library_safety"\nversion="0.0.0"\n'
            f'dependencies=["path@{(index / "packages/std").as_posix()}"]\n')
        path = root / "src/main.enq"
        path.write_text(source)
        process = subprocess.Popen([str(Path(sys.argv[1]).resolve())], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        next_id = 1

        def call(method, params):
            nonlocal next_id
            next_id += 1
            return request(process, next_id, method, params)

        def open_document(path, text):
            send(process, {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": path.as_uri(), "version": 1, "text": text}}})

        def expect(path, text, needle, name, safe, asserted=False):
            result = call("textDocument/hover", {
                "textDocument": {"uri": path.as_uri()}, "position": position(text, needle, name)})
            value = result["contents"]["value"]
            assert f'Safety: **{"safe" if safe else "unsafe"}**' in value, (path, needle, result)
            if asserted:
                assert "explicit `#attr(safe)`" in value, (path, needle, result)

        try:
            call("initialize", {"rootUri": root.as_uri(), "initializationOptions": {
                "jobs": 2, "debounceMs": 0, "cache": False}})
            open_document(path, source)
            for name in ("print", "println", "io_write"):
                expect(path, source, f'{name}("' if name != "io_write" else "io_write(1_i32", name, True)
            cases = {
                "platform/src/io/mod.enq": [("io_read", True), ("wait_readable", True), ("io_write", True)],
                "platform/src/time/mod.enq": [("perf_counter_ns", True)],
                "platform/src/os/mod.enq": [("argc", True), ("file_sha256", True)],
                "platform/src/process/mod.enq": [("output", True), ("run_args_owned", True)],
                "platform/src/net/mod.enq": [("read", True)],
                "platform/src/tls/mod.enq": [("connect", True), ("read", False), ("close", False)],
                "platform/src/net/loopback.enq": [("connect", True), ("read", False)],
                "platform/src/thread/mod.enq": [("available_parallelism", True), ("is_finished", False)],
                "core/src/vec/mod.enq": [("with_capacity", True), ("get", True), ("push", True), ("get_unchecked_borrowed", False)],
                "core/src/future/mod.enq": [("wake", False)],
                "std/src/fs/mod.enq": [("remove_file", True)],
                "std/src/http/mod.enq": [("send", True)],
                "std/src/future/mod.enq": [("block_on", False)],
            }
            count = 3
            for relative, functions in cases.items():
                target = index / "packages" / relative
                text = target.read_text()
                open_document(target, text)
                for name, safe in functions:
                    expect(target, text, "pub fn " + name + "(" if name != "block_on" else "pub fn block_on[", name, safe, safe)
                    count += 1
            call("shutdown", {})
            send(process, {"jsonrpc": "2.0", "method": "exit", "params": {}})
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print(f"system-library safety: {count} boundaries, unsafe contracts and console re-exports; ok")


if __name__ == "__main__":
    main()
