### Added
- Spatial shader built-in `PREVIOUS_FRAME` (true while the vertex function runs for last frame's position) and render mode `alpha_motion_vectors` (a blended surface writes motion vectors in the alpha pass, blended by its alpha)
- PhysXHair3D strands write motion vectors, so TAA, FSR 2 and DLSS follow moving hair instead of smearing it
