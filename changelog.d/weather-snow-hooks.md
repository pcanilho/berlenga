### Added

- Weather: falling snow no longer wets surfaces (only the rain share, `WeatherMap3D.rain_fraction()`, reaches the water film), and a snowpack's runoff does instead (`set_surface_water_source()`, fed by `SnowCover3D`). New getters `get_air_temperature()`, `get_air_humidity()`, `get_canopy_height_at()`, `get_canopy_closure_at()`.
