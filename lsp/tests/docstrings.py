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
        local_dependency = root / ".localdocs"
        (local_dependency / "src" / "widget").mkdir(parents=True)
        (local_dependency / "encore.toml").write_text(
            '[project]\nname = "localdocs"\nversion = "0.1.0"\n'
            'description = "Local documentation fixture"\ndependencies = []\n'
        )
        (local_dependency / "src" / "lib.enq").write_text(
            "//! Local dependency used by editor tests.\n\npub import refrain::widget\n"
        )
        (local_dependency / "src" / "widget" / "mod.enq").write_text(
            "//! Widgets from the local path dependency.\n"
        )
        (root / "encore.toml").write_text(
            '[project]\nname = "doc_hover"\nversion = "0.1.0"\n'
            'dependencies = ["path@.localdocs"]\n'
            '\n[format]\nindent-width = 2\n'
        )

        net_source = """//! Networking primitives.

/// Opens a connection.
///
/// # Errors
/// Returns an error when the peer is unavailable.
pub fn connect(host: str, timeout: u32) -> u32 { ret timeout }
"""
        main_source = """import doc_hover::net::connect
fn main() -> u32 {
    let answer = connect("localhost", 5_u32)
    ret answer
}
"""
        net_path = source_dir / "net.enq"
        main_path = source_dir / "main.enq"
        api_dir = source_dir / "api"
        api_dir.mkdir()
        api_module_path = api_dir / "mod.enq"
        api_types_path = api_dir / "types.enq"
        net_path.write_text(net_source)
        main_path.write_text(main_source)
        api_module_path.write_text(
            "//! Public API types.\n\npub import mod::types::*\n"
        )
        api_types_path.write_text(
            "//! API type declarations.\n\n/// Describes an API document.\npub struct Documentation { value: str }\n"
        )

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
            expected_release = (
                Path(__file__).resolve().parents[3] / "encore" / "VERSION"
            ).read_text().strip()
            assert initialized["result"]["serverInfo"]["version"] == expected_release, initialized
            assert "diagnosticProvider" not in initialized["result"]["capabilities"], initialized
            assert initialized["result"]["capabilities"]["textDocumentSync"]["change"] == 2, initialized
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

            import_path = source_dir / "imports.enq"
            import_source = "import st\n"
            import_path.write_text(import_source)
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {
                        "textDocument": {
                            "uri": import_path.as_uri(),
                            "languageId": "encore",
                            "version": 1,
                            "text": import_source,
                        }
                    },
                },
            )
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 20,
                    "method": "textDocument/completion",
                    "params": {
                        "textDocument": {"uri": import_path.as_uri()},
                        "position": {"line": 0, "character": len("import st")},
                    },
                },
            )
            package_labels = {
                item["label"] for item in response_for(process, 20)["result"]["items"]
            }
            assert "std" in package_labels, package_labels

            import_source = "import std::\n"
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": import_path.as_uri(), "version": 2},
                        "contentChanges": [
                            {
                                "range": {
                                    "start": {"line": 0, "character": len("import st")},
                                    "end": {"line": 0, "character": len("import st")},
                                },
                                "text": "d::",
                            }
                        ],
                    },
                },
            )
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 21,
                    "method": "textDocument/completion",
                    "params": {
                        "textDocument": {"uri": import_path.as_uri()},
                        "position": {"line": 0, "character": len("import std::")},
                    },
                },
            )
            module_labels = {
                item["label"] for item in response_for(process, 21)["result"]["items"]
            }
            assert {"io", "vec", "string"} <= module_labels, module_labels

            local_source = "import localdocs::\n"
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": import_path.as_uri(), "version": 3},
                        "contentChanges": [{"text": local_source}],
                    },
                },
            )
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 27,
                    "method": "textDocument/completion",
                    "params": {
                        "textDocument": {"uri": import_path.as_uri()},
                        "position": {"line": 0, "character": len("import localdocs::")},
                    },
                },
            )
            local_items = response_for(process, 27)["result"]["items"]
            local_widget = next(item for item in local_items if item["label"] == "widget")
            assert local_widget["detail"] == "Widgets from the local path dependency.", local_widget

            # Completion must resolve the current project through Encore's
            # virtual `refrain` package and carry module documentation rather
            # than a generic UI placeholder.
            refrain_source = "import refrain::\n"
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": import_path.as_uri(), "version": 4},
                        "contentChanges": [{"text": refrain_source}],
                    },
                },
            )
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 23,
                    "method": "textDocument/completion",
                    "params": {
                        "textDocument": {"uri": import_path.as_uri()},
                        "position": {"line": 0, "character": len("import refrain::")},
                    },
                },
            )
            refrain_items = response_for(process, 23)["result"]["items"]
            net_module = next(item for item in refrain_items if item["label"] == "net")
            assert net_module["detail"] == "Networking primitives.", net_module
            assert net_module["documentation"] == {
                "kind": "markdown",
                "value": "Networking primitives.",
            }, net_module

            refrain_symbol_source = "import refrain::net::\n"
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": import_path.as_uri(), "version": 5},
                        "contentChanges": [{"text": refrain_symbol_source}],
                    },
                },
            )
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 24,
                    "method": "textDocument/completion",
                    "params": {
                        "textDocument": {"uri": import_path.as_uri()},
                        "position": {"line": 0, "character": len("import refrain::net::")},
                    },
                },
            )
            refrain_symbols = response_for(process, 24)["result"]["items"]
            connect_item = next(item for item in refrain_symbols if item["label"] == "connect")
            assert connect_item["detail"] == "Opens a connection.", connect_item
            assert "# Errors" in connect_item["documentation"]["value"], connect_item

            grouped_source = "import refrain::api::{Doc\n"
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": import_path.as_uri(), "version": 6},
                        "contentChanges": [{"text": grouped_source}],
                    },
                },
            )
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 26,
                    "method": "textDocument/completion",
                    "params": {
                        "textDocument": {"uri": import_path.as_uri()},
                        "position": {"line": 0, "character": len(grouped_source) - 1},
                    },
                },
            )
            grouped_items = response_for(process, 26)["result"]["items"]
            documentation_item = next(
                item for item in grouped_items if item["label"] == "Documentation"
            )
            assert documentation_item["detail"] == "Describes an API document.", documentation_item

            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 25,
                    "method": "textDocument/hover",
                    "params": {
                        "textDocument": {"uri": import_path.as_uri()},
                        "position": {"line": 0, "character": len("import refrain::ap")},
                    },
                },
            )
            module_hover = response_for(process, 25)["result"]
            assert "refrain::api" in module_hover["contents"]["value"], module_hover
            assert "Public API types." in module_hover["contents"]["value"], module_hover

            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": import_path.as_uri(), "version": 7},
                        "contentChanges": [{"text": "import std::\n"}],
                    },
                },
            )

            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 22,
                    "method": "textDocument/hover",
                    "params": {
                        "textDocument": {"uri": import_path.as_uri()},
                        "position": {"line": 0, "character": len("import st")},
                    },
                },
            )
            package_hover = response_for(process, 22)["result"]
            assert package_hover is not None, package_hover
            assert "std" in package_hover["contents"]["value"], package_hover
            assert "Encore package" in package_hover["contents"]["value"], package_hover

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

            call_line = main_source.splitlines()[2]
            open_character = call_line.index("connect(") + len("connect(")
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 12,
                    "method": "textDocument/signatureHelp",
                    "params": {
                        "textDocument": {"uri": main_path.as_uri()},
                        "position": {"line": 2, "character": open_character},
                    },
                },
            )
            signature = response_for(process, 12)["result"]
            assert signature["signatures"][0]["label"] == (
                "connect(host: str, timeout: u32) -> u32"
            ), signature
            assert signature["activeParameter"] == 0, signature

            comma_character = call_line.index(",") + 1
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 13,
                    "method": "textDocument/signatureHelp",
                    "params": {
                        "textDocument": {"uri": main_path.as_uri()},
                        "position": {"line": 2, "character": comma_character},
                    },
                },
            )
            after_comma = response_for(process, 13)["result"]
            assert after_comma["activeParameter"] == 1, after_comma

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
            if arity:
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
                    "method": "textDocument/completion",
                    "params": {
                        "textDocument": {"uri": decorator_path.as_uri()},
                        "position": {"line": 7, "character": 0},
                    },
                },
            )
            completion = response_for(process, 7)["result"]
            labels = {item["label"] for item in completion["items"]}
            assert {"async", "await", "spawn", "static"} <= labels, labels

            type_source = """fn typed[T](value: T, next: u) -> T { ret value }
fn main() -> u32 { let value = typed(1_u32, 2_u32) ret value }
"""
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": decorator_path.as_uri(), "version": 2},
                        "contentChanges": [{"text": type_source}],
                    },
                },
            )
            type_character = type_source.splitlines()[0].index("next: u") + len("next: u")
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 14,
                    "method": "textDocument/completion",
                    "params": {
                        "textDocument": {"uri": decorator_path.as_uri()},
                        "position": {"line": 0, "character": type_character},
                    },
                },
            )
            type_items = response_for(process, 14)["result"]["items"]
            type_labels = {item["label"] for item in type_items}
            assert {"u8", "u32", "usize", "str", "T"} <= type_labels, type_labels

            fstring_source = """fn my_add(a: usize, b: usize) -> usize { ret a + b }
fn main() -> u32 { let shown = f"value={my_add(1_usize, 2_usize)}" ret 0_u32 }
"""
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": decorator_path.as_uri(), "version": 3},
                        "contentChanges": [{"text": fstring_source}],
                    },
                },
            )
            fstring_line = fstring_source.splitlines()[1]
            fstring_character = fstring_line.index("my_add(") + len("my_add(")
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 15,
                    "method": "textDocument/signatureHelp",
                    "params": {
                        "textDocument": {"uri": decorator_path.as_uri()},
                        "position": {"line": 1, "character": fstring_character},
                    },
                },
            )
            fstring_signature = response_for(process, 15)["result"]
            assert fstring_signature["signatures"][0]["label"] == (
                "my_add(a: usize, b: usize) -> usize"
            ), fstring_signature

            # Incomplete buffers are normal while typing. They must still
            # complete keywords without taking the language server down.
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": decorator_path.as_uri(), "version": 4},
                        "contentChanges": [{"text": "i"}],
                    },
                },
            )
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 8,
                    "method": "textDocument/completion",
                    "params": {
                        "textDocument": {"uri": decorator_path.as_uri()},
                        "position": {"line": 0, "character": 1},
                    },
                },
            )
            partial_labels = {
                item["label"] for item in response_for(process, 8)["result"]["items"]
            }
            assert "import" in partial_labels, partial_labels

            # Import diagnostics must follow the manifest after `encore add`
            # without requiring an editor or LSP restart.
            import_source = (
                "import std::io::println\n"
                'fn main() -> u32 { println("ready") ret 0_u32 }\n'
            )
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": decorator_path.as_uri(), "version": 5},
                        "contentChanges": [{"text": import_source}],
                    },
                },
            )
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 9,
                    "method": "textDocument/diagnostic",
                    "params": {"textDocument": {"uri": decorator_path.as_uri()}},
                },
            )
            before_add = response_for(process, 9)["result"]["items"]
            assert any(item.get("code") == "unresolved-import" for item in before_add), before_add

            (root / "encore.toml").write_text(
                '[project]\nname = "doc_hover"\nversion = "0.1.0"\n'
                'dependencies = ["index@std"]\n\n[format]\nindent-width = 2\n'
            )
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "workspace/didChangeWatchedFiles",
                    "params": {
                        "changes": [
                            {"uri": (root / "encore.toml").as_uri(), "type": 2}
                        ]
                    },
                },
            )
            send_message(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 10,
                    "method": "textDocument/diagnostic",
                    "params": {"textDocument": {"uri": decorator_path.as_uri()}},
                },
            )
            after_add = response_for(process, 10)["result"]["items"]
            assert not any(
                item.get("code") == "unresolved-import" for item in after_add
            ), after_add
            assert not any(
                item.get("code") == "unresolved-call" for item in after_add
            ), after_add

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

            cache_files = list((root / ".encore_cache" / "lsp" / "v1").glob("*.json"))
            assert cache_files, "LSP did not persist its analysis cache"

            send_message(process, {"jsonrpc": "2.0", "id": 11, "method": "shutdown", "params": None})
            response_for(process, 11)
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
            elif process.returncode != 0 and process.stderr is not None:
                error = process.stderr.read().decode()
                if error:
                    print(error, file=sys.stderr)


if __name__ == "__main__":
    main()
    print("docstring hover and semantic inlay integration: ok")
