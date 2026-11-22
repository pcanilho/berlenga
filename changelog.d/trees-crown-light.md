### Fixed

- Tree leaves take sky light again under path tracing: crowns in shade no longer go near-black, and raster and path-traced crowns now match in brightness.

### Changed

- In raster, each tree crown shades its own inner and lower leaves from the sky (setting crown_occlusion).
