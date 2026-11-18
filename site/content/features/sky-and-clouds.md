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

- **Cloud layer** (`cloud_base_height`, `cloud_thickness`, `cloud_density`): shaped from the weather map's cover, cloud type and storms, with fair skies carrying fewer, thinner clouds as density and patchiness follow `cloud_cover`; cumulus sit on flat bases, come in mixed sizes and fray into wisps, and lean with the wind shear as their tops drift downwind and veer with height (`cloud_shear`; `WeatherMap3D.wind_shear_aloft`, `get_wind_aloft()`); drawn by `WeatherSkyMaterial` and seen by path traced rays (`pathtracing_sky_shader_enabled`).

  ![A cloud layer over a meadow under mountains, raster and path traced](media/weather-clouds.jpg)

  *Clouds: the layer seen from the ground over a meadow, raster and path traced.*

- **Clouds that never repeat** (always on): shape noise read at two scales with an irrational ratio.

  ![Cloud cover from above over 24 km, before and after the fix](media/clouds-repeat.jpg)

  *Clouds that never repeat: seen from above over 24 km, the 3 km grid is gone.*

- **Evolving clouds** (`cloud_evolution`; `cloud_lifetime`, `cloud_churn`): clouds form, clear and reshape over time instead of only drifting.
- **Cloud reprojection** (`WeatherSkyMaterial.cloud_reprojection`): clouds draw a sixteenth of their pixels each frame and reproject the rest (Nubis), still full resolution.
- **Cloud shadows** (`sun_light`): the clouds' shadow is baked into the sun's projector, matching in raster and path tracing.

  ![Cloud shadows on a field, seen from above, path traced](media/weather-cloud-shadows.jpg)

  *Cloud shadows: dark patches on the ground under the clouds.*

- **Sun projector**: `light_projector` works on DirectionalLight3D (`projector_size`, `projector_offset`), raster and path traced.
