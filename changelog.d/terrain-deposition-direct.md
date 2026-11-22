### Changed
- TerrainGenerator3D: `deposition` solves each step exactly in two sweeps along the receiver tree before its Gauss-Seidel passes, so a step needs one or two passes instead of about seven (the deposition part of a generate 0.73 to 0.40 s). `get_last_deposition_passes()` reports them.
