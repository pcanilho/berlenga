### Added

- PlantSpecies `near_mesh_source` and `near_mesh_key`: near meshes on demand. VegetationField3D asks the source for a cell's near mesh once the camera is within 1.2 x `near_range` and frees the cell beyond 1.5 x, so heavy near meshes exist only for plants that come close.
