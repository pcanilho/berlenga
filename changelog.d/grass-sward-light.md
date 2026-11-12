### Changed

- GrassField3D: near density defaults to 120 blades/m2 (was 60), affordable with the GPU fill.
- GrassField3D: the darkening towards the root moved from the blades' albedo to AO, from the leaf area above (Monsi-Saeki, Goudriaan); the path tracer no longer darkens the roots twice, and raster ambient follows the sward's density.

### Fixed

- GrassField3D: in shadow passes blades widened towards the light's camera, so the sward cast more shadow than it has; they now widen towards the main camera.
- GrassField3D: the GPU fill's compute shader is freed at exit.
