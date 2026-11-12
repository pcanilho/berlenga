### Added
- Terrain sky occlusion: the generator keeps each point's horizons and, from the live sky, the sky it sees; raster ground takes ambient light only from that sky (`TerrainGenerator3D.sky_occlusion`, `sky_light_source`; `TerrainMaterial3D.sky_occlusion_enabled`). Slopes under a ridge no longer glow with a sunset the ridge hides, matching the path tracer.
