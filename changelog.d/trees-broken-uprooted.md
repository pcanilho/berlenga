### Added

- Trees: snapped ends get a splintered fracture face and torn bark; `TreeGenerator.limb_breakage` breaks limbs on living trees in storms, some left hanging.
- Trees: `TreeGenerator.uprooted` lays a fallen tree with its root plate (soil, stones, torn roots) and gives the mesh meta `tree_root_pit`; stands lay their freshest log uprooted.
- Trees: pollards and coppice: `TreeGenerator.cut_height`, `cut_cycle` and `cut_since` grow a tree cut back over the years, with a swollen head or a stool of poles.
- Trees: old ivy carries the unlobed adult leaves high on its stems.

### Changed

- Trees: mesh meta `tree_deadwood_axis` carries the lying stem's measured reach along its axis (12 samples after the widest reach and the count).
