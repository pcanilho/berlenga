### Added

- Bark beetle kill: `TreeGenerator.beetle_years` grows a conifer from green attack (resin beads, boring dust) through red needles to a grey snag shedding its bark, then a broken top; mesh meta `tree_beetle_years`. Stand `STAND_BEETLE_SPRUCE` with grey snag spots, a red front at their edges and living spruce between, sized by `beetle_outbreak`.
- `TreeGenerator.get_living_meshes()`: a species' meshes without its fire band variants.

### Changed

- Far tree meshes leave out the displaced bark relief (drawn by the near mesh) and the centimetre fruit; far date palm pinnae are two triangles. A wild date palm clump far mesh is about half the triangles.
