### Fixed

- A dark, dotted rim around Flow gas in raster: the light pass now uses the opacity of Flow's own ray march (Flow patch 0011).
- Raster Flow gas lacked the sky's blue: sky light is no longer halved or greyed by a whole-sphere average, and the ground under the gas is lit by the sky above it.
