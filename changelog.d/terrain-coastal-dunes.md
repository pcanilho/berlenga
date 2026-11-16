### Added

- Terrain generator: coastal dunes. The beaches feed wind-blown sand inland on a fine grid; marram builds foredunes along the plant line, and blowouts and parabolic dunes open where the cover fails (`dunes_enabled`, off by default; `get_dune_height()`, `get_dune_stability()`).
- Terrain material: scree shows a gravel and fines matrix between its stones up close (`rock_detail_enabled`).

### Changed

- Terrain features are on by default: `dunes_enabled`, `glacial_enabled`, `glacial_converged`, `tile_erosion_enabled`, `strata_enabled`, `guide_preview`, `ice_worn_detail`, `deposition` 0.5, `moraine_age` 2000, `scree_age` 10000, `meander_age` 3000, `outwash_age` 10000 (TerrainGenerator3D); `cliff_relief` 0.6 m (Landscape3D); `rock_detail_enabled`, `cliff_self_shadow`, `far_blend_enabled`, `far_start_from_vegetation` (TerrainMaterial3D). Turn them off where a project needs a faster generate.
- Terrain material: sand the generator laid (beaches, dunes) holds on slopes up to the cliff slope instead of turning to soil past 40 degrees.
