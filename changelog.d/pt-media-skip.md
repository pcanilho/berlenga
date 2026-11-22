### Fixed

- The path tracer skips caustic photons and the camera medium probe while no material can bound a medium, and dry ground, beside water or without it, no longer loses the radiance cache to the caustics map (`rendering/pathtracing/caustics`).
