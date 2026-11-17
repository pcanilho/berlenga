### Added

- PhysXFlow3D `terrain_floor` (on by default): the ground under the gas is a solid, laid as overlapping tiles tilted to the slope, so smoke and dust lie on and run down terrain instead of sinking into it. Heights come from `terrain_path` or a downward ray onto static bodies.
- PhysXFlow3D `smoke_buoyancy` (Flow's buoyancy per smoke): below 0 the smoke is heavier than air, for dust and cold fog.
