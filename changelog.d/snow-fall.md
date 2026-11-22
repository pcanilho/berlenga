### Added

- `SnowFall3D`: falling and blowing snow around the camera from the `SnowCover3D` and the weather, in raster and under path tracing. Flake sizes and fall speeds follow measurements of snowfall, carried by the weather map's wind with its gusts and shelter; flakes stop under roofs; each is drawn as a streak of its motion during `streak_time`. Above the threshold wind of dry snow, snow blows off the pack in a saltation layer and a drifting layer above it. `get_extinction()` and `get_visibility()` give the fog of the snowfall beyond the particles.
- `SnowCover3D.get_air_temperature_at()` and `get_snowfall_rate_at()`.
