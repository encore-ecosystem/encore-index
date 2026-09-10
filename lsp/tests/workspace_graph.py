#!/usr/bin/env python3

"""End-to-end coverage for project-scoped semantic dependency graphs."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile


def send(process: subprocess.Popen[bytes], payload: object) -> None:
    body = json.dumps(payload, separators=(",", ":")).encode()
    assert process.stdin is not None
    process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
    process.stdin.flush()


def read(process: subprocess.Popen[bytes]) -> dict:
    assert process.stdout is not None
    length = None
    while True:
        line = process.stdout.readline()
        if not line:
            assert process.stderr is not None
            raise RuntimeError(process.stderr.read().decode())
        if line in (b"\n", b"\r\n"):
            break
        name, value = line.decode().split(":", 1)
        if name.lower() == "content-length":
            length = int(value)
    assert length is not None
    return json.loads(process.stdout.read(length))


def response(process: subprocess.Popen[bytes], request_id: int) -> dict:
    while True:
        message = read(process)
        if message.get("id") == request_id:
            assert "error" not in message, message
            return message


def diagnostics(process: subprocess.Popen[bytes], request_id: int, uri: str) -> list[dict]:
    send(
        process,
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": "textDocument/diagnostic",
            "params": {"textDocument": {"uri": uri}},
        },
    )
    return response(process, request_id)["result"]["items"]


def fixture(root: Path, exported_type: str) -> tuple[Path, str]:
    dependency = root / "dep"
    (dependency / "src" / "types").mkdir(parents=True)
    (dependency / "encore.toml").write_text(
        '[project]\nname = "shared"\nversion = "0.1.0"\ndependencies = []\n'
    )
    (dependency / "src" / "lib.enq").write_text("pub import refrain::types::*\n")
    (dependency / "src" / "types" / "mod.enq").write_text(
        f"pub struct {exported_type} {{ value: u32 }}\n"
    )

    source_dir = root / "src"
    source_dir.mkdir()
    (root / "encore.toml").write_text(
        '[project]\nname = "same_app"\nversion = "0.1.0"\n'
        'dependencies = ["path@dep"]\n'
    )
    path = source_dir / "main.enq"
    # Keep the disk source deliberately stale. didOpen must be the overlay used
    # to discover the dependency graph.
    path.write_text("fn main() -> u32 { ret 0_u32 }\n")
    source = (
        "//! Multi-root semantic fixture.\n\n"
        f"import shared::types::{exported_type}\n"
        f"fn consume(value: {exported_type}) -> u32 {{ ret value.value }}\n"
        "fn main() -> u32 { ret 0_u32 }\n"
    )
    return path, source


def main() -> None:
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="encore-lsp-workspace-graph-") as temporary:
        parent = Path(temporary)
        first_path, first_source = fixture(parent / "first", "Alpha")
        second_path, second_source = fixture(parent / "second", "Beta")
        roots = [first_path.parents[1], second_path.parents[1]]

        process = subprocess.Popen(
            [str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        try:
            send(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "method": "initialize",
                    "params": {
                        "rootUri": parent.as_uri(),
                        "workspaceFolders": [
                            {"uri": root.as_uri(), "name": root.name} for root in roots
                        ],
                        "initializationOptions": {"jobs": 4, "debounceMs": 0, "cache": False},
                    },
                },
            )
            response(process, 1)
            send(process, {"jsonrpc": "2.0", "method": "initialized", "params": {}})

            for path, source in ((first_path, first_source), (second_path, second_source)):
                send(
                    process,
                    {
                        "jsonrpc": "2.0",
                        "method": "textDocument/didOpen",
                        "params": {
                            "textDocument": {
                                "uri": path.as_uri(),
                                "languageId": "encore",
                                "version": 1,
                                "text": source,
                            }
                        },
                    },
                )

            first = diagnostics(process, 2, first_path.as_uri())
            second = diagnostics(process, 3, second_path.as_uri())
            assert not first, first
            assert not second, second

            # Force a fresh analysis of the first root after the second root,
            # whose root package and dependency names intentionally collide.
            send(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": first_path.as_uri(), "version": 2},
                        "contentChanges": [{"text": first_source + "\n"}],
                    },
                },
            )
            assert not diagnostics(process, 4, first_path.as_uri())

            broken = first_source.replace("Alpha) ->", "Missing) ->")
            send(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": first_path.as_uri(), "version": 3},
                        "contentChanges": [{"text": broken}],
                    },
                },
            )
            findings = diagnostics(process, 5, first_path.as_uri())
            assert any(
                item.get("code") == "unknown-type" and "Missing" in item.get("message", "")
                for item in findings
            ), findings

            # An incomplete import is an ordinary editor state and must not
            # enter the strict workspace loader or terminate the server.
            send(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": first_path.as_uri(), "version": 4},
                        "contentChanges": [{"text": "import shared::\n"}],
                    },
                },
            )
            diagnostics(process, 6, first_path.as_uri())

            send(process, {"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": None})
            response(process, 99)
            send(process, {"jsonrpc": "2.0", "method": "exit", "params": None})
            process.wait(timeout=5)
            assert process.returncode == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()


if __name__ == "__main__":
    main()
    print("project-scoped semantic workspace graph: ok")
