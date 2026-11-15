### Changed

- GrassField3D: a settings change no longer empties the field or stalls a frame: the old tiles stay drawn until their successors are built (nearest first), the compiled blade shader is kept when its code is unchanged, and the ground tint map is re-baked over several frames (1.5 ms each).
