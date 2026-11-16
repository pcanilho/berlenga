### Changed
- PhysXFlow3D.ambient_scattering: sky light inside the smoke is dimmed by all the gas above each point, read with one fetch from a volumetric shadow map built looking straight down, instead of a 4-step march over 4 m. Tall plumes darken at the base and core as in path tracing.
