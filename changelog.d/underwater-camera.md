### Added
- Camera under water: the path tracer starts each pixel's camera ray in the medium found above its near plane point, so the water fogs, scatters and bends the view up through the surface; `PhysXWaterSurface3D.underwater_enabled` does the same in raster (depth fog with the path traced medium, the image split at the water line, Snell's window and total internal reflection from below) (#185).

### Changed
- Path tracing: camera rays start at the near plane, as raster clips, and a ray that misses everything inside clear water is absorbed (#185).
