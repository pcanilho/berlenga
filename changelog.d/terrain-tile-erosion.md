### Added

- `TerrainGenerator3D` tile erosion (`tile_erosion_enabled`): droplet hydraulic erosion with loose and bedrock layers plus thermal slides on a fine grid, in overlapping tiles cross-faded so there are no seams; deterministic, threaded. Rills and alluvial valley floors; flow and deposit maps (`get_tile_flow()`, `get_tile_deposit()`).
