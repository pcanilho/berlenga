### Changed

- GrassField3D measures its far colour from a small render of its own blades (`get_far_color()`, `is_far_color_baked()`, signal `far_color_changed`), so the far meadow matches the blades for any grass look, lean included; VegetationField3D's cover map follows it.
