### Fixed

- Blast pieces no longer fly apart and through the ground when they break: `PhysXDestructible3D.max_depenetration_velocity` (3 m/s by default) caps how fast PhysX pushes touching pieces apart, so projects need not cap it globally.
