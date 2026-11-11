### Changed
- The sea, lake and river flow bakes solve their stream function with multigrid-preconditioned conjugate gradients instead of SOR: about 6x faster at 160 x 160 cells, 10x at 320 x 320, the same flow. `physics/physx_3d/water/multigrid_flow` (on) is the opt-out.
