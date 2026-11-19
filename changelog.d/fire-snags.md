### Added

- Fire-killed trees stand as snags: a fire kills a tree by how much of its crown it scorched against its bark, and over the game years the dead tree's limbs and roots rot, so storms strip it and it falls at its butt, leaving a stump. `WorldFire.crown_scorch_at()`, `TreeWind3D.get_snag_count()`.
- `PhysXFireSpread3D.get_burn_age_at()`: how long ago each patch burnt, in the weather's game years (`WorldFire.burn_age_at()`).
