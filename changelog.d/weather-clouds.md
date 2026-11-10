### Added
- DirectionalLight3D projector: `light_projector` now works on directional lights, projected along the light over `projector_size` and `projector_offset` metres, in raster (Forward+, Mobile) and in path traced sun light alike (#207).
- Weather clouds: `WeatherMap3D` cloud layer (`cloud_base_height`, `cloud_thickness`, `cloud_density`, `cloud_shape_scale`) shaped by Nubis-style 3D noise from the map's cover, type and storms, with cloud shadows baked onto `sun_light` in the background, and `get_cloud_density_at()` and `get_sun_visibility_at()` (#207).
- `WeatherSkyMaterial`: the physical sky with raymarched clouds (energy-conserving steps, two-lobe phase, Wrenninge octaves, two-stream diffuse light) drawn at half resolution, seen by path traced camera and bounce rays (#207).
