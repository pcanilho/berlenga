### Added

- `PhysXFireSpread3D.use_weather_moisture` (on): fuel moisture follows the active `WeatherMap3D`, drying or damping towards the air's equilibrium moisture over `moisture_timelag`, and rain wets the fuel at once, slows the front and puts burning ground out; `get_moisture_at()` reads it.
