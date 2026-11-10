### Added

- `SnowCover3D`: a snowpack that follows the camera and takes its weather from `WeatherMap3D`: rain or snow by temperature and humidity, colder with altitude, new snow density by temperature, settling, albedo ageing, melt by temperature and sunlight on each slope, meltwater held and refrozen, snow sliding off steep slopes and drifting into the lee of ridges. Queries `get_depth_at()`, `get_swe_at()`, `get_runoff_at()` and more; shader globals `snow_map`, `snow_map_rect`.
- BaseMaterial3D Snow (`snow_enabled`, `snow_amount`, `snow_max_slope`, `snow_from_weather`): snow cover from the snowpack on any material, in raster and under path tracing.
