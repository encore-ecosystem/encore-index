# Encore Index

Public refrain registry: Encore + Blaze, SQLite, immutable local archives.
Publication, ownership, search, paginated HTML catalogue, yank/unyank, audit,
offline backups and API-based import. Reading packages requires no account;
browser sign-in uses GitHub, with offline administration available separately.

## Settings and compatibility

New installations use `ENCORE_INDEX_DATA`, `ENCORE_INDEX_PORT`,
`ENCORE_INDEX_PUBLIC_URL`, `ENCORE_INDEX_CSS`, and (for maintenance clients)
`ENCORE_INDEX_TOKEN`. GitHub sign-in uses `ENCORE_INDEX_GITHUB_CLIENT_ID` and
`ENCORE_INDEX_GITHUB_CLIENT_SECRET`.

The old `ENCORE_REGISTRY_DATA/PORT/PUBLIC_URL/CSS/TOKEN` names and
`ENCORE_GITHUB_CLIENT_ID/SECRET` remain aliases. A nonempty new value wins;
empty values are unset. Without a public URL, the origin is derived from the
selected local port. The old `/assets/registry.css` URL remains an alias.

The compiler independently selects its registry through `ENCORE_INDEX_URL`,
`[registry].index`, or `ENCORE_DEFAULT_INDEX`. Its publication credentials still
use the compiler's `ENCORE_REGISTRY_TOKEN` and `ENCORE_REGISTRY_TOKEN_INDEX`
contract; the service rename does not change that compiler interface.

## Local package catalogue

The initial fresh catalogue contains `std 0.0.8`, `json 0.0.3`, `toml 0.0.0`,
`sqlite 0.1.0`, and `blaze 0.1.0-beta.2`. Archives, database and credentials are
runtime data and are never committed. No old users or publication history are
imported. Package source repositories remain independent of this service.

For source builds, run a compatible index containing those dependencies and set:

```sh
export ENCORE_DEFAULT_INDEX="http://127.0.0.1:8137/index"
```

## Run

Requires Linux, Encore >=0.1.6 and OpenSSL/libarchive development libraries.
SQLite is bundled by the `sqlite` refrain; no system SQLite development package
is required. The current std also requires OpenSSL and c-ares >=1.34 headers/libraries. Dependencies are pinned in `encore.lock` and fetched through the configured registry.
The source repository contains the service, not package metadata or package source mirrors.

```sh
encore sync
encore build
export ENCORE_INDEX_DATA="$PWD/data"
target/dev/encore-index init
target/dev/encore-index user-add alice
target/dev/encore-index token-create alice
target/dev/encore-index reserve std alice
target/dev/encore-index serve
```

Listens on **127.0.0.1:8137** (`ENCORE_INDEX_PORT` overrides the port).
For remote use, put an HTTPS reverse proxy in front
and set `ENCORE_INDEX_PUBLIC_URL=https://packages.example.org` (origin, no
trailing slash). Configure proxy rate/connection limits; never log Authorization
headers. TLS termination and remote deployment are outside this repository.

The interface uses server-rendered HTML and local CSS, with one small local
JavaScript file for copying install commands. No JS framework, web fonts or CDN.
Without JavaScript the commands remain readable and navigation still works.
Light/dark themes follow the OS: graphite, off-white and red accents.
Run from the project directory or set `ENCORE_INDEX_CSS` to the absolute path
of `assets/index.css`. Styles are loaded once at startup; restart after CSS
changes. A missing/empty stylesheet is a startup error. Browsers revalidate it
on navigation rather than retaining an immutable cached version.

Admin commands run **offline**. Stop the service first; a filesystem lock
prevents concurrent instances/admin writes. `token-create` prints its secret
once; SQLite stores only its SHA-256 digest. `token-revoke ID` takes a database
token ID. Data, staging and archive directories must be owned by the current
user with mode 0700. Reservations protect system names from other owners.

### Browser and device authentication

Create a GitHub OAuth App with callback
`https://packages.example.org/auth/github/callback`. Set
`ENCORE_INDEX_GITHUB_CLIENT_ID` and `ENCORE_INDEX_GITHUB_CLIENT_SECRET` securely in the
server environment, alongside `ENCORE_INDEX_PUBLIC_URL`. Never put the
secret in source control, command arguments or logs. Restart the service after
configuration changes. Auth discovery is advertised only when both credentials
are configured. Without them, the package service and offline administration
remain available.

`/account` signs in through GitHub using PKCE and a browser-bound, single-use
state. Identity is keyed by immutable GitHub ID, not login name. Existing owners
can be linked explicitly with offline `github-link OWNER GITHUB_ID`; login never
claims an existing owner merely because names match. The account page lists and
revokes credentials, creates package-scoped CI tokens, and approves device codes
at `/auth/device`. CI tokens are displayed once. All browser mutations require
the session's CSRF value and the configured Origin. HTTPS cookies are Secure,
HttpOnly, SameSite=Lax and use the `__Host-` prefix.

