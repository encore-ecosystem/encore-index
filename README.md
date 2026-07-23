# Encore package index

This repository is the official sparse package index for Encore.

Package metadata is stored at `<first-two-characters>/<name>.json`. Immutable
source archives are attached to GitHub releases and pinned by SHA-256. The
reviewable source corresponding to the initial package set lives in
`packages/`.

The compiler's default index URL is:

```text
https://raw.githubusercontent.com/encore-ecosystem/encore-index/refs/heads/main
```

Published versions are append-only. Remove neither versions nor metadata;
mark withdrawn versions as `yanked` instead.
