### Added

- `RainFall3D`: falling rain round the camera from the weather map, streaks slanting in the wind, splashes, rain haze, and drips from leaves and eaves that keep falling after the rain. `max_drops`, `splashes_enabled`, `drips_enabled`, `haze_enabled`, `shelter_enabled`.
- Raindrop rings on the sea, lakes, pools, rivers, live water and the ground's puddles, raster and path traced; heavy rain dulls reflections (water shader uniform `rain_rings`).
- `WeatherMap3D` rain on leaves: the canopy holds rain and drips after it stops (`canopy_water_enabled`, `canopy_storage`); rain and snow split by the air's temperature at each height (`get_rain_share_at`, `get_rainfall_at`, `get_snowfall_at`).
- `BaseMaterial3D` `wetness_canopy`: leaves stay wet while the canopy holds water, with beads on their upper faces; on for trees and falling leaves, and ground plants.
- `WeatherRunoff3D`: rain runs off the land over kilometres; ground with runoff over it stays wet.
- `PhysXRiver3D` `rain_response` (on): rivers rise and run faster with the runoff; lakes with `level_from_flows` rise with it.
