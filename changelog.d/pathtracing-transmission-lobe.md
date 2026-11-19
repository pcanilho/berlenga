### Changed
- Path tracing: transmissive surfaces pick reflection or transmission by the Fresnel at the view angle instead of the head-on reflectance, so water and glass seen at grazing angles show their sky reflection without the bright speckle (about a third of the noise at the same sample count).

### Added
- Shader built-in `RAY_FOOTPRINT` (fragment): the width of the path tracer's ray cone where it meets the surface, in world units; 0 in raster. Rivers use it to filter ripples finer than a pixel into roughness, as raster does.
