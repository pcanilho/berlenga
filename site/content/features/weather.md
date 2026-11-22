---
title: "Weather"
weight: 70
summary: "One weather map for wind, storms, rain, wet surfaces and puddles, driving water, cloth and hair."
---

- **Weather map** (`WeatherMap3D`): one weather state round the camera (wind, cloud cover, rain, wetness, puddles), read by every shader through global uniforms, raster and path traced.

  ![Weather map fields round a storm: cloud cover, rain, wind at 10 m, wetness](media/weather-map.jpg)

  *Weather map: a storm's cloud, rain core, wind and the wet ground it leaves.*

- **Wind**: slows near the ground (`roughness_length`); gusts follow the measured spectrum and travel with the wind.
- **Wind near the ground** (`add_canopy`, `add_shelter_box`, `gust_patch_size`): shelter inside plant canopies and behind buildings; gust patches sweep across fields.
- **Storms** (`add_storm`): drift with the wind, with a rain core and a downdraft.
- **Lightning and thunder** (`WeatherLightning3D`): strikes from storm cells at a rate set by cloud-top height, lit by their own channel under path tracing, with sharp shadows in raster too (`raster_shadow_distance`) and thunder made from the channel.

  ![A lightning strike among blocks, raster and path traced](media/weather-lightning.jpg)

  *Lightning: the channel lights the ground and casts sharp shadows from the blocks, raster and path traced.*

- **Precipitation haze** (`WeatherMap3D.set_precipitation_extinction()`): snow and rain haze the air in raster fog and the path traced medium; blowing snow is a lit layer near the ground.

  ![A shelter in calm weather, then in a blizzard, raster and path traced](media/weather-blizzard.jpg)

  *Precipitation haze: in a blizzard the blowing snow hides the distance.*

- **Wet surfaces and puddles** (`surface_storage`, `puddle_drain_rate`, `temperature`, `humidity`): fill with rain and dry by evaporation; materials with Wetness on darken and gloss up.

  ![A beach dry and after rain: darker sand, puddles and a sheen](media/weather-wet.jpg)

  *Wet surfaces: the sand darkens, puddles form and catch the sky.*

- **Followers**: water waves grow with the weather's wind (`follow_weather`); cloth and hair move in it (`weather_wind`).
