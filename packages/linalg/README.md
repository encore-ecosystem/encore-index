# linalg

Reusable three-dimensional vectors, matrices and camera transforms for Encore.

`Mat4` uses right-handed coordinates. Perspective matrices target Vulkan clip depth `0..1` and invert projection Y. Matrix values are exported in column-major order for GLSL uniforms.
