### Changed
- Oasis pools hold clear spring water (near pure water's absorption, little scattering), in raster and path tracing alike.
- An oasis spring whose ground is not hollowed below its level gets no pool or boils, with a warning; `TerrainGenerator3D.carve_oases()` carves it.
- Sand boils vary in size from vent to vent, with darker throats.

### Fixed
- Path tracing: bands along the depth contours under flat water, where the outer caustics cascades took their photons from every other pixel row and column.
