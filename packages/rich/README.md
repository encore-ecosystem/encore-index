# rich

Console rendering helpers inspired by Rich.

Current modules:

- `Console`
- `Spinner` and status helpers
- `ProgressBar`
- `ProgressTask` / `ProgressView`
- `Prompt`
- `SelectPrompt`
- prompt validators (`non-empty`, `identifier`, `usize`)
- prompt retry helpers
- typed prompt validation results
- named live/progress updates
- stable id updates for live/progress
- `pretty` / `PrettyPrinter`
- `markup`
- `inspect`
- `json` rendering
- `Layout`
- `LiveFrame` / `LiveView`
- `Traceback`
- `Theme`
- themed panel/table/progress helpers
- mutable live/progress updates
- `Text` styling helpers
- `Panel`
- `Table`
- `TreeNode`
- `CompilerReport`
- `rich.logging` helpers over the `log` refrain

Logical structure:

- `console`, `style`, `text`, `markup`, `theme`: high-level rendering and styling
- `panel`, `table`, `tree`, `layout`: static structured output
- `report`: higher-level compiler report composition
- `progress`, `live`, `status`, `spinner`: dynamic workflow views
- `prompt`: interactive input, validation, retry, typed results
- `pretty`, `inspect`, `json_render`, `traceback`: diagnostics and developer-facing output
- `logging`: bridge to `log` with console/file/writer sinks

Overview examples in [`examples/`](/home/meshushkevich/Projects/2E-encore/encore/index/rich/examples):

- `overview_console_rendering.enq`: console, status, table, tree, panel, layout, JSON, progress
- `overview_interactive_workflow.enq`: prompt validation, typed results, live updates, traceback
- `overview_logging.enq`: console/file/custom-writer logging through `rich.logging`
- `overview_theme_diagnostics.enq`: themed output, markup, inspect, pretty, diagnostics traceback
- `overview_dashboard_helpers.enq`: higher-level panel/table/layout composition for compiler dashboards
- `overview_compiler_diagnostics.enq`: compiler state snapshots and structured traceback notes/hints
- `overview_stage_orchestration.enq`: synchronized stage updates across progress and live views
- `overview_compiler_report.enq`: end-to-end compiler report with summary, stages, diagnostics and notes

## Quick start

```encore
import rich::{Console, Panel, Spinner, status}

fn main() -> u32 {
    let console = Console::new()
    console.info("bootstrap started")
    console.println(status(Spinner::dots(), 1_usize, "loading parser"))
    console.panel(Panel::titled("Build", "frontend\nbackend"))
    ret 0_u32
}
```

## Table

```encore
import core::vec::Vec
import rich::{Console, Table}

fn main() -> u32 {
    let console = Console::new()
    let mut table = Table::with_headers(Vec[str]::new())
    table.set_title("Modules")
    table.add_row(Vec[str]::singleton("frontend"))
    table.add_row(Vec[str]::singleton("backend"))
    console.table(table)
    ret 0_u32
}
```

## Progress and pretty output

```encore
import rich::{Console, ProgressBar, pretty_kv}

fn main() -> u32 {
    let console = Console::new()
    console.println(pretty_kv("module", "frontend"))
    console.progress(ProgressBar::new(20_usize), 7_usize, 10_usize, "translate")
    ret 0_u32
}
```

## Prompt

```encore
import core::vec::Vec
import rich::{Console, Prompt, SelectPrompt}

fn main() -> u32 {
    let console = Console::new()
    let name = console.prompt(Prompt::with_default("Target", "bootstrap"))
    let mut choices = Vec[str]::new()
    choices.push("debug")
    choices.push("release")
    let profile = console.select(SelectPrompt::with_default("Profile", choices, 0_usize))
    console.info("selected: " + name)
    console.info("profile: " + profile)
    ret 0_u32
}
```

## Markup, inspect, JSON

```encore
import core::vec::Vec
import json::{Json, field}
import rich::Console

fn main() -> u32 {
    let console = Console::new()
    console.markup("[info]build[/] [warn]cache off[/]")

    let mut fields = Vec[(str, str)]::new()
    fields.push(("target", "bootstrap"))
    fields.push(("modules", "3"))
    console.inspect_fields("Project", fields)

    let mut obj = Vec[(str, Json)]::new()
    obj.push(field("name", Json::String("encore")))
    obj.push(field("ok", Json::Bool(true)))
    console.json(Json::Object(obj))
    ret 0_u32
}
```

