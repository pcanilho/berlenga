### Changed
- `GroundDust3D` and `CaveNetwork3D` follow the current camera when `follow_target` is empty, as the weather, the ground and the other world systems do. A cave network whose meshes were built with `build_meshes()` keeps them and does not stream.
