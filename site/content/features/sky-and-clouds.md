---
title: "Sky and clouds"
weight: 85
group: "World"
summary: "Sun and moon set by date, time and place; a scattering atmosphere with twilight; a cloud layer from the weather map, reprojected, path traced, with shadows on the ground."
---

- **Sun and moon clock** (`SkyClock`): date, time and place point the sun and moon lights, with moon phases (Meeus); the sun is within 0.0004 degrees of the ephemeris and sunrise within half a minute of the almanac (`docs/bench/sky --test`).
- **Sky atmosphere** (`WeatherSkyMaterial.atmosphere_model`, `SkyClock.drive_light_energy`): multiple scattering (Hillaire 2020) lit by sun and moon, with twilight, the Earth's shadow and clouds hazed to the horizon; `atmo_far_water` hazes far sea, `energy_per_lux` scales the lights.
- **Night sky** (`stars_enabled`, `moon_disc_enabled`, `airglow_enabled`, `milky_way`): the 9,100 naked-eye stars of the Yale Bright Star Catalogue, placed by the sky clock, coloured by temperature, dimmed by the air and hidden behind clouds, with glare on the brightest; the moon disc shows its real phase and earthshine; on moonless nights the faint green airglow of the upper air brightens toward the horizon and lights cloud tops; the Milky Way comes from a galactic panorama turned with the stars.
- **Eye adaptation** (`SkyClock.drive_exposure`, `adaptation`): exposure follows the light on the ground like an adapting eye, so twilight, moonlit and starlit scenes stay readable.
- **Night vision** (`CameraAttributes.night_vision`): in dim light colour fades to the eye's blue-grey night vision (Purkinje shift) while lamps, lit windows and the moon keep theirs, from the scene's real luminance, in raster and path tracing.

  ![The sea horizon at midday under the old sky and the new atmosphere](media/sky-atmosphere.jpg)

  *Sky atmosphere: the dark grey band at the horizon is gone and far clouds fade into haze.*

  ![The island at sunset under the old sky and the new atmosphere](media/sky-sunset.jpg)

  *Sunset: the old sky goes black; the atmosphere lights the clouds orange from below the horizon.*

- **Cloud layer** (`cloud_base_height`, `cloud_thickness`, `cloud_density`): shaped from the weather map's cover, cloud type and storms, with fair skies carrying fewer, thinner clouds as density and patchiness follow `cloud_cover`; cumulus sit on flat bases, come in mixed sizes with uneven gaps at the same cover (`cloud_size_mix`), fray into wisps and fade into the horizon haze, and lean with the wind shear as their tops drift downwind and veer with height (`cloud_shear`; `WeatherMap3D.wind_shear_aloft`, `get_wind_aloft()`); low stratus and stratocumulus follow the day's heating, wind, humidity and rain, with their base at the condensation level (`WeatherMap3D.cloud_regime_from_weather`); storms (`WeatherMap3D.add_storm()`) tower into a leaning cumulonimbus about as wide as it is tall, with a flat anvil spreading downwind, a thin cirrus shield beyond it and mammatus beneath, a tenuous edge shell that glows as a silver rim against the sun (`cloud_storm_shell`, `cloud_storm_shell_density`) and turret relief that reads in shade from how much sky each part sees (`cloud_sky_occlusion_reach`), and hang rain shafts under their base, slanted by the low wind, as dense as the rain is heavy and drawn in front of far terrain (`WeatherMap3D.distant_shafts`), or in cold storms snow shafts that trail long soft veils downwind, and a moving storm drops a shelf cloud along its gust front; high up, shear aloft breaks the cirrus into cirrocumulus grains and a spreading high sheet merges into a cirrostratus veil that carries halos, and its oriented ice crystals throw sun dogs and tangent arcs (`cloud_parhelia_strength`); sunlight through gaps in a broken mid layer reaches the ground and the clouds below, and each layer takes the sun's colour at its own height, so high clouds stay lit after sunset; lenticular clouds have a crisp rim and stack in plates with sky between them, and their thin edges near the sun show iridescence (`cloud_iridescence`); drawn by `WeatherSkyMaterial` and seen by path traced rays (`pathtracing_sky_shader_enabled`).

  ![Fair weather cumulus over a meadow at 14:00, looking up toward the sea, raster](media/sky-clouds.jpg)

  *Fair weather cumulus of mixed sizes over a meadow, raster.*

  ![A smooth lenticular cloud over a mountain ridge, pastel colours along its edge near the sun and thin plates stacked downwind, raster](media/sky-lenticular.jpg)

  *Lenticular cloud: a smooth lens over the ridge, iridescent along its thin edge near the sun, with stacked plates downwind.*

- **Clouds that never repeat** (always on): shape noise read at two scales with an irrational ratio; mid-level cloudlets are warped and gathered into groups of varied size, so they never line up in rows.

  ![Cloud cover from above over 24 km, before and after the fix](media/clouds-repeat.jpg)

  *Clouds that never repeat: seen from above over 24 km, the 3 km grid is gone.*

- **Evolving clouds** (`cloud_evolution`; `cloud_lifetime`, `cloud_churn`): clouds form, clear and reshape over time instead of only drifting.
- **Cloud reprojection** (`WeatherSkyMaterial.cloud_reprojection`): clouds draw a sixteenth of their pixels each frame and reproject the rest (Nubis), still full resolution.
- **Cloud shadows** (`sun_light`; `cloud_light_grid_enabled`): the clouds' shadow is baked into the sun's projector, matching in raster and path tracing, and clouds shade each other across the whole layer, with long shadows at low sun.

  ![Cloud shadows on a field, seen from above, path traced](media/weather-cloud-shadows.jpg)

  *Cloud shadows: dark patches on the ground under the clouds.*

- **Sun projector**: `light_projector` works on DirectionalLight3D (`projector_size`, `projector_offset`), raster and path traced.
