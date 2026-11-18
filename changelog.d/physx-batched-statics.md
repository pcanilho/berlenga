### Changed

- PhysX: static bodies entering a space go into the scene together before the next step or query, through a pruning structure for large batches, so streaming in a region of static bodies costs less in its first frame. Adds `docs/bench/physx_openworld`.
