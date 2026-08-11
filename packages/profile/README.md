# profile

Aggregated, named function profiling for Encore. Decorators are statically
expanded by the compiler, so the wrapped function is reached by a direct call.

```encore
import profile::ProfileManager

pub static RENDER_PROFILE = ProfileManager::new("render")

@RENDER_PROFILE.profile("draw_frame")
fn draw_frame() -> () {
    ret ()
}

fn main() -> u32 {
    draw_frame()
    RENDER_PROFILE.report()
    ret 0_u32
}
```
