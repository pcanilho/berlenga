### Added
- PhysXWaterSurface3D shore_wave_curl: plunging breakers curl over on beaches whose slope makes them plunge (surf similarity number), and mesh_detail for a finer water mesh (#N)

### Changed
- PhysXWaterSurface3D: the swash runs each breaking shore wave up the sand, as far as the Stockdon run-up for the slope; shore wave crests are sharper and troughs flatter as they break (#N)
- PhysXWaterSurface3D: whitecaps also where the wave cascades add up (Jacobian of the summed waves); the water material reads the cascades once per pixel for normals and foam (about 25% less fragment work), and the GPU profiler labels the water passes (#N)

### Fixed
- PhysXWaterSurface3D: with step_mode per frame the swash now follows the shore waves (#N)
