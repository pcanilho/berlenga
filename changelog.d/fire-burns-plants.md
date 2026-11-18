### Added

- `PhysXFireSpread3D` burns the plants over it: grass burns down to charred stubble with glowing tips in the burning band, small plants char under the flame and scorch brown above it, tree bark chars to the flame length and leaves brown up to the crown scorch height (Van Wagner 1973), in raster and path tracing, through a new `fire_state_map` global.
- `PhysXFireSpread3D.fuel_patchiness` (on) makes fuel and moisture patchy, so a burn scar has ragged edges and unburnt islands; `fuel_map` takes moisture in its green channel, and `vegetation_field` makes worn paths bare and leaf litter burn.
