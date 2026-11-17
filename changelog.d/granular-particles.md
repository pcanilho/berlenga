### Fixed

- PhysXGranular3D (and the sand grains a tyre throws) draws as particles again: the fluid's Isosurface default had turned the grains into a clear-water surface, so thrown sand was invisible.

### Changed

- PhysXGroundGrains3D: each thrown speck keeps at least about 1.3 pixels across, so the spray arc reads from several metres away.
