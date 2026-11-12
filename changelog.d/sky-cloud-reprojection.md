### Fixed

- Reprojected sky clouds no longer show a checker or a 4x4 lattice: the march start is interleaved gradient noise shifted every frame, each pixel keeps a running mean clamped to this frame's samples, and the history follows exposure changes.
