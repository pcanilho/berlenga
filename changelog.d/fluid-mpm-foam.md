### Added

- MPM fluid makes spray, foam and air bubbles (Ihmsen et al. 2012 diffuse particles; bubble drag and foam surface hold from Wretborn et al. 2022). On by default through `foam_enabled`.

### Fixed

- MPM fluid surface no longer shows bright cell outlines: closed air pockets inside the marched surface are smoothed out before meshing.
- MPM fluid streams fall at full speed again: velocity damping is per second instead of per substep, so many substeps no longer hold a falling stream under 1 m/s. Pours now thin and break up as they accelerate.
