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
- **Wind over terrain** (`terrain_wind_enabled`, `drainage_winds`, `upslope_winds`, `valley_winds`, `foehn_winds`): faster over crests, calm in lees, turned along valleys and rising up slopes; on clear calm nights cold air runs down the slopes near the ground, and on sunny calm afternoons the air runs up them and, when moist, gathers fair-weather cumulus over the crests; valleys blow up-valley on sunny calm afternoons and down-valley on clear calm nights, carrying smoke and fog along them; wind across a range comes down the lee warmer and drier once the windward side has rained out, under a gap in the cloud.
- **Wind near the ground** (`add_canopy`, `add_shelter_box`, `gust_patch_size`; RockField3D `wind_obstacles`, PhysXVehicle3D `wind_obstacle`): shelter inside plant canopies and behind buildings, boulders and vehicles; gust patches sweep across fields.
- **Season state** (`WeatherMap3D.get_season()`): day length, budburst and leaf-colour sums, snow line, soil frost and moisture from the date and climate, for every system.
- **Late frost** (`WeatherMap3D.late_frost`): a hard spring frost browns the young leaves of species already out, first in cold hollows, until a second flush replaces them weeks later.
- **Storms** (`add_storm`): drift with the wind, with a rain core and a downdraft.
- **Falling rain** (`RainFall3D`): drops sized and falling like real rain, slanting in the wind, with splashes, haze, and drips from leaves and eaves after it stops.
- **Rain or snow by height** (`WeatherMap3D.get_rain_share_at()`, `warm_nose_temperature`, `warm_nose_base`): rain turns to sleet and then snow as the air cools with height; under a warm layer aloft it falls as freezing rain, or as ice pellets when the cold layer below is deep, and every system reads the same split.
- **Lake-effect snow** (`WeatherMap3D.lake_effect_snow`): cold air crossing open water builds snow bands downwind, reaching some 60 km inland.
- **Lightning and thunder** (`WeatherLightning3D`): strikes from storm cells at a rate set by cloud-top height, hitting tall things first and starting fires in dry fuel (`PhysXFireSpread3D.lightning_ignition`), lit by their own channel under path tracing, with sharp shadows in raster too (`raster_shadow_distance`) and thunder made from the channel.

  ![A lightning strike among blocks, raster and path traced](media/weather-lightning.jpg)

  *Lightning: the channel lights the ground and casts sharp shadows from the blocks, raster and path traced.*

- **Precipitation haze** (`WeatherMap3D.set_precipitation_extinction()`): snow and rain haze the air in raster fog and the path traced medium; blowing snow is a lit layer near the ground.

  ![A shelter in calm weather, then in a blizzard, raster and path traced](media/weather-blizzard.jpg)

  *Precipitation haze: in a blizzard the blowing snow hides the distance.*

- **Haze and cloud shadow in fog** (`aerosol_haze`, `haze_visibility`): volumetric fog takes the cloud shadow, and the weather adds haze from visibility and humidity.
- **Shafts across the sky** (`atmo_cloud_shadow_steps`): the air under the clouds reads a baked cloud shadow map, so light shafts reach across the whole sky, and the weather's haze carries on past the volumetric fog.
- **Path traced haze** (`WeatherMap3D.traced_haze`): under path tracing the haze is a traced medium lit through shadows and the clouds' direct beam, so shafts stay sharp at any distance and show in reflections.
- **Dust storms** (`WeatherMap3D.dust_storms`): strong dry wind over loose ground raises mineral dust that dims the sun orange-brown, settles out and washes out in rain, and a storm's outflow piles it into a haboob: a lobed, billowing wall lit tan by the sun, in raster and path tracing.
- **Smoke haze** (`WeatherMap3D.smoke_haze`, `smoke_washout`; `PhysXFlow3D.rain_washout`; `PhysXFlowEmitter3D.regional_smoke`): smoke from fires, campfires and chimneys drifts on the wind past the Flow box as a haze layer that dims and reddens the sun's disc and glow, turns the sky brown under a plume and cools the day; rain and snow wash smoke and dust out of the air.

- **Wet surfaces and puddles** (`surface_storage`, `puddle_drain_rate`, `temperature`, `humidity`): fill with rain and dry by evaporation, faster in sun than in shade or at night (`sun_drying`); materials with Wetness on darken and gloss up, walls facing the wind catch the driven rain, roofs the falling rain and lee faces none, raster and path traced; leaves hold rain as beads and stay wet after the ground dries (`wetness_canopy`, `canopy_storage`).

  ![A beach dry and after rain: darker sand, puddles and a sheen](media/weather-wet.jpg)

  *Wet surfaces: the sand darkens, puddles form and catch the sky.*

- **Raindrop rings** (`rain_rings` water uniform): rings on sea, lakes, pools, rivers and puddles; heavy rain dulls reflections.
- **Runoff** (`WeatherRunoff3D`, `PhysXRiver3D.rain_response`): rain runs off the land and wets the ground it crosses, and rivers and lakes rise with it.

- **Dew and frost** (`dew_and_frost`, `cold_pools`, `valley_fog`, `smoke_in_pools`): clear calm nights bring dew, and hoarfrost below freezing on ground, grass, plants and rocks; cold air pools in valleys and hollows, so frost forms there first, and saturated pools fill with a flat fog sea that lifts after sunrise or in a wind, wetting the trees standing in it and riming them below freezing; smoke stays low in the pools and drains down the slopes until the sun mixes it up.
- **Fog drip** (`WeatherMap3D.fog_drip`): cloud lying on wooded ground drips from the trees and wets the ground beneath them.
- **Sea spray** (`WeatherMap3D.sea_spray`): breaking waves wet the shore and cliff faces downwind and leave salt that rain washes off; salted grass browns at the tips, and shrubs and trees brown leaf by leaf.
- **Wet tracks** (`WeatherMap3D.wet_tracks`): bodies and vehicles coming out of water, wet mud or puddles leave wet footprints and tyre tracks that fade along the way, drip where they stand, and dry with the weather.
- **Contact wetness** (`contact_wetness_enabled`): ground that running water crossed stays dark after it drains, then dries with the weather.
- **Rain on rock** (rock material `rain_rivulets`): rivulets run down rock faces while it rains, and the rock dries in patches after.
- **Air temperature** (`air_follows_climate`, `air_daily_cycle`): follows the season and the time of day.
- **Followers**: water waves grow with the weather's wind (`follow_weather`) and gusts darken the ripples in moving cat's paws (`cats_paws` shader uniform); cloth, hair and smoke move in it (`weather_wind`); particles drift with it and its gusts (`wind_drag`); tyres lose grip on wet, flooded and icy ground and aquaplane at speed, and slip on lying snow (most on powder and slush, almost as on ice on a hard crust) and on bare lake ice, where chains and lugs win it back.
