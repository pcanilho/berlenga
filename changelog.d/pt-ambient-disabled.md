### Fixed
- Path tracing honours `Environment.ambient_light_source = AMBIENT_SOURCE_DISABLED`: diffuse bounces that leave the scene no longer gather the sky (and a procedural sky's sun glow), so a scene lit only by its sun matches the rasterizer (a white Lambert plane under a 30 degree sun: 0.498 traced, 0.497 raster, 0.500 analytic; before, the sky added 7 to 17%).
