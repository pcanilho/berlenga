### Added
- PhysXHair3D `simplify_tolerance` (default 0.5 of the root width): straight runs of a strand are drawn through fewer of its points (Douglas-Peucker on the groom), fewer triangles for the path tracer to refit with no visible change; every point still simulates
