### Added
- `TerrainDeformer3D`: non-destructive height brushes over a heightfield ground (Unreal-style edit layers). Keeps and saves the base heights under every brush, composites the stack in order (lower, raise, flatten, add, blended by each brush's mask) and writes only changed vertices, so moving or removing a brush restores the ground. Grounds: a Terrain3D node, called by name and never bundled (edited regions re-uploaded and saved, collision rebuilt), or a `HeightMapShape3D` with an optional mesh rebuilt from the same heights. `base_height_at()` gives brushes such as `PhysXRiver3D` the uncarved ground.
- `TerrainBrush3D`: circle or rectangle brush, flat or bowl, with a soft edge, for craters, plots and road beds.

### Fixed
- `test_render_raytracing_capture.cpp` compiles with GCC (a doctest `CHECK` on a bitwise and).