## Layout and live progress

```encore
import rich::{Console, Layout, LiveFrame, LiveView, ProgressView, StageProgress}

fn main() -> u32 {
    let console = Console::new()

    let mut layout = Layout::new("Build Dashboard")
    layout.add_section("left", "frontend")
    layout.add_section("right", "backend")
    console.layout(layout)

    let mut progress = ProgressView::new("Tasks", 12_usize)
    progress.add_task("lexer", 2_usize, 3_usize)
    progress.add_task("parser", 1_usize, 4_usize)
    console.progress_view(progress)

    let mut live = LiveView::new()
    live.push(LiveFrame::new("Tick 1", "compiling"))
    live.push(LiveFrame::with_footer("Tick 2", "linking", "done"))
    console.live_view(live)

    console.stage_snapshot(StageProgress::with_footer("emit", "Emit", "writing object files", 1_usize, 2_usize, "running"), 12_usize)
    ret 0_u32
}
```

## Stage Orchestration

```encore
import core::vec::Vec
import rich::{Console, LiveView, ProgressView, StageProgress, stage_summary}

fn main() -> u32 {
    let console = Console::new()
    let mut stages = Vec[StageProgress]::new()
    stages.push(StageProgress::with_footer("parse", "Parse", "collecting modules", 3_usize, 3_usize, "done"))
    stages.push(StageProgress::with_footer("translate", "Translate", "lowering eHIR", 2_usize, 4_usize, "running"))

    let mut progress = ProgressView::new("Compiler stages", 16_usize)
    let mut live = LiveView::new()

    let mut index = 0_usize
    loop {
        match stages.get(index) {
            Some(stage) => {
                console.sync_stage(progress, live, stage)
                index = index + 1_usize
            }
            None => {
                break
            }
        }
    }

    console.info(stage_summary(stages))
    console.progress_view(progress)
    console.live_view(live)
    ret 0_u32
}
```

## Traceback

```encore
import rich::{Console, Traceback}

fn main() -> u32 {
    let console = Console::new()
    let mut tb = Traceback::compiler_error("unknown symbol")
    tb.add_context_frame("frontend/parser.enq", "parse_expr", 12_usize, 4_usize, "let x = 1", "foo(bar)", "ret x")
    tb.add_frame("main.enq", "main", 2_usize, 1_usize, "parse()")
    tb.add_note("symbol `foo` is not defined in this scope")
    tb.add_hint("check imported modules and local bindings")
    console.traceback(tb)
    ret 0_u32
}
```

## Compiler Diagnostics

```encore
import rich::{Console, Traceback}

fn main() -> u32 {
    let console = Console::new()
    console.inspect_compiler_state("typecheck", "frontend", 42_usize, "resolving generic bounds")

    let mut tb = Traceback::compiler_error("trait method `fmt` is not implemented")
    tb.add_context_frame(
        "frontend/pretty.enq",
        "render_value",
        33_usize,
        15_usize,
        "let value = lower(expr)",
        "ret value.fmt()",
        "ret output",
    )
    tb.add_note("the inferred type does not satisfy the formatting contract")
    tb.add_hint("add an explicit formatter or reduce the generic surface")
    console.traceback(tb)
    ret 0_u32
}
```

## Compiler Report

```encore
import rich::{CompilerReport, Console, StageProgress, Theme, Traceback}

fn main() -> u32 {
    let console = Console::new()
    let theme = Theme::compiler()

    let mut report = CompilerReport::new("Bootstrap Report")
    report.add_summary("target", "bootstrap")
    report.add_summary("backend", "llvm")
    report.add_stage(StageProgress::with_footer("parse", "Parse", "collecting modules", 3_usize, 3_usize, "done"))

    let mut tb = Traceback::compiler_error("unknown symbol")
    tb.add_context_frame(
        "frontend/checker.enq",
        "check_symbol",
        18_usize,
        5_usize,
        "let ty = resolve(expr)",
        "ret env.lookup(name)",
        "ret ty",
    )
    report.add_traceback(tb)
    report.add_note("cache disabled for reproducible diagnostics")

    console.compiler_report(theme, report)
    ret 0_u32
}
```

## Theme and validators

