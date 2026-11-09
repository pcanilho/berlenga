### Fixed
- Path tracing: shaders that move vertices in vertex() get their varyings, UV and COLOR from the rest pose at a hit, as in raster, instead of from the already moved hit point (#169, #182).

### Removed
- PhysXWaterSurface3D: the path-traced shader no longer inverts its own wave displacement; the engine now gives it the rest pose (#182).
