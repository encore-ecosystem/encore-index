#!/usr/bin/env python3
"""Keep the server alive throughout incomplete imports and syntax recovery."""

from pathlib import Path
import subprocess
import sys
import tempfile

from protocol_features import request, send


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="encore-lsp-import-editing-") as temporary:
        root = Path(temporary)
        (root / "src").mkdir()
        manifest = '[project]\nname = "editing"\nversion = "0.0.0"\ndependencies = []\n'
        (root / "encore.toml").write_text(manifest)
        path = root / "src" / "main.enq"
        path.write_text("fn main() -> u32 { ret 0 }\n")
        uri = path.as_uri()
        process = subprocess.Popen(
            [str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        try:
            request(process, 1, "initialize", {
                "rootUri": root.as_uri(),
                "initializationOptions": {"jobs": 2, "debounceMs": 0, "cache": False},
            })
            send(process, {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": uri, "version": 1, "languageId": "encore", "text": ""},
            }})
            full = "import std::io::println"
            sources = [full[:end] for end in range(1, len(full) + 1)]
            sources += [full[:end] for end in range(len(full) - 1, -1, -1)]
            sources += [
                "import std::{", "import std::io::{println,", "import std::io::{println}",
                "import std::io\nfn main(", "import std::io\nfn main() -> u32 {",
                "import std::io\nfn main() -> u32 { ret 0 }", "import st", "import std::",
                "import std::io::",
            ]
            program = 'fn main() -> u32 {\n let a = 52\n ret a\n}'
            sources += [program[:end] for end in range(1, len(program) + 1)]
            sources += [
                'macro_rules! broken {',
                'fn broken() -> u32 { let a =\nlet b = 1 ret b }\nfn good() -> u32 { ret 2 }',
                'fn main() -> u32 { ret 0 }',
                'fn main() {\n let a: us',
                'fn f(arg: us',
                'pub struct S { field: us',
            ]
            for version, source in enumerate(sources, 2):
                send(process, {"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                    "textDocument": {"uri": uri, "version": version},
                    "contentChanges": [{"text": source + "\n"}],
                }})
                params = {"textDocument": {"uri": uri}, "position": {
                    "line": source.count("\n"), "character": len(source.split("\n")[-1]),
                }}
                completion = request(process, version * 10, "textDocument/completion", params)
                labels = {item["label"] for item in completion["items"]}
                expected = {"import st": "std", "import std::": "io", "import std::io::": "println"}
                if source.endswith(': us'):
                    expected[source] = "usize"
                if source in expected:
                    assert expected[source] in labels, (source, labels)
                # Wait for the worker pipeline as well: completion alone can
                # succeed just before the strict workspace loader kills it.
                diagnostics = request(process, version * 10 + 1, "textDocument/diagnostic", params)
                assert not any(d.get("code") == "missing-module-docstring"
                               for d in diagnostics["items"]), diagnostics
                request(process, version * 10 + 2, "textDocument/semanticTokens/full", params)
            # Manifest and dependency failures are protocol diagnostics, not
            # exits or unframed text on stdout. Restoring the manifest heals it.
            for index, broken in enumerate([
                '[project]\nversion = "0.0.0"\ndependencies = []\n',
                manifest.replace('dependencies = []', 'dependencies = ["path@missing"]'),
                manifest,
            ]):
                (root / "encore.toml").write_text(broken)
                send(process, {"jsonrpc": "2.0", "method": "workspace/didChangeWatchedFiles", "params": {
                    "changes": [{"uri": (root / "encore.toml").as_uri(), "type": 2}],
                }})
                send(process, {"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                    "textDocument": {"uri": uri, "version": 1000 + index},
                    "contentChanges": [{"text": 'fn main() -> u32 { ret 0 }\n'}],
                }})
                diagnostics = request(process, 9000 + index, "textDocument/diagnostic", {
                    "textDocument": {"uri": uri},
                })
                failures = [d for d in diagnostics["items"] if d.get("code") in ("manifest", "workspace")]
                assert bool(failures) == (index < 2), (index, diagnostics)
            request(process, 10000, "shutdown", {})
            send(process, {"jsonrpc": "2.0", "method": "exit", "params": {}})
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print("incremental import editing, syntax recovery and default doc lint: ok")


if __name__ == "__main__":
    main()
