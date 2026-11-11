### Added
- TerrainGenerator3D: glacial stage (`glacial_enabled`, `glacial_ela`, `glacial_share`, `glacial_final`, `glacial_erosion`, `glacial_sliding`). Shallow-ice glaciers alternate with the rivers and wear troughs and basins; the last glaciation's basins become lakes by themselves (`get_glacier_depth()`, `get_last_glacial_*`). Walls the ice leaves past `strata_cliff_angle` shed rockfall to their foot.
- Sills with a `level` are a hard band at that height (a riegel): rock above it wears down normally. Range paths with `basin` metadata scale the uplift down (a high bowl).
- `cliff_ledge_spacing`, `cliff_ledge_strength`: rock beds of uneven thickness stepped into faces steeper than 40 degrees.
- `get_waterfalls()` fall record for the water code: `id`, `direction`, `type` (plunge, horsetail, cascade, tiered), `lip_points` with `lip_share` (Manning share of the flow), `lip_width`, `caprock`, `overhang`, three `face_profiles`, `tiers`, `ledges`, `power`, `breakup_length`, drag-limited `impact_speed` and `impact_speed_free`, `discharge_months`, and the plunge pool: `pool_center`, `pool_radius`, `pool_depth`, `pool_level`, `lands_in` (pool, lake, sea, ground).
- `plunge_pools` (on): a scour hole below each fall of 4 m or more (Mason and Arumugam depth), with its tail bar downstream so it holds water.
- `terrain_spray_potential` global map (with `terrain_spray_rect`, `spray_cell` 4 m, `get_spray_potential()`, `get_spray_image()`): the static share of fall spray each place can get, from each fall's power and the distance to its water.
- `gorge_age`, `gorge_rate`: lips over hard rock wear back upstream (knickpoint retreat, rate times sqrt(Q), a twentieth in massive rock) and leave a slot gorge below the fall; never into a lake. `get_waterfalls()` reports the fall at the gorge's head.
- `get_boulders(rect, density)`: blocks shed by cliffs, resting where their travel angle drops below the shadow angle, larger blocks further out; (x, y, z, radius) for placement.
### Fixed
- Detail no longer fades on faces steeper than loose fill can stand, however much sediment the cells around them hold.
