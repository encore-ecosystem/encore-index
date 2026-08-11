# argparse

`argparse` is a small, dependency-light command-line parser for Encore
programs. It provides typed option specifications, deterministic diagnostics,
and a single process-argument boundary for command-line tools.

```encore
let spec = CommandSpec::new("build")
    .option(OptionSpec::value("--profile", "", "name", "optimization profile"))
    .option(OptionSpec::flag("--help", "-h", "show help"))

match parse(spec, process_args(2_usize)) {
    ParseOutcome::Ok(args) => { /* use args.value/has/positionals */ }
    ParseOutcome::Err(message) => { /* render one deterministic error */ }
}
```

Use `process_arg_count()` and `process_arg(index)` when a command needs the
raw process vector (for example, to forward arguments after `--`).
