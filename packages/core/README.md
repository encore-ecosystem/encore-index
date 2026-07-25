# core

`core` is Encore's low-level standard package. The compiler injects it into
normal builds, so user packages can import `core::...` APIs without adding
`core` to `encore.toml`.

Use `core` for target-independent language primitives. Platform IO, filesystem,
networking, processes, threads, clocks, and TLS are exposed by `std`; their
internal implementation comes from the target kit's `platform` refrain.

## Modules

| Module | Main API |
| --- | --- |
| `core::option` | `Option[T]`, `Some(T)`, `None`, `is_some`, `is_none`, `unwrap_or`, `and`, `or`, `xor`, `flatten` |
| `core::result` | `Result[T, E]`, `Ok(T)`, `Err(E)`, `is_ok`, `is_err`, `unwrap_or`, `unwrap_err_or` |
| `core::vec` | `Vec[T]`, `VecIter[T]`, `new`, `with_capacity`, `singleton`, `len`, `capacity`, `is_empty`, `reserve`, `push`, `set`, `get`, `first`, `last`, `clear`, `pop`, `extend`, `iter` |
| `core::iter` | `Iterator[T]`, `IntoIterator[T, Iter]`, `Range`, `Enumerate`, `range`, `range_inclusive`, `enumerate` |
| `core::fmt` | `Debug` for primitive values, `Option`, `Result` and `Vec` |
| `core::ops` | Operator traits for arithmetic, comparison, bitwise operators and `ContextManager` |
| `core::cast` | `Numeric`, `Cast[T]` for numeric casts |
| `core::panic` | `panic(message)` |
| `core::testing` | `assert`, `fail` for executable tests |

## Common Patterns

```enq
import core::option::Option
import core::testing::*
import core::vec::Vec

fn main() -> u32 {
    let mut values = Vec[u32]::new()
    values = values.push(3_u32)
    values = values.push(5_u32)

    match values.get(1_usize) {
        Option[u32]::Some(value) => {
            if !assert(value == 5_u32) {
                ret 1_u32
            }
        }
        Option[u32]::None => {
            ret 1_u32
        }
    }

    ret 0_u32
}
```

`Vec` methods return the updated vector. Assign the result when the vector may
grow or when you want code that is independent of current compiler update
sugar.

```enq
import core::iter::enumerate
import core::vec::Vec

fn weighted(values: Vec[u32]) -> u32 {
    let mut total = 0_u32
    for pair in enumerate(values.iter()) {
        total = total + (pair.0 as u32) * pair.1
    }
    ret total
}
```

## Native Runtime

`index/core/build.enq` publishes native link metadata for `index/core/runtime.c`. Backends
consume that metadata instead of owning runtime C code directly. Package authors
can use the same build-script pattern for native objects:

1. Put the build script path in `[project].build` in `encore.toml`.
2. Have the script write native metadata JSON to the path passed as argv `1`.
3. Return a non-zero exit code if metadata generation fails.

The current beta validates this flow on Linux first.

## TLS

`core::tls` provides a verified blocking client stream. `TlsClientConfig::system()`
uses the platform trust store and a 30 second I/O timeout. Applications can use
`with_ca_file(path)` for a private CA and `with_timeout_ms(value)` to bound each
connect, handshake, read and write wait. `TlsStream::connect` always verifies the
certificate chain and requested hostname and enables TLS 1.2 or newer.

The native backend is OpenSSL on Linux, Security.framework on macOS and Schannel
on Windows. The Encore API does not expose backend-specific handles.
