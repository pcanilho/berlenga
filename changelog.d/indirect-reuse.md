### Added

- Path tracing: indirect light reuse (ReSTIR GI), `rendering/pathtracing/indirect_reuse`, off by default. The first bounce of each pixel's diffuse samples is resampled across frames and neighbouring pixels. A room lit through one window goes from 22.8 to 30.0 dB at 4 spp, better than 64 spp without it. Uses 104 bytes per internal pixel.
