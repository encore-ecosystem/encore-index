#!/usr/bin/env python3

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time


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
            raise RuntimeError(
                f"LSP exited with {process.poll()}: {process.stderr.read().decode()}"
            )
        if line in (b"\n", b"\r\n"):
            break
        name, value = line.decode().split(":", 1)
        if name.lower() == "content-length":
            length = int(value)
    if length is None:
        raise RuntimeError("missing Content-Length")
    return json.loads(process.stdout.read(length))


def response(
    process: subprocess.Popen[bytes], request_id: int, observed: list[dict] | None = None
) -> dict:
    while True:
        message = read(process)
        if observed is not None:
            observed.append(message)
        if message.get("id") == request_id:
            return message


def publishes_code(messages: list[dict], code: str) -> bool:
    return any(
        message.get("method") == "textDocument/publishDiagnostics"
        and any(item.get("code") == code for item in message.get("params", {}).get("diagnostics", []))
        for message in messages
    )


def start(binary: Path, root: Path, cache: Path) -> subprocess.Popen[bytes]:
    process = subprocess.Popen(
        [str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    send(
        process,
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "rootUri": root.as_uri(),
                "initializationOptions": {
                    "jobs": 4,
                    "debounceMs": 0,
                    "cacheDir": str(cache),
                },
            },
        },
    )
    assert "error" not in response(process, 1)
    send(process, {"jsonrpc": "2.0", "method": "initialized", "params": {}})
    return process


def stop(process: subprocess.Popen[bytes]) -> None:
    send(process, {"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": None})
    response(process, 99)
    started = time.monotonic()
    send(process, {"jsonrpc": "2.0", "method": "exit", "params": None})
    process.wait(timeout=1)
    assert time.monotonic() - started < 1.0
    assert process.returncode == 0


