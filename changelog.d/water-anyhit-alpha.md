### Added

- `IN_ANY_HIT` in spatial shaders: true in the path tracer's any-hit pass, so a material can keep that pass to its coverage and the light it lets through.

### Changed

- Path-traced sea, rivers and sea-cave water: their any-hit pass reads only the shoreline cut-out and the light it lets through to shadows, not the whole surface, so every ray crossing the water costs far less.
