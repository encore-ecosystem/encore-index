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
- Scoped autoimports offer public declarations from the current project and
  installed direct dependencies as `additionalTextEdits`. Existing bindings do
  not receive duplicate imports; insertions preserve module/item documentation
  and line endings. Unrelated workspace projects are not completion scope.
- `completionItem/resolve` loads declaration documentation on demand for clients
  supporting deferred documentation, with source-hash checks against stale items.
- `textDocument/codeAction` exposes machine-applicable diagnostic suggestions and
  import fixes for unknown types/names as versioned workspace edits.
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
  Editor graph loading is offline, including transitive and Git dependencies:
  missing packages report a recoverable error asking the user to run `encore sync`.
  Documentation rules (`missing-module-docstring` and
  `missing-public-docstring`) are opt-in through `[lint.rules]`.
- `textDocument/prepareCallHierarchy`, `callHierarchy/outgoingCalls`, `callHierarchy/incomingCalls` for callable symbols.
- Dynamic watching of `encore.toml` and `encore.lock`; package changes refresh
  workspace indexing and diagnostics without restarting the server.

The stdio thread owns protocol state and output. A bounded worker scheduler
defaults to half the available CPUs (rounded up), capped at four analysis tasks. It
analyzes documents, and revision checks discard stale results. Incremental text
sync and a short debounce keep typing responsive; cursor features use a fast
declaration outline while deeper diagnostics are pending.

Semantic highlighting has no document-size cutoff. A sequential lossless token
pass handles multiline comments, CRLF and UTF-16 coordinates, with a name index
instead of rescanning all declarations for every token. Results for unchanged
document revisions are reused; edits and close/reopen invalidate them. Interpolated
string syntax remains owned by Tree-sitter so semantic spans cannot cover embedded
expressions with one opaque string highlight.

Run `python3 tests/semantic_tokens.py target/release/lsp` for the large-document,
Unicode and cache-invalidation contract; an optional final argument sets the number
of generated functions (for example `2000`). The test prints request timings.

Validated analysis metadata is cached per project in
`.encore_cache/lsp/v1`. Cache files are atomically replaced and invalidated by
compiler version, module, source hash and source size. Configure the server with
initialization options `jobs`, `debounceMs`, `cache`, and `cacheDir`, or with
`ENCORE_LSP_JOBS`, `ENCORE_LSP_DEBOUNCE_MS`, `ENCORE_LSP_CACHE`, and
`ENCORE_LSP_CACHE_DIR`.

The shared frontend `AnalysisDatabase` owns applied results and performs
interface-aware dependant invalidation. Navigation resolves physical declaration
identities, including aliases, same-named types and enum variants.

## Current limitations

Version 0.1.0 does not claim complete semantic coverage of the language. Member
rename is intentionally unavailable until capture-safe edits can be guaranteed.
Enum payload signature help, complete qualified-expression type inference, and
fully structural type propagation remain follow-up work. Implementation lookup
and call hierarchy are less precise than declaration navigation.

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
python3 tests/autoimports.py target/release/lsp
python3 tests/offline_dependencies.py target/release/lsp
python3 tests/member_completion.py target/release/lsp
python3 tests/code_actions.py target/release/lsp
```

The integration tests use only the Python standard library. If future tests
gain Python dependencies, run and lock them with `uv`.