def main() -> None:
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="encore-lsp-scheduler-") as temporary:
        root = Path(temporary)
        source_dir = root / "src"
        source_dir.mkdir()
        (root / "encore.toml").write_text(
            '[project]\nname = "scheduler"\nversion = "0.1.0"\ndependencies = []\n'
        )
        path = source_dir / "main.enq"
        large = "//! scheduler fixture\n" + "".join(
            f"fn function_{index}(value: u32) -> u32 {{ ret value + 1_u32 }}\n"
            for index in range(4000)
        )
        large += "fn main() -> u32 { ret function_3999(1_u32) }\n"
        changed = "//! changed revision\nfn main() -> u32 { ret missing() }\n"
        current = "//! current revision\nfn main() -> u32 { ret 0_u32 }\n"
        path.write_text(large)
        cache = root / "cache"
        process = start(binary, root, cache)
        uri = path.as_uri()
        try:
            started = time.monotonic()
            send(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {"textDocument": {"uri": uri, "version": 1, "text": large}},
                },
            )
            send(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 2,
                    "method": "textDocument/completion",
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": {"line": 4001, "character": 24},
                    },
                },
            )
            completion = response(process, 2)
            assert time.monotonic() - started < 2.0
            assert completion["result"]["items"]

            # A strict semantic request may be expensive, but it must not own
            # the protocol loop while an interactive request is waiting.
            send(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 6,
                    "method": "textDocument/diagnostic",
                    "params": {"textDocument": {"uri": uri}},
                },
            )
            started = time.monotonic()
            send(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 7,
                    "method": "textDocument/completion",
                    "params": {
                        "textDocument": {"uri": uri},
                        "position": {"line": 4001, "character": 24},
                    },
                },
            )
            assert response(process, 7)["result"]["items"]
            assert time.monotonic() - started < 2.0

            send(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": uri, "version": 2},
                        "contentChanges": [{"text": changed}],
                    },
                },
            )

            # Semantic diagnostics are pushed automatically after the fast
            # tolerant result, without requiring a pull-diagnostic request.
            pushed_semantic = False
            for attempt in range(50):
                request_id = 30 + attempt
                send(
                    process,
                    {
                        "jsonrpc": "2.0",
                        "id": request_id,
                        "method": "textDocument/completion",
                        "params": {
                            "textDocument": {"uri": uri},
                            "position": {"line": 1, "character": 20},
                        },
                    },
                )
                observed: list[dict] = []
                response(process, request_id, observed)
                if publishes_code(observed, "unresolved-call"):
                    pushed_semantic = True
                    break
                time.sleep(0.01)
            assert pushed_semantic, "semantic diagnostics were not pushed"

            send(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 3,
                    "method": "textDocument/diagnostic",
                    "params": {"textDocument": {"uri": uri}},
                },
            )
            diagnostics = response(process, 3)["result"]["items"]
            assert any(item.get("code") == "unresolved-call" for item in diagnostics)

            send(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didChange",
                    "params": {
                        "textDocument": {"uri": uri, "version": 3},
                        "contentChanges": [{"text": current}],
                    },
                },
            )
            path.write_text(current)
            send(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didSave",
                    "params": {"textDocument": {"uri": uri}, "text": current},
                },
            )
            send(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 5,
                    "method": "textDocument/diagnostic",
                    "params": {"textDocument": {"uri": uri}},
                },
            )
            response(process, 5)
            assert list(cache.glob("*.json")), "analysis cache was not written"
        finally:
            if process.poll() is None:
                stop(process)

        warm = start(binary, root, cache)
        try:
            send(
                warm,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {"textDocument": {"uri": uri, "version": 1, "text": current}},
                },
            )
            send(
                warm,
                {
                    "jsonrpc": "2.0",
                    "id": 4,
                    "method": "textDocument/diagnostic",
                    "params": {"textDocument": {"uri": uri}},
                },
            )
            assert response(warm, 4)["result"]["items"] == []
        finally:
            if warm.poll() is None:
                stop(warm)

        # Compatibility versions do not identify parser behavior: another
        # named release must rebuild the cached document.
        cache_file = next(cache.glob("*.json"))
        cached = json.loads(cache_file.read_text())
        release = (Path(__file__).resolve().parents[3] / "encore" / "VERSION").read_text().strip()
        assert cached["compiler"] == release
        cached["compiler"] = "0.0.0-other-release"
        cache_file.write_text(json.dumps(cached))
        refreshed = start(binary, root, cache)
        try:
            send(refreshed, {
                "jsonrpc": "2.0", "method": "textDocument/didOpen",
                "params": {"textDocument": {"uri": uri, "version": 1, "text": current}},
            })
            send(refreshed, {
                "jsonrpc": "2.0", "id": 9, "method": "textDocument/diagnostic",
                "params": {"textDocument": {"uri": uri}},
            })
            assert response(refreshed, 9)["result"]["items"] == []
            assert json.loads(cache_file.read_text())["compiler"] == release
        finally:
            if refreshed.poll() is None:
                stop(refreshed)

        # A partial or externally damaged cache entry is a miss, never a
        # protocol failure, and the worker replaces it atomically.
        cache_file = next(cache.glob("*.json"))
        cache_file.write_text("{")
        recovered = start(binary, root, cache)
        try:
            send(
                recovered,
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {"textDocument": {"uri": uri, "version": 1, "text": current}},
                },
            )
            send(
                recovered,
                {
                    "jsonrpc": "2.0",
                    "id": 8,
                    "method": "textDocument/diagnostic",
                    "params": {"textDocument": {"uri": uri}},
                },
            )
            assert response(recovered, 8)["result"]["items"] == []
            assert json.loads(cache_file.read_text())["schema"] == "encore-lsp-analysis-v1"
        finally:
            if recovered.poll() is None:
                stop(recovered)

    print("nonblocking scheduler, revisions, and persistent cache recovery: ok")


if __name__ == "__main__":
    main()