Device endpoints implement one-time grants, expiry and polling backoff. Browser
sessions cannot be used as API bearer credentials. Device credentials expire
after 90 days (30 days idle), browser sessions after 30 days (7 days idle), and
CI tokens after at most 90 days. Credential secrets are stored only as hashes;
short-lived PKCE verifiers are retained until callback consumption or expiry.
Provider access tokens are neither persisted nor returned to clients.

OAuth exchange uses two bounded workers and HTTPS with redirects disabled.
Auth endpoints have process-wide rate limits as well as per-device polling
limits; configure per-IP limits at the reverse proxy. Do not log callback query
strings, cookies or authorization headers.

The new server upgrades schema 1/2 to schema 3 on startup, under the exclusive
service lock. Back up the data **before** upgrading. Package ownership and
archives are preserved. Existing admin tokens receive a 30-day migration
deadline; newly issued legacy admin tokens also expire after 30 days.
Do not run an older server on a migrated database; restore a pre-upgrade backup
to a separate directory if rollback is needed.

Implemented and locally tested foundations:

- Scoped device/browser/CI credential storage, expiry and immediate revocation.
- Atomic one-time device approval/polling storage and CI package restrictions.
- Publication rechecks authentication and scope inside the commit transaction.
- Schema migration and schema-3 backup/restore.
- In the standard library: HTTPS `HttpClient.send` without redirect forwarding, and a
  new `secret-store` package using native OS keyrings without file fallback.

The matching compiler includes `encore login/whoami/logout`, OS-keyring
integration and native HTTP publication. Plain HTTP uses deadline-bounded IPv4
loopback connections without DNS. These changes are local and unreleased;
the globally installed compiler has not been changed. Native keyring roundtrips
on all three platforms and a real GitHub OAuth App smoke test remain release
gates. Local OAuth coverage uses a disposable TLS provider, not a real GitHub
account. CLI integration tests substitute a private test-only libsecret provider;
they do not prove integration with the user's desktop keyring.

Before deployment, also finish total-deadline enforcement across DNS and TLS
handshake: the current TLS backend bounds individual blocking operations, while
the new plaintext loopback transport already uses a single monotonic deadline.
Do not treat the local passing tests as a completed cross-platform release audit.

## Publish and consume

Use the matching Encore compiler, for example `../encore/target/release/encore`. Packaging requires Git and tar; authenticated
publication no longer invokes curl.

```toml
[project]
name = "example_math"
version = "1.0.0"
encore = ">=0.0.0"
dependencies = []

[registry]
index = "http://127.0.0.1:8137/index"
```

```sh
git init
git add encore.toml src
# Run check/test for your package before publishing.
encore publish --dry-run
encore login
encore whoami
encore publish
encore logout
```

For compiler publication in CI, inject `ENCORE_REGISTRY_TOKEN` through the secret manager and bind it
with `ENCORE_REGISTRY_TOKEN_INDEX=http://127.0.0.1:8137/index` (use the HTTPS
index URL in production). Without a matching explicit binding the CLI refuses
to send the token. Offline legacy admin tokens require the same binding.

Only tracked files selected by `[publish].include`/`exclude` and `.encoreignore`
are packaged. No remote, commit, clean worktree or push is required. Deterministic
archives rewrite root `path@` dependencies to exact index versions in staging
only. Nested manifests cannot retain path/Git dependencies. Embedded
`workspace@name` requires `workspace/name/encore.toml` in the archive; workspace
aggregation manifests are rejected. Dry run checks archive structure without
network requests; it does not run package tests. The server never executes
uploaded source, native code or build scripts.

Configure the same registry in a consumer, then `encore add example_math` and
`encore build`. Root configuration or `ENCORE_INDEX_URL` selects one index for
the whole graph. Locks/cache namespaces pin its origin; changing it requires
`encore update`. There is no GitHub sparse-index fallback.

## API

- `GET /index/config.json`: schema-1 discovery; same-origin `/api/v1`.
- `GET /api/v1/packages?q=substring&after=last-name`: sorted array, at most 100
  entries. Continue with the last name. `/` offers HTML pagination;
  `/packages/{name}` displays versions and compiler requirements.
- `GET /index/{first-two-letters}/{name}.json`: sparse metadata.
- `POST /api/v1/uploads`: bearer token, returns upload ID.
- `PUT /api/v1/uploads/{id}/{byte-offset}`: binary chunk, at most 64 KiB.
- `POST /api/v1/uploads/{id}/finish`: `{"checksum":"...","nametag":"neumann"}`.
- `DELETE /api/v1/uploads/{id}`: cancel staging and release the owner's quota;
  committed package versions are never removed. The CLI attempts this on failure.
- `POST /api/v1/packages/{name}/{version}/yank` or `/unyank`: owner token.
- `GET /archives/{sha256}.tar.gz`: immutable download, including yanked versions.

Name + SemVer is immutable. Same bytes are idempotent; different bytes return
409. Repeated chunks are compared byte-for-byte. Restarting `encore publish`
recreates the same archive and is safe after an uncertain response. Upload IDs
expire after 15 minutes or service restart; then restart publication.

