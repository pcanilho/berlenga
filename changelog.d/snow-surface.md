### Added

- `SnowSurface3D`: deep snow as real geometry on top of the `SnowCover3D` pack, in raster and under path tracing. Bodies in the `snow_deform` (or `grass_trample`) group press prints and tracks into it: snow compacts under them and is pushed into a rim, loaded bodies sink by Bekker's pressure-sinkage law, and a collision patch keeps them in their own tracks. New snow and wind fill tracks, settling and melt shrink them, and tracks are kept when the camera leaves and returns. `press()`, `press_box()`, `get_surface_height_at()`.
- `SnowCover3D.get_new_snow_at()`, `sample_ground_height()`, `get_update_count()`, `get_drift_factor()`.

### Fixed

- `SnowCover3D` could crash in its parallel update when another node held a copy of its runoff map.
