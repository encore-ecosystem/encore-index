# Encore LSP

An Encore language server implemented in Encore.

Current server capabilities:

- JSON-RPC framing over stdio.
- `initialize`, `shutdown`, `exit`.
- Multi-root workspaces and `workspace/didChangeWorkspaceFolders`.
- `textDocument/didOpen`, `textDocument/didChange`, `textDocument/didSave`, `textDocument/didClose`.
- `textDocument/definition` and `textDocument/declaration` for symbols in open documents, indexed workspace files and resolved workspace imports.
- `textDocument/implementation` for `impl Type` / `impl Trait` symbols in open documents and indexed workspace files.
- `textDocument/hover` for identifiers and declarations from open documents, indexed workspace files and resolved workspace imports.
- Markdown hover includes structured `///` documentation for local and imported
  declarations.
- `textDocument/signatureHelp` for function calls in open documents and indexed workspace files.
- `textDocument/references` across open documents and indexed workspace files, using workspace import resolution for top-level symbols.
- `textDocument/documentHighlight` for same-document identifiers.
- `textDocument/prepareRename` and `textDocument/rename` across open documents and indexed workspace files, including import-based top-level symbol usages.
- `textDocument/documentSymbol` for `fn`, `struct`, `enum`, `trait`, `impl`.
- `workspace/symbol` over open documents and indexed sources for workspace projects activated by open files; project roots are discovered from `encore.toml`.
- `textDocument/completion` with Encore keywords and declarations from open documents and indexed workspace files.
- `textDocument/documentLink` for import paths, including file targets resolved through workspace project/module discovery.
- `textDocument/foldingRange` for brace-delimited blocks.
- `textDocument/selectionRange` for identifier selections.
- `textDocument/semanticTokens/full` for lexical semantic highlighting.
- `textDocument/formatting` and `textDocument/rangeFormatting` through the same
  deterministic lossless formatter used by `encore format`.
- Push and per-document pull diagnostics for lexer, structural and basic semantic errors, including unresolved imports, unresolved calls, unknown types and call arity mismatches. The native docstring analyzer reports `missing-module-docstring` by default; `missing-public-docstring` is available as an opt-in rule.
- `textDocument/prepareCallHierarchy`, `callHierarchy/outgoingCalls`, `callHierarchy/incomingCalls` for callable symbols.

Document analysis is stored in the shared frontend `AnalysisDatabase`. Source
updates carry revisions and invalidate modules that depend on the changed
module; the LSP requests the tolerant lightweight view needed while code is
temporarily incomplete. Body-only edits retain dependant results because
invalidation follows declaration-interface fingerprints. Navigation and rename
use deterministic frontend `ModuleId`/`SymbolId` identities instead of
URI-and-name identities. Function-level semantic queries provide the same
typed diagnostics used during compiler checking. Inlay hints first consult
their inferred binding types, including return types obtained through imported
function calls, and retain the tolerant token inference as an incomplete-code
fallback.

## Source Layout

- `src/main.enq`: JSON-RPC server loop, request dispatch and feature handlers.
- `src/protocol.enq`: LSP framing, response/notification writers and JSON/LSP builders.
- `src/paths.enq`: file URI and path helpers.
- `src/text.enq`: text scanning, range support and identifier utilities.
- `src/tokens.enq`: Encore token classification helpers for semantic tokens.

Build and run with the installed compiler from this directory:

```sh
encore build --profile release
./target/release/lsp
```

Install the editor-facing binary with the stable launcher name:

```sh
encore install --path . --name encore-lsp --force --profile release
```

The server communicates over stdio. Configure an editor client with the
absolute path to `target/release/lsp`; no Python runtime or wrapper process is
required. LSP positions and ranges use UTF-16 as required by the protocol, and
message `Content-Length` is measured in UTF-8 bytes.

Run the protocol and feature integration suites with Python 3:

```sh
python3 tests/docstrings.py target/debug/lsp
```

The integration tests use only the Python standard library. If future tests
gain Python dependencies, run and lock them with `uv`.
