### Added
- PhysXHair3D distance LOD (`lod_enabled`, `lod_start_distance`, `lod_end_distance`, `lod_strand_ratio`): far away the rasterizer draws a random share of the strands, widened to keep the hair's opacity, as TressFX; simulation and path tracing keep every strand

### Fixed
- Raster strand hair measured its pixel size with the wrong sign and drew every strand as a 2 cm ribbon: strands are now sharp and much cheaper to draw
