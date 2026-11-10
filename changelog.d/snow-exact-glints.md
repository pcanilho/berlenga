### Added
- Spatial shaders: `SPECULAR_NORMAL`, a normal for the base specular lobe alone under the path tracer (the diffuse keeps `NORMAL`); raster ignores it.
- Snow: exact path-traced sparkle, every 2 mm ice grain of dry snow a facet hit per ray, with the global shader parameter `snow_exact_glints` (off by default); `snow_glint_apply_specular()` in `get_snow_shader_code()` for custom shaders.
