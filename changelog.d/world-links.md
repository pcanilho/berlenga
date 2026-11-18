### Added

- The air follows the season and the time of day: coldest near dawn, warmest in the afternoon, humid at night (`air_follows_climate`, `air_daily_cycle`, `daily_temperature_range`), and cools with height.
- The sun dries wet ground faster than shade or night (`sun_drying`).
- Clear calm nights bring dew, and hoarfrost on terrain, grass, plants and rocks below freezing; the morning sun melts it (`dew_and_frost`).
- Radiation mist comes sooner and thicker over wet ground, and steam fog rises off lakes and the sea when the air is much colder than the water (`WeatherMap3D.get_water_temperature`).
- Lightning strikes tall things first and can start fires in dry fuel (`lightning_ignition`).
- Tyres lose grip on wet, flooded and icy ground, and aquaplane at speed.
- Every moving body bends grass, plants and branches, presses ground and snow and ripples water (`physics/3d/world_contact/auto_join`).
- World contracts: weather, water and body contacts each behind one interface (`WorldContracts`), with script stand-ins for tests.

### Fixed

- WeatherMap3D.advance() now sees canopies and walls added since the last frame.
