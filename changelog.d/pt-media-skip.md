### Fixed

- The path tracer skips caustic photons and the camera medium probe while no material can bound a medium, and dry ground no longer loses the radiance cache to an empty caustics map (`rendering/pathtracing/caustics`).
