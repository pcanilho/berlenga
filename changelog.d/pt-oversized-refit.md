### Fixed
- Path tracing: a dynamic mesh larger than `rendering/pathtracing/dynamic_blas_update_triangle_budget` (such as a simulated hair groom) was never refit after its first build, so it stood still while the raster moved; the first refit of a frame now ignores the budget, as the first build already did
- Path tracing: a material writing `BENT_NORMAL_MAP` failed to compile its hit shader and the object vanished
