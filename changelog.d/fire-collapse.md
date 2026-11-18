### Added

- Burning buildings and props break apart: a `PhysXBurnable3D` on a `PhysXDestructible3D` loses strength as its timber chars, so a burning wall or shed breaks into pieces that fall and keep their char. `collapse_share`; `PhysXDestructible3D.set_material_remap()`, `set_instance_shader_parameter()`, `get_render_materials()`.