Chunks go to private staging files. A dedicated worker validates tar without
extraction: traversal, links, devices, sparse files, Windows-reserved paths,
case-insensitive collisions, duplicate members/manifest keys and expansion
bombs are rejected. The immutable archive is linked and fsynced **before** the
SQLite transaction commits. Failed commits may leave unreferenced files, never
visible metadata pointing to partial uploads. Startup clears abandoned staging;
snapshots omit unreferenced archives. SQLite uses WAL + synchronous FULL.

GitHub login creates an account for its immutable provider ID; offline
administration remains available. Private packages are not supported.

### Resource limits

Archives: **4 MiB compressed / 64 MiB expanded**, 4096 members, 512 KiB manifest
JSON (128 manifests, at most 64 KiB each). Descriptions: 1 KiB; cumulative version
metadata per package: 512 KiB. SQLite value/row limit: 1 MiB.
Staging: 16 uploads, at most 4 per user. Queue: 32 jobs; one Encore storage
worker owns the connection, one native worker validates archives and two
Encore workers perform OAuth network requests without holding DB transactions.
HTTP: one worker, 256 connections, 64 KiB request bodies. Uploads
are incremental; downloads use bounded buffers, not zero-copy streaming.
Abandoned completed jobs expire after 60 seconds. Overload returns 429/503.

Blaze sweeps pending futures every 10 ms while any are pending; it is not yet a
per-future wake-driven executor. Slow handlers do not block other connections
or cause unread pipelined requests to busy-spin.

## Backup, restore, import

Python >=3.11 is needed only for maintenance/tests.

```sh
# Stop the service. Destinations must not exist.
python3 maintenance.py backup data /path/to/new-snapshot
python3 maintenance.py restore /path/to/new-snapshot /path/to/new-data
# Set ENCORE_INDEX_DATA to new-data and start the service.

# Running service, owner token in ENCORE_INDEX_TOKEN:
python3 maintenance.py import /path/to/index --url http://127.0.0.1:8137 --package std
```

Snapshots contain token hashes; keep them private. Backup/restore check SQLite
integrity and every archive hash. Restore refuses existing destinations and
incomplete/corrupt snapshots. Import checks archive checksum/identity, then uses
the normal publication API and preserves yanks. It never bypasses ownership or
archive checks. Reserve names and publish dependencies before switching clients.
No official index, releases or deployment have been modified.

## Verify

```sh
encore format --check
encore check
encore lint
encore test
encore build
python3 tests/settings.py target/dev/encore-index
python3 tests/integration.py target/dev/encore-index ../encore/target/extreme/encore
python3 tests/publication.py target/dev/encore-index ../encore/target/extreme/encore
# Against the running demo (Playwright is only a testing dependency):
uv run --with playwright python tests/browser.py --package demo_math
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-function -g -fsanitize=address,undefined \
  tests/transport.c -lcrypto -larchive -pthread -o target/transport-test
fixture=$(mktemp -d)
ASAN_OPTIONS=detect_leaks=1 target/transport-test "$fixture"
# Encore storage_* tests cover SQL, ownership, credentials and publication.
# Compare migration with the saved pre-migration native executable:
python3 tests/sqlite-migration.py /path/to/previous-registry target/dev/encore-index
# Against the running index: resolve SQLite in an empty cache and run a transaction.
python3 tests/sqlite-consumer.py ../encore/target/dev/encore
# Disposable server: upload and byte-for-byte download a bundled SQLite archive.
ENCORE_INDEX_PORT=18140 python3 tests/large-publication.py target/dev/encore-index /path/to/sqlite-0.1.0.tar.gz
# Optional migration check; the first binary must still use schema 1:
python3 tests/auth-migration.py /path/to/schema-1-registry target/dev/encore-index
# Builds a disposable test-only binary and runs real TLS OAuth/device/CI flows:
python3 tests/oauth.py
# Also exercise the locally built CLI and a test-only keyring provider:
ENCORE_TEST_CLI=../encore/target/dev/encore python3 tests/oauth.py
```

Tests use disposable data. Set `ENCORE_INDEX_PORT=18137` when the preview
occupies the default port; concurrent suites must use distinct free ports.
`ENCORE_TEST_COMPILER` selects the compiler for the disposable OAuth test build.
Coverage includes
real CLI publish/add/run, retries/conflicts, auth/ownership, archive safety,
quotas, concurrency, rollback after file commit, forced process death, import
and backup/restore including corruption rejection. Blaze's
`tests/pending-server.py` checks slow-handler fairness, pipelining, disconnects
and CPU consumption on port 8138.

Browser checks cover both themes at 375/768/1440 px, search, keyboard focus,
contrast, long content, checksum disclosure and absence of scripts/external
requests. Screenshots are saved in `target/ui-screenshots`. Install Playwright's
Chromium first, or supply an existing Chromium via `--browser /path/to/chrome`.
