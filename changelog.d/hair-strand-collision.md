### Added
- PhysXHair3D strand-strand collision (`strand_collision`, `strand_collision_radius`, `strand_collision_friction`): guide strands no longer pass through each other; a spatial hash finds close guide points once a frame and pushes them apart with friction, the followers come along
