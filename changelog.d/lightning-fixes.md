### Fixed

- Weather: `WeatherLightning3D.get_last_channel()` keeps the newest ground flash's channel after its light fades.

### Added

- Weather: `WeatherMap3D.get_cloud_top_at()`, the top of the cloud the sky draws over a point. Storm flash rates use it: a storm's core reaches the base plus three times `cloud_thickness`.
