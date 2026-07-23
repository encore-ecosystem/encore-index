# log

Structured logging for Encore with reusable sinks.

Current surface:

- `Logger` with `trace`, `debug`, `info`, `warn`, `error`, `fatal`
- `ConsoleSink` for stdout / stderr
- `FileSink` for file append-style logging
- `WriterSink` for any object implementing `Writable`
- `Formatter` for plain or colored output

## Quick start

```encore
import log::Logger

fn main() -> u32 {
    let mut logger = Logger::console("bootstrap")
    logger.info("build started")
    logger.warn("cache disabled")
    logger.error("missing module")
    ret 0_u32
}
```

## File logging

```encore
import log::Logger

fn main() -> u32 {
    let mut logger = Logger::new("compiler")
    logger.add_file_sink("target/compiler.log")
    logger.info("translation started")
    ret 0_u32
}
```

## Custom writable object

```encore
import core::os::file_exists
import core::os::read_file
import core::os::write_file
import log::{Logger, Writable}

struct CaptureFile {
    path: str
}

impl Writable for CaptureFile {
    fn write(self: CaptureFile, value: str) -> i32 {
        let mut current = ""
        if file_exists(self.path) {
            current = read_file(self.path)
        }
        ret write_file(self.path, current + value)
    }
}

fn main() -> u32 {
    let mut logger = Logger::new("custom")
    logger.add_writer_sink(CaptureFile{"target/custom.log"} as dyn Writable)
    logger.info("hello")
    ret 0_u32
}
```
