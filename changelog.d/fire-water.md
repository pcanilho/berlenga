### Added

- Water puts fires out: `PhysXParticleFluid3D` water lying on a `PhysXFireSpread3D` grid soaks the grass and litter under it, putting out burning cells and stopping the front at wet ground; water on a `PhysXBurnable3D` damps and puts out its flames and drowns the embers on it. `PhysXFireSpread3D.get_doused_cells()`.
