### Added
- PhysXWaterSurface3D `wave_spreading` (Donelan-Banner by default, Hasselmann, or the old Cos2) and `swell`, after Horvath 2015: the dominant waves run with the wind and short waves cross, matching Cox and Munk's measured slopes (1.41 against 1.48 along/across at 14 m/s; Cos2 gave 2.72).

- `physics/physx_3d/water/shared_memory_fft` (off until timed on a GPU): the ocean FFT runs each line's whole transform in shared memory, all four transforms of a step in two dispatches per cascade instead of 8 log2(N); the same field.
