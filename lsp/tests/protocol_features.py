#!/usr/bin/env python3

"""Contract coverage for every LSP capability advertised by Encore."""

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


def request(process: subprocess.Popen[bytes], request_id: int, method: str, params: object) -> object:
    send(
        process,
        {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params},
    )
    while True:
        message = read(process)
        if message.get("id") == request_id:
            assert "error" not in message, message
            return message["result"]


def position_params(uri: str, line: int, character: int) -> dict:
    return {
        "textDocument": {"uri": uri},
        "position": {"line": line, "character": character},
    }


def main() -> None:
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="encore-lsp-protocol-") as temporary:
        root = Path(temporary)
        source_dir = root / "src"
        source_dir.mkdir()
        (root / "encore.toml").write_text(
            '[project]\nname = "protocol"\nversion = "0.1.0"\ndependencies = []\n'
        )
        helper_source = """//! Arithmetic helpers.

/// Adds two values.
pub fn add(lhs: u32, rhs: u32) -> u32 {
    ret lhs + rhs
}
"""
        main_source = """//! Protocol feature fixture.

import refrain::helper::add

/// Calls the arithmetic helper.
fn caller(value: u32) -> u32 {
    let result = add(value, 1_u32)
    ret result
}

fn main() -> u32 {
    ret caller(2_u32)
}
"""
        helper_path = source_dir / "helper.enq"
        main_path = source_dir / "main.enq"
        helper_path.write_text(helper_source)
        main_path.write_text(main_source)
        helper_uri = helper_path.as_uri()
        main_uri = main_path.as_uri()

        process = subprocess.Popen(
            [str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        next_id = 1

        def call(method: str, params: object) -> object:
            nonlocal next_id
            result = request(process, next_id, method, params)
            next_id += 1
            return result

        try:
            initialized = call(
                "initialize",
                {
                    "rootUri": root.as_uri(),
                    "workspaceFolders": [{"uri": root.as_uri(), "name": "protocol"}],
                    "initializationOptions": {"jobs": 2, "debounceMs": 0, "cache": False},
                },
            )
            capabilities = initialized["capabilities"]
            expected = {
                "definitionProvider",
                "declarationProvider",
                "implementationProvider",
                "hoverProvider",
                "referencesProvider",
                "documentHighlightProvider",
                "renameProvider",
                "signatureHelpProvider",
                "documentSymbolProvider",
                "workspaceSymbolProvider",
                "completionProvider",
                "inlayHintProvider",
                "documentLinkProvider",
                "foldingRangeProvider",
                "selectionRangeProvider",
                "semanticTokensProvider",
                "documentFormattingProvider",
                "documentRangeFormattingProvider",
                "callHierarchyProvider",
            }
            assert expected <= capabilities.keys(), capabilities
            send(process, {"jsonrpc": "2.0", "method": "initialized", "params": {}})
            for uri, text in ((helper_uri, helper_source), (main_uri, main_source)):
                send(
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

            diagnostics = call("textDocument/diagnostic", {"textDocument": {"uri": main_uri}})
            assert not any(item.get("severity") == 1 for item in diagnostics["items"]), diagnostics

            symbols = call("textDocument/documentSymbol", {"textDocument": {"uri": main_uri}})
            assert {"caller", "main"} <= {item["name"] for item in symbols}, symbols
            workspace_symbols = call("workspace/symbol", {"query": "call"})
            assert any(item["name"] == "caller" for item in workspace_symbols), workspace_symbols

            add_line = main_source.splitlines()[6]
            add_position = add_line.index("add") + 1
            add_params = position_params(main_uri, 6, add_position)
            for method in ("textDocument/definition", "textDocument/declaration"):
                locations = call(method, add_params)
                assert locations and locations[0]["uri"] == helper_uri, (method, locations)
            hover = call("textDocument/hover", add_params)
            assert "Adds two values." in hover["contents"]["value"], hover
            references = call(
                "textDocument/references", {**add_params, "context": {"includeDeclaration": True}}
            )
            assert any(item["uri"] == main_uri for item in references), references
            highlights = call("textDocument/documentHighlight", add_params)
            assert highlights, highlights

            prepared = call("textDocument/prepareRename", add_params)
            assert prepared["placeholder"] == "add", prepared
            renamed = call("textDocument/rename", {**add_params, "newName": "sum"})
            assert helper_uri in renamed["changes"] and main_uri in renamed["changes"], renamed

            completion = call(
                "textDocument/completion", position_params(main_uri, 6, add_line.index("add") + 3)
            )
            assert completion["items"], completion
            signature = call(
                "textDocument/signatureHelp",
                position_params(main_uri, 6, add_line.index("add(") + len("add(")),
            )
            assert signature["signatures"][0]["label"] == "add(lhs: u32, rhs: u32) -> u32", signature
            inlays = call(
                "textDocument/inlayHint",
                {
                    "textDocument": {"uri": main_uri},
                    "range": {
                        "start": {"line": 0, "character": 0},
                        "end": {"line": 20, "character": 0},
                    },
                },
            )
            assert any(item.get("label") == ": u32" for item in inlays), inlays

            links = call("textDocument/documentLink", {"textDocument": {"uri": main_uri}})
            assert links and links[0].get("target") == helper_uri, links
            folds = call("textDocument/foldingRange", {"textDocument": {"uri": main_uri}})
            assert len(folds) >= 2, folds
            selections = call(
                "textDocument/selectionRange",
                {"textDocument": {"uri": main_uri}, "positions": [{"line": 6, "character": add_position}]},
            )
            assert selections and selections[0]["range"], selections
            semantic = call("textDocument/semanticTokens/full", {"textDocument": {"uri": main_uri}})
            assert semantic["data"], semantic

            formatting = call(
                "textDocument/formatting",
                {"textDocument": {"uri": main_uri}, "options": {"tabSize": 4, "insertSpaces": True}},
            )
            assert isinstance(formatting, list), formatting
            range_formatting = call(
                "textDocument/rangeFormatting",
                {
                    "textDocument": {"uri": main_uri},
                    "range": {
                        "start": {"line": 4, "character": 0},
                        "end": {"line": 8, "character": 1},
                    },
                    "options": {"tabSize": 4, "insertSpaces": True},
                },
            )
            assert isinstance(range_formatting, list), range_formatting
            workspace_diagnostics = call("workspace/diagnostic", {})
            assert "items" in workspace_diagnostics, workspace_diagnostics

            caller_position = position_params(main_uri, 5, len("fn call"))
            hierarchy = call("textDocument/prepareCallHierarchy", caller_position)
            assert hierarchy and hierarchy[0]["name"] == "caller", hierarchy
            outgoing = call("callHierarchy/outgoingCalls", {"item": hierarchy[0]})
            assert any(item["to"]["name"] == "add" for item in outgoing), outgoing
            add_hierarchy = call("textDocument/prepareCallHierarchy", position_params(helper_uri, 3, 8))
            assert add_hierarchy, add_hierarchy
            incoming = call("callHierarchy/incomingCalls", {"item": add_hierarchy[0]})
            assert any(item["from"]["name"] == "caller" for item in incoming), incoming

            # Implementation requests are allowed to return an empty list for
            # symbols without an `impl`, but must obey the advertised contract.
            implementations = call("textDocument/implementation", add_params)
            assert isinstance(implementations, list), implementations

            send(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didClose",
                    "params": {"textDocument": {"uri": main_uri}},
                },
            )
            after_close = call("textDocument/documentSymbol", {"textDocument": {"uri": main_uri}})
            assert {"caller", "main"} <= {item["name"] for item in after_close}, after_close
        finally:
            if process.poll() is None:
                try:
                    call("shutdown", None)
                    send(process, {"jsonrpc": "2.0", "method": "exit", "params": None})
                    assert process.stdin is not None
                    process.stdin.close()
                    process.wait(timeout=5)
                except Exception:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    main()
    print("advertised LSP protocol feature contract: ok")
