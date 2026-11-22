### Added
- Landscape3D: a heightfield ground drawn as static chunk meshes with an LOD chain picked by projected error (`lod_error`, pixels), neighbours one level apart with stitched edges (no cracks, no skirts). A level change swaps the chunk's mesh, so under path tracing each mesh keeps one BLAS and a moving camera costs no rebuilds or refits. Reads a TerrainGenerator3D (`source`) or a height image.

### Fixed
- TerrainGenerator3D: a lake marker that made no lake failed silently; it now warns with the reason.
