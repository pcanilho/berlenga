### Added

- `GrassField3D.gpu_fill` (on by default): grass tiles are filled by a compute shader that writes the blades straight into each tile's MultiMesh, seen by the path tracer as before; about 40% less main-thread time per tile, more at higher densities.

### Changed

- MultiMesh buffers are cleared on the GPU when allocated instead of uploading a zeroed copy.
- GrassField3D's blue-noise rank no longer takes a lock per blade.