```encore
import rich::{Console, Prompt, Theme, parse_identifier, parse_usize_or}

fn main() -> u32 {
    let console = Console::new()
    let theme = Theme::compiler()
    console.theme_info(theme, "building bootstrap")

    let raw_name = console.prompt(Prompt::with_default("Module", "frontend"))
    let module_name = parse_identifier(raw_name, "frontend")
    let jobs = parse_usize_or("8", 4_usize)
    console.info(module_name + " jobs=" + jobs.fmt())
    ret 0_u32
}
```

## Retry and themed rendering

```encore
import core::vec::Vec
import rich::{Console, ProgressView, Prompt, RetryPolicy, Table, Theme}

fn main() -> u32 {
    let console = Console::new()
    let theme = Theme::compiler()

    let policy = RetryPolicy::three("module name must be an identifier")
    let module_name = console.prompt_identifier_retry_with_policy(Prompt::with_default("Module", "frontend"), "frontend", policy)
    console.theme_info(theme, "module: " + module_name)

    let mut progress = ProgressView::new("Build", 12_usize)
    progress.add_task("parse", 2_usize, 3_usize)
    console.themed_progress_view(theme, progress)

    let mut headers = Vec[str]::new()
    headers.push("module")
    headers.push("state")
    let mut table = Table::with_headers(headers)
    table.set_title("Compiler state")
    let mut row = Vec[str]::new()
    row.push("frontend")
    row.push("active")
    table.add_row(row)
    console.themed_table(theme, table)
    console.themed_panel(theme, "Summary", "frontend active")
    ret 0_u32
}
```

## Typed validation and updates

```encore
import core::result::Result
import rich::{LiveFrame, ProgressView, parse_identifier_result}

fn main() -> u32 {
    let mut progress = ProgressView::new("Build", 12_usize)
    progress.add_task("parse", 1_usize, 3_usize)
    progress.advance_task_by_label("parse", 1_usize)

    let mut frame = LiveFrame::new("Stage", "collect modules")
    frame.set_footer("running")

    match parse_identifier_result("frontend_core") {
        Result[str, str]::Ok(name) => {
        }
        Result[str, str]::Err(message) => {
        }
    }
    ret 0_u32
}
```

## Named updates

```encore
import rich::{LiveFrame, LiveView, ProgressView}

fn main() -> u32 {
    let mut progress = ProgressView::new("Build", 12_usize)
    progress.add_task_with_id("parse-stage", "parse", 1_usize, 3_usize)
    progress.advance_task_by_id("parse-stage", 1_usize)

    let mut live = LiveView::new()
    live.push(LiveFrame::with_id("parse-stage", "parse", "collect modules", ""))
    live.replace_by_id("parse-stage", LiveFrame::with_id("parse-stage", "parse", "translate ast", "running"))
    ret 0_u32
}
```

## Logging

```encore
import core::os::{file_exists, read_file, write_file}
import log::Writable
import rich::{RichLoggerBuilder, compact_formatter, file_logger}

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
    let file = file_logger("bootstrap", "target/bootstrap.log")
    file.warn("file log")

    let mut builder = RichLoggerBuilder::new("bootstrap")
    builder.add_console_with_formatter(compact_formatter())
    builder.add_writer(CaptureFile{"target/bootstrap-writer.log"} as dyn Writable)
    let logger = builder.build()
    logger.info("console + writer log")
    ret 0_u32
}
```

## Dashboard Helpers

```encore
import core::vec::Vec
import rich::{Console, Layout, Panel, Table, Theme}

fn main() -> u32 {
    let console = Console::new()
    let theme = Theme::compiler()

    let mut fields = Vec[(str, str)]::new()
    fields.push(("target", "bootstrap"))
    fields.push(("backend", "llvm"))
    console.panel(Panel::key_values("Summary", fields))

    let mut rows = Vec[(str, str)]::new()
    rows.push(("parse", "done"))
    rows.push(("translate", "running"))
    let table = Table::from_pairs("Pipeline", "stage", "state", rows)
    console.themed_table(theme, table)

    let mut layout = Layout::themed(theme, "Dashboard")
    layout.add_panel_section("summary", Panel::lines("Workers", Vec[str]::singleton("jobs=8")))
    layout.add_columns_section("split", "frontend", "backend", " => ")
    console.layout(layout)
    ret 0_u32
}
```
