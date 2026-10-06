# secret-store

OS-backed credentials. No files, shell commands or environment variables are
used for secret persistence. Keys should be canonical registry URLs, including
the index path. Values are nonempty UTF-8 strings without control characters,
at most 2048 bytes. Keys are at most 1024 bytes.

API: `get(key) -> Result[Option[str], StoreError]`,
`set(key, value)` and `delete(key)`. Missing credentials are distinct from
unavailable/locked stores, denial and backend failure.

Linux loads libsecret at runtime. macOS uses Keychain and Windows uses
Credential Manager. Absence of a keyring must never trigger a plaintext fallback.
Calls are synchronous and may prompt; use them on a CLI thread, not a UI/HTTP
event loop. Callers must not log or stringify returned credentials.

Native checks (Linux):

```sh
cc -std=c11 -Wall -Wextra -Werror tests/native.c -ldl -pthread -o target/keyring-test
target/keyring-test
DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent/encore-test target/keyring-test --unavailable
```

An opt-in real-backend test accepts `--roundtrip` and a unique key under
`https://encore-keyring-test.invalid/`. It creates, replaces and removes only
that test credential. Do not substitute a real registry key.
