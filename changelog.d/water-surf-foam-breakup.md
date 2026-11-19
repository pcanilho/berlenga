### Fixed
- Surf foam no longer shows the shore wave grid as rows of short straight bars: PhysXWaterSurface3D reads its shore wave fields smoothly (cubic) and tears the roller's white water with wandering streaks; foam noise uses turned lattices, its holes cluster and its grain is patchy.
- Path-traced open sea no longer turns black toward the horizon: wave detail is filtered by the pixel's footprint under the path tracer too.
