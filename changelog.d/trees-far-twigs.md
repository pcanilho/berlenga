### Changed

- Trees and shrubs: far meshes leave out leaf twigs under a millimetre, which only the near mesh draws (a mature lentisk's far bark 929k to 233k triangles).
- Gorse gets a near mesh like the other shrubs; its far mesh starts at the lighter spine, flower and pod levels (2.5M to 1.2M triangles mature).

### Fixed

- Trees and shrubs: no more "index_count >= index_array_len" errors from LOD levels no smaller than their base.
