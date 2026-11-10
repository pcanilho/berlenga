### Added

- `RenderingServer.multimesh_mark_buffer_rd_changed()`: MultiMesh records written by a compute shader now reach the AABB, motion vectors and the path tracer (merged at once, per-instance records after an asynchronous readback).

### Fixed

- Path tracing: a MultiMesh under the merge cap kept its first records after `multimesh_set_buffer()` or `multimesh_instance_set_*()` when motion vectors were off.
