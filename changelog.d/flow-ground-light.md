### Added

- `PhysXFlow3D.ground_albedo`: the sunlit ground under Flow gas lights its underside in raster, as path tracing's ground bounces do; snow powder clouds use 0.8.

### Fixed

- Path-traced smoke, dust and powder came out grey and see-through when the scene's bounce count was low: scattering inside gas now has its own budget instead of using up the surface bounces.
- Thin edges of Flow gas in raster were as bright as thick gas; light scattered more than once now needs gas around it, so wisps stay see-through as under path tracing.
