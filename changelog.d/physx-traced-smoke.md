### Added
- Path tracer: simulated gas. The PhysX Flow volume nearest the camera is traced as a participating medium (delta tracking, ratio-tracked shadows, Henyey-Greenstein phase, temperature emission). Setting `rendering/pathtracing/simulated_gas` (#PR).
- PhysXCloth3D.release_pins(): lets pinned vertices go without resetting the cloth, so an edge can tear free (#PR).
- PhysXCloth3D.tear(): cuts the cloth along a line so it rips apart (CPU solver) (#PR).
- PhysXFlowEmitter3D: an emitter with no smoke, fuel, burn or heat only pushes air, for wind on the gas; it no longer cools the gas it passes (#PR).
