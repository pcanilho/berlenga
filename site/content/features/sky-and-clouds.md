---
title: "Sky and clouds"
weight: 80
summary: "Sun and moon set by date, time and place; a scattering atmosphere with twilight; a cloud layer from the weather map, reprojected, path traced, with shadows on the ground."
---

- **Sun and moon clock** (`SkyClock`): date, time and place point the sun and moon lights, with moon phases (Meeus); the sun is within 0.0004 degrees of the ephemeris and sunrise within half a minute of the almanac (`docs/bench/sky --test`).
- **Sky atmosphere** (`WeatherSkyMaterial.atmosphere_model`, `SkyClock.drive_light_energy`): multiple scattering (Hillaire 2020) lit by sun and moon, with twilight, the Earth's shadow and clouds hazed to the horizon; `atmo_far_water` hazes far sea, `energy_per_lux` scales the lights.

  ![The same island under the old sky and the new atmosphere](media/sky-atmosphere.jpg)

  *Sky atmosphere: the dark grey band at the horizon is gone and far clouds fade into haze.*

  ![The island at sunset under the old sky and the new atmosphere](media/sky-sunset.jpg)

  *Sunset: the old sky goes black; the atmosphere lights the clouds orange from below the horizon.*

- **Cloud layer** (`cloud_base_height`, `cloud_thickness`, `cloud_density`): shaped from the weather map's cover, cloud type and storms; drawn by `WeatherSkyMaterial` and seen by path traced rays (`pathtracing_sky_shader_enabled`).

  ![A cloud layer over a field of blocks, raster and path traced](media/weather-clouds.jpg)

  *Clouds: the layer seen from the ground, with its shadows on the field, raster and path traced.*

- **Clouds that never repeat** (always on): shape noise read at two scales with an irrational ratio.

  ![Cloud cover from above over 24 km, before and after the fix](media/clouds-repeat.jpg)

  *Clouds that never repeat: seen from above over 24 km, the 3 km grid is gone.*

- **Cloud reprojection** (`WeatherSkyMaterial.cloud_reprojection`, on by default): clouds draw a sixteenth of their pixels each frame and reproject the rest (Nubis), still full resolution.
- **Cloud shadows** (`sun_light`): the clouds' shadow is baked into the sun's projector, matching in raster and path tracing.

  ![Cloud shadows moving across a field, seen from above, path traced](media/weather-cloud-shadows.jpg)

  *Cloud shadows: dark patches on the ground under the clouds.*

- **Sun projector**: `light_projector` works on DirectionalLight3D (`projector_size`, `projector_offset`), raster and path traced.
