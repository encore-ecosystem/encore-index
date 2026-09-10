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
- Typed `textDocument/signatureHelp` for ordinary and f-string calls in open documents and indexed workspace files.
- `textDocument/references` across open documents and indexed workspace files, using workspace import resolution for top-level symbols.
- `textDocument/documentHighlight` for same-document identifiers.
- `textDocument/prepareRename` and `textDocument/rename` across open documents and indexed workspace files, including import-based top-level symbol usages.
- `textDocument/documentSymbol` for `fn`, `struct`, `enum`, `trait`, `impl`.
- `workspace/symbol` over open documents and indexed sources for workspace projects activated by open files; project roots are discovered from `encore.toml`.
- `textDocument/completion` with Encore keywords, types, generics, packages,
  modules and imported declarations from open documents and indexed workspace
  files. Current-project `refrain`/`repo`, local `path@` dependencies and the
  installed package index share one resolver. Module and declaration candidates
  include their Markdown documentation and a concise documented summary.
  Parsed dependency metadata is reused in memory across completion keystrokes.
- `textDocument/documentLink` for import paths, including file targets resolved through workspace project/module discovery.
- `textDocument/foldingRange` for brace-delimited blocks.
- `textDocument/selectionRange` for identifier selections.
- `textDocument/semanticTokens/full` for lexical semantic highlighting.
- `textDocument/formatting` and `textDocument/rangeFormatting` through the same
  deterministic lossless formatter used by `encore format`.
- Push diagnostics for lexer, structural and semantic errors, including
  unresolved imports, unresolved calls, unknown types and call arity
  mismatches. The pull request remains supported for compatible clients but is
  not advertised, preventing editors from rendering every diagnostic twice.
  Typed diagnostics use the compiler's resolved `ProjectWorkspace`, including
  `core`, `std`, current-package modules and transitive path/index dependencies.
  Each open entrypoint owns an isolated immutable graph snapshot, so multi-root
  workspaces cannot mix packages with the same name.
  The native docstring analyzer reports `missing-module-docstring` by default;
  `missing-public-docstring` is available as an opt-in rule.
- `textDocument/prepareCallHierarchy`, `callHierarchy/outgoingCalls`, `callHierarchy/incomingCalls` for callable symbols.
- Dynamic watching of `encore.toml` and `encore.lock`; package changes refresh
  workspace indexing and diagnostics without restarting the server.

The stdio thread owns protocol state and output. A bounded `CPU - 1` worker scheduler
analyzes documents, and revision checks discard stale results. Incremental text
sync and a short debounce keep typing responsive; cursor features use a fast
declaration outline while deeper diagnostics are pending.

Validated analysis metadata is cached per project in
`.encore_cache/lsp/v1`. Cache files are atomically replaced and invalidated by
compiler version, module, source hash and source size. Configure the server with
initialization options `jobs`, `debounceMs`, `cache`, and `cacheDir`, or with
`ENCORE_LSP_JOBS`, `ENCORE_LSP_DEBOUNCE_MS`, `ENCORE_LSP_CACHE`, and
`ENCORE_LSP_CACHE_DIR`.

The shared frontend `AnalysisDatabase` still owns applied results and performs
interface-aware dependant invalidation. Navigation and rename use deterministic
frontend `ModuleId`/`SymbolId` identities.

## Source Layout

- `src/main.enq`: JSON-RPC server loop, request dispatch and feature handlers.
- `src/cache.enq`: validated persistent analysis metadata.
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
python3 tests/docstrings.py target/dev/lsp
python3 tests/scheduler.py target/dev/lsp
python3 tests/protocol_features.py target/dev/lsp
python3 tests/workspace_graph.py target/dev/lsp
```

The integration tests use only the Python standard library. If future tests
gain Python dependencies, run and lock them with `uv`.
