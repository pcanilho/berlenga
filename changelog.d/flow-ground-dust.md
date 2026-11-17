### Added

- PhysXGroundDust3D: tyre dust near the camera simulated in Flow. Every sliding or spinning wheel feeds it; the dust trails, rolls in the car's wake, is pushed by the ground wind, flows round the car body, rests on the ruts and shades the ground by its density. Farther dust stays with GroundDust3D.
- GroundSurface3D `get_height`, so a PhysXFlow3D terrain floor can follow the deformed sand.

### Fixed

- PhysXFlow3D: vehicles (bodies whose shapes live on their own PhysX actor) are solids for the gas again, from their CollisionShape3D children; before, they were skipped with an error each step.
