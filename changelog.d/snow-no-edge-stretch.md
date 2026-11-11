### Fixed
- Snow: with no world tier, ground outside the snowpack's square shows no snow (raster and path traced) and `SnowCover3D.get_depth_at()` returns 0 there, instead of stretching the square's edge outward.
