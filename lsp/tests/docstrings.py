#!/usr/bin/env python3

import json
from pathlib import Path
import subprocess
import sys
import tempfile


def send_message(process: subprocess.Popen[bytes], payload: object) -> None:
    body = json.dumps(payload, separators=(",", ":")).encode()
    assert process.stdin is not None
    process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
    process.stdin.flush()


def read_message(process: subprocess.Popen[bytes]) -> dict:
    assert process.stdout is not None
    length = None
    while True:
        raw = process.stdout.readline()
        if not raw:
            raise RuntimeError("LSP server closed its output")
        line = raw.decode().strip()
        if not line:
            break
        name, value = line.split(":", 1)
        if name.lower() == "content-length":
            length = int(value.strip())
    if length is None:
        raise RuntimeError("LSP message has no Content-Length")
    return json.loads(process.stdout.read(length))


def response_for(process: subprocess.Popen[bytes], request_id: int) -> dict:
    while True:
        message = read_message(process)
        if message.get("id") == request_id:
            return message


def main() -> None:
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="encore-lsp-docstrings-") as temporary:
        root = Path(temporary)
        source_dir = root / "src"
        source_dir.mkdir()
        (root / "encore.toml").write_text(
            '[project]\nname = "doc_hover"\nversion = "0.1.0"\ndependencies = []\n'
            '\n[format]\nindent-width = 2\n'
        )

        net_source = """//! Networking primitives.

/// Opens a connection.
///
/// # Errors
/// Returns an error when the peer is unavailable.
pub fn connect() -> u32 { ret 0_u32 }
"""
        main_source = """import doc_hover::net::connect
fn main() -> u32 {
    let answer = connect()
    ret answer
}
"""
        net_path = source_dir / "net.enq"
        main_path = source_dir / "main.enq"
        net_path.write_text(net_source)
        main_path.write_text(main_source)

        process = subprocess.Popen(
            [str(binary)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            root_uri = root.as_uri()
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "method": "initialize",
                    "params": {
                        "rootUri": root_uri,
                        "workspaceFolders": [{"uri": root_uri, "name": "doc_hover"}],
                    },
                },
            )
            initialized = response_for(process, 1)
            if "error" in initialized:
                raise RuntimeError(f"initialize failed: {initialized}")
            send_message(process, {"jsonrpc": "2.0", "method": "initialized", "params": {}})

            for uri, text in ((net_path.as_uri(), net_source), (main_path.as_uri(), main_source)):
                send_message(
                    process,
                    {
                        "jsonrpc": "2.0",
                        "method": "textDocument/didOpen",
                        "params": {
                            "textDocument": {
                                "uri": uri,
                                "languageId": "encore",
                                "version": 1,
                                "text": text,
                            }
                        },
                    },
                )

            character = main_source.splitlines()[2].index("connect") + 2
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 2,
                    "method": "textDocument/hover",
                    "params": {
                        "textDocument": {"uri": main_path.as_uri()},
                        "position": {"line": 2, "character": character},
                    },
                },
            )
            hover = response_for(process, 2)["result"]
            contents = hover["contents"]
            value = contents["value"]
            assert contents["kind"] == "markdown"
            assert "```encore\nfn connect\n```" in value, value
            assert "Opens a connection." in value, value
            assert "# Errors" in value, value

            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 3,
                    "method": "textDocument/inlayHint",
                    "params": {
                        "textDocument": {"uri": main_path.as_uri()},
                        "range": {
                            "start": {"line": 0, "character": 0},
                            "end": {"line": 4, "character": 0},
                        },
                    },
                },
            )
            hints = response_for(process, 3)["result"]
            assert any(hint.get("label") == ": u32" for hint in hints), hints

            formatting_source = """//! Formatting fixture.
fn value(lhs:u32,rhs:u32)->u32{ret lhs+rhs}
fn main()->u32{
let expected:str="answer"
expected=1_u32
ret value(1_u32)
}
"""
            formatting_path = source_dir / "formatting.enq"
            formatting_path.write_text(formatting_source)
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {
                        "textDocument": {
                            "uri": formatting_path.as_uri(),
                            "languageId": "encore",
                            "version": 1,
                            "text": formatting_source,
                        }
                    },
                },
            )
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 4,
                    "method": "textDocument/formatting",
                    "params": {
                        "textDocument": {"uri": formatting_path.as_uri()},
                        "options": {"tabSize": 4, "insertSpaces": True},
                    },
                },
            )
            formatting = response_for(process, 4)["result"]
            assert len(formatting) == 1, formatting
            formatted = formatting[0]["newText"]
            assert "//! Formatting fixture." in formatted, formatted
            assert "fn value(lhs: u32, rhs: u32) -> u32 {" in formatted, formatted
            assert "  ret lhs + rhs" in formatted, formatted

            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 5,
                    "method": "textDocument/diagnostic",
                    "params": {"textDocument": {"uri": formatting_path.as_uri()}},
                },
            )
            diagnostics = response_for(process, 5)["result"]["items"]
            arity = [item for item in diagnostics if item.get("code") == "argument-mismatch"]
            assert len(arity) == 1, diagnostics
            assert "expects 2 argument(s), got 1" in arity[0]["message"], arity
            assignment = [item for item in diagnostics if item.get("code") == "assignment-type-mismatch"]
            assert len(assignment) == 1, diagnostics
            assert assignment[0].get("relatedInformation"), assignment
            assert assignment[0].get("data", {}).get("suggestions"), assignment
            assert "help:" in assignment[0]["message"], assignment

            decorator_source = """struct Profiler {}
impl for Profiler {
#attr(decorator)
fn profile[Args](self:Self,func:Callable[Args,u32],args:Args,label:str)->u32{ret func(args)}
}
static RENDER_PROFILE:Profiler=Profiler{}
@RENDER_PROFILE.profile("draw_frame")
fn draw_frame(value:u32)->u32{ret value}
"""
            decorator_path = source_dir / "decorator.enq"
            decorator_path.write_text(decorator_source)
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {
                        "textDocument": {
                            "uri": decorator_path.as_uri(),
                            "languageId": "encore",
                            "version": 1,
                            "text": decorator_source,
                        }
                    },
                },
            )
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 6,
                    "method": "textDocument/formatting",
                    "params": {
                        "textDocument": {"uri": decorator_path.as_uri()},
                        "options": {"tabSize": 2, "insertSpaces": True},
                    },
                },
            )
            decorator_formatting = response_for(process, 6)["result"]
            assert len(decorator_formatting) == 1, decorator_formatting
            decorated = decorator_formatting[0]["newText"]
            assert '#attr(decorator)' in decorated, decorated
            assert '@RENDER_PROFILE.profile("draw_frame")' in decorated, decorated
            assert "fn draw_frame(value: u32) -> u32 {" in decorated, decorated

            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 7,
                    "method": "textDocument/diagnostic",
                    "params": {"textDocument": {"uri": decorator_path.as_uri()}},
                },
            )
            decorator_diagnostics = response_for(process, 7)["result"]["items"]
            syntax_errors = [
                item
                for item in decorator_diagnostics
                if item.get("code") in {"unexpected-token", "unresolved-call", "unknown-type"}
            ]
            assert not syntax_errors, decorator_diagnostics

            send_message(process, {"jsonrpc": "2.0", "id": 8, "method": "shutdown", "params": None})
            response_for(process, 8)
            send_message(process, {"jsonrpc": "2.0", "method": "exit", "params": None})
            assert process.stdin is not None
            process.stdin.close()
            return_code = process.wait(timeout=5)
            if return_code != 0:
                assert process.stderr is not None
                raise RuntimeError(process.stderr.read().decode())
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()


if __name__ == "__main__":
    main()
    print("docstring hover and semantic inlay integration: ok")
