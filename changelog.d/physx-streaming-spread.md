### Changed

- PhysX: a streamed region's static bodies enter the scene over a few steps (`physics/physx_3d/streaming/statics_per_step`, 1024), those near moving bodies and those a query reaches first, so nothing falls through or is missed; static removals go out in one batch. The first-step hitch of a region drops by up to a third with up to 10,000 statics loaded.
