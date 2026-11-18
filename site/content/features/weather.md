---
title: "Weather"
weight: 80
group: "World"
summary: "One weather map for wind, storms, rain, wet surfaces and puddles, driving water, cloth and hair."
---

- **Weather map** (`WeatherMap3D`): one weather state round the camera (wind, cloud cover, rain, wetness, puddles), read by every shader through global uniforms, raster and path traced.

  ![Weather map fields round a storm: cloud cover, rain, wind at 10 m, wetness](media/weather-map.jpg)

  *Weather map: a storm's cloud, rain core, wind and the wet ground it leaves.*

- **Wind**: slows near the ground (`roughness_length`); gusts follow the measured spectrum and travel with the wind.
- **Wind near the ground** (`add_canopy`, `add_shelter_box`, `gust_patch_size`): shelter inside plant canopies and behind buildings; gust patches sweep across fields.
- **Season state** (`WeatherMap3D.get_season()`): day length, budburst and leaf-colour sums, snow line, soil frost and moisture from the date and climate, for every system.
- **Storms** (`add_storm`): drift with the wind, with a rain core and a downdraft.
- **Lightning and thunder** (`WeatherLightning3D`): strikes from storm cells at a rate set by cloud-top height, hitting tall things first and starting fires in dry fuel (`PhysXFireSpread3D.lightning_ignition`), lit by their own channel under path tracing, with sharp shadows in raster too (`raster_shadow_distance`) and thunder made from the channel.

  ![A lightning strike among blocks, raster and path traced](media/weather-lightning.jpg)

  *Lightning: the channel lights the ground and casts sharp shadows from the blocks, raster and path traced.*

- **Precipitation haze** (`WeatherMap3D.set_precipitation_extinction()`): snow and rain haze the air in raster fog and the path traced medium; blowing snow is a lit layer near the ground.

  ![A shelter in calm weather, then in a blizzard, raster and path traced](media/weather-blizzard.jpg)

  *Precipitation haze: in a blizzard the blowing snow hides the distance.*

- **Haze and cloud shadow in fog** (`aerosol_haze`, `haze_visibility`): volumetric fog takes the cloud shadow, and the weather adds haze from visibility and humidity.
- **Shafts across the sky** (`atmo_cloud_shadow_steps`): the air under the clouds reads a baked cloud shadow map, so light shafts reach across the whole sky, and the weather's haze carries on past the volumetric fog.
- **Path traced haze** (`WeatherMap3D.traced_haze`): under path tracing the haze is a traced medium lit through shadows and the clouds' direct beam, so shafts stay sharp at any distance and show in reflections.

- **Wet surfaces and puddles** (`surface_storage`, `puddle_drain_rate`, `temperature`, `humidity`): fill with rain and dry by evaporation, faster in sun than in shade or at night (`sun_drying`); materials with Wetness on darken and gloss up.

  ![A beach dry and after rain: darker sand, puddles and a sheen](media/weather-wet.jpg)

  *Wet surfaces: the sand darkens, puddles form and catch the sky.*

- **Dew and frost** (`dew_and_frost`): clear calm nights bring dew, and hoarfrost below freezing on ground, grass, plants and rocks.
- **Contact wetness** (`contact_wetness_enabled`): ground that running water crossed stays dark after it drains, then dries with the weather.
- **Rain on rock** (rock material `rain_rivulets`): rivulets run down rock faces while it rains, and the rock dries in patches after.
- **Air temperature** (`air_follows_climate`, `air_daily_cycle`): follows the season and the time of day.
- **Followers**: water waves grow with the weather's wind (`follow_weather`); cloth and hair move in it (`weather_wind`); tyres lose grip on wet, flooded and icy ground and aquaplane at speed.
