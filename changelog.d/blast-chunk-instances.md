### Added

- `PhysXDestructible3D.initial_linear_velocity` and `initial_angular_velocity`: a dynamic destructible can start moving, so swapping a falling object for one keeps its motion.
- `PhysXBlastAsset.prepare_render()`: converts the chunks' render meshes ahead of time, from any thread.

### Changed

- Destructible pieces are drawn as instances of per-chunk meshes built once per asset and shared, instead of a merged mesh rebuilt on every spawn and split: no more frame stall when a large asset such as a tree spawns or breaks.

### Fixed

- `apply_radial_damage` on a scaled destructible: the point and radii are now in the asset's units.
