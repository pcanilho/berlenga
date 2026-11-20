### Fixed
- Path tracing: a mesh whose vertex() samples a texture now follows that texture when it is rewritten in place (`ImageTexture.update`, `texture_2d_update`, a 3D texture update, `texture_replace`), material or global; it kept the shape from before the change until something else moved it.
