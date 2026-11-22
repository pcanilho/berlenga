/**************************************************************************/
/*  material.cpp                                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "material.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/error/error_macros.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/version.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/texture.h"
#include "servers/rendering/rendering_rt_material_state.h"
#include "servers/rendering/rendering_server.h"

void Material::set_next_pass(const Ref<Material> &p_pass) {
	for (Ref<Material> pass_child = p_pass; pass_child.is_valid(); pass_child = pass_child->get_next_pass()) {
		ERR_FAIL_COND_MSG(pass_child == this, "Can't set as next_pass one of its parents to prevent crashes due to recursive loop.");
	}

	if (next_pass == p_pass) {
		return;
	}

	next_pass = p_pass;

	if (material.is_valid()) {
		RID next_pass_rid;
		if (next_pass.is_valid()) {
			next_pass_rid = next_pass->get_rid();
		}

		RS::get_singleton()->material_set_next_pass(material, next_pass_rid);
	}
}

Ref<Material> Material::get_next_pass() const {
	return next_pass;
}

void Material::set_render_priority(int p_priority) {
	ERR_FAIL_COND(p_priority < RENDER_PRIORITY_MIN);
	ERR_FAIL_COND(p_priority > RENDER_PRIORITY_MAX);

	render_priority = p_priority;

	if (material.is_valid()) {
		RS::get_singleton()->material_set_render_priority(material, p_priority);
	}
}

int Material::get_render_priority() const {
	return render_priority;
}

RID Material::get_rid() const {
	return material;
}

void Material::_validate_property(PropertyInfo &p_property) const {
	if (!_can_do_next_pass() && p_property.name == "next_pass") {
		p_property.usage = PROPERTY_USAGE_NONE;
	} else if (!_can_use_render_priority() && p_property.name == "render_priority") {
		p_property.usage = PROPERTY_USAGE_NONE;
	}
}

void Material::_mark_ready() {
	init_state = INIT_STATE_READY;
}

void Material::_mark_initialized(const Callable &p_add_to_dirty_list, const Callable &p_update_shader) {
	// If this is happening as part of resource loading, it is not safe to queue the update
	// as an addition to the dirty list. It would be if the load is happening on the main thread,
	// but even so we'd rather perform the update directly instead of using the dirty list.
	if (ResourceLoader::is_within_load()) {
		DEV_ASSERT(init_state != INIT_STATE_READY);
		if (init_state == INIT_STATE_UNINITIALIZED) { // Prevent queueing twice.
			if (p_update_shader.is_valid()) {
				init_state = INIT_STATE_INITIALIZING;
				callable_mp(this, &Material::_mark_ready).call_deferred();
				p_update_shader.call_deferred();
			} else {
				init_state = INIT_STATE_READY;
			}
		}
	} else {
		// Straightforward conditions.
		init_state = INIT_STATE_READY;
		p_add_to_dirty_list.call();
	}
}

void Material::inspect_native_shader_code() {
	SceneTree *st = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
	RID shader = get_shader_rid();
	if (st && shader.is_valid()) {
		st->call_group_flags(SceneTree::GROUP_CALL_DEFERRED, "_native_shader_source_visualizer", "_inspect_shader", shader);
	}
}

RID Material::get_shader_rid() const {
	RID ret;
	GDVIRTUAL_CALL(_get_shader_rid, ret);
	return ret;
}
Shader::Mode Material::get_shader_mode() const {
	Shader::Mode ret = Shader::MODE_MAX;
	GDVIRTUAL_CALL(_get_shader_mode, ret);
	return ret;
}

bool Material::_can_do_next_pass() const {
	bool ret = false;
	GDVIRTUAL_CALL(_can_do_next_pass, ret);
	return ret;
}

bool Material::_can_use_render_priority() const {
	bool ret = false;
	GDVIRTUAL_CALL(_can_use_render_priority, ret);
	return ret;
}

Ref<Resource> Material::create_placeholder() const {
	Ref<PlaceholderMaterial> placeholder;
	placeholder.instantiate();
	return placeholder;
}

void Material::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_next_pass", "next_pass"), &Material::set_next_pass);
	ClassDB::bind_method(D_METHOD("get_next_pass"), &Material::get_next_pass);

	ClassDB::bind_method(D_METHOD("set_render_priority", "priority"), &Material::set_render_priority);
	ClassDB::bind_method(D_METHOD("get_render_priority"), &Material::get_render_priority);

	ClassDB::bind_method(D_METHOD("inspect_native_shader_code"), &Material::inspect_native_shader_code);
	ClassDB::set_method_flags(get_class_static(), StringName("inspect_native_shader_code"), METHOD_FLAGS_DEFAULT | METHOD_FLAG_EDITOR);

	ClassDB::bind_method(D_METHOD("create_placeholder"), &Material::create_placeholder);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "render_priority", PROPERTY_HINT_RANGE, itos(RENDER_PRIORITY_MIN) + "," + itos(RENDER_PRIORITY_MAX) + ",1"), "set_render_priority", "get_render_priority");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "next_pass", PROPERTY_HINT_RESOURCE_TYPE, Material::get_class_static()), "set_next_pass", "get_next_pass");

	BIND_CONSTANT(RENDER_PRIORITY_MAX);
	BIND_CONSTANT(RENDER_PRIORITY_MIN);

	GDVIRTUAL_BIND(_get_shader_rid)
	GDVIRTUAL_BIND(_get_shader_mode)
	GDVIRTUAL_BIND(_can_do_next_pass)
	GDVIRTUAL_BIND(_can_use_render_priority)
}

Material::Material() {
	render_priority = 0;
}

Material::~Material() {
	if (material.is_valid()) {
		ERR_FAIL_NULL(RenderingServer::get_singleton());
		RenderingServer::get_singleton()->free_rid(material);
	}
}

///////////////////////////////////

bool ShaderMaterial::_set(const StringName &p_name, const Variant &p_value) {
	if (shader.is_valid()) {
		const StringName *sn = remap_cache.getptr(p_name);
		if (sn) {
			set_shader_parameter(*sn, p_value);
			return true;
		}
		String s = p_name;
		if (s.begins_with("shader_parameter/")) {
			String param = s.replace_first("shader_parameter/", "");
			remap_cache[s] = param;
			set_shader_parameter(param, p_value);
			return true;
		}
#ifndef DISABLE_DEPRECATED
		// Compatibility remaps are only needed here.
		if (s.begins_with("param/")) {
			s = s.replace_first("param/", "shader_parameter/");
		} else if (s.begins_with("shader_param/")) {
			s = s.replace_first("shader_param/", "shader_parameter/");
		} else if (s.begins_with("shader_uniform/")) {
			s = s.replace_first("shader_uniform/", "shader_parameter/");
		} else {
			return false; // Not a shader parameter.
		}

		WARN_PRINT("This material (containing shader with path: '" + shader->get_path() + "') uses an old deprecated parameter names. Consider re-saving this resource (or scene which contains it) in order for it to continue working in future versions.");
		String param = s.replace_first("shader_parameter/", "");
		remap_cache[s] = param;
		set_shader_parameter(param, p_value);
		return true;
#endif
	}

	return false;
}

bool ShaderMaterial::_get(const StringName &p_name, Variant &r_ret) const {
	if (shader.is_valid()) {
		const StringName *sn = remap_cache.getptr(p_name);
		if (sn) {
			// Only return a parameter if it was previously set.
			r_ret = get_shader_parameter(*sn);
			return true;
		}
	}

	return false;
}

void ShaderMaterial::_get_property_list(List<PropertyInfo> *p_list) const {
	if (shader.is_valid()) {
		List<PropertyInfo> list;
		shader->get_shader_uniform_list(&list, true);

		HashMap<String, List<PropertyInfo>> groups;
		LocalVector<String> vgroups;

		String last_group = "<None>";

		bool is_none_group_undefined = true;
		bool is_none_group = true;

		for (const PropertyInfo &pi : list) {
			if (pi.usage == PROPERTY_USAGE_GROUP) {
				if (!pi.name.is_empty()) {
					last_group = pi.name;
					is_none_group = false;

					if (!groups.has(last_group)) {
						PropertyInfo info;
						info.usage = PROPERTY_USAGE_GROUP;
						info.name = last_group.capitalize();
						info.hint_string = "shader_parameter/";

						List<PropertyInfo> props;
						props.push_back(info);

						groups.insert(last_group, props);
						vgroups.push_back(last_group);
					}
				} else {
					last_group = "<None>";
					is_none_group = true;
				}
				continue; // Pass group.
			}

			if (is_none_group_undefined && is_none_group) {
				is_none_group_undefined = false;

				PropertyInfo info;
				info.usage = PROPERTY_USAGE_GROUP;
				info.name = "Shader Parameters";
				info.hint_string = "shader_parameter/";

				List<PropertyInfo> props;
				props.push_back(info);

				groups.insert("<None>", props);
				vgroups.push_back("<None>");
			}

			const bool is_uniform_cached = param_cache.has(pi.name);
			bool is_uniform_type_compatible = true;

			if (is_uniform_cached) {
				// Check if the uniform Variant type changed, for example vec3 to vec4.
				const Variant &cached = param_cache.get(pi.name);

				if (cached.is_array()) {
					// Allow some array conversions for backwards compatibility.
					is_uniform_type_compatible = Variant::can_convert(pi.type, cached.get_type());
				} else {
					is_uniform_type_compatible = pi.type == cached.get_type();
				}

#ifndef DISABLE_DEPRECATED
				// PackedFloat32Array -> PackedVector4Array conversion.
				if (!is_uniform_type_compatible && pi.type == Variant::PACKED_VECTOR4_ARRAY && cached.get_type() == Variant::PACKED_FLOAT32_ARRAY) {
					PackedVector4Array varray;
					PackedFloat32Array array = (PackedFloat32Array)cached;

					for (int i = 0; i + 3 < array.size(); i += 4) {
						varray.push_back(Vector4(array[i], array[i + 1], array[i + 2], array[i + 3]));
					}

					param_cache.insert(pi.name, varray);
					is_uniform_type_compatible = true;
				}
#endif

				if (is_uniform_type_compatible && pi.type == Variant::OBJECT && cached.get_type() == Variant::OBJECT) {
					// Check if the Object class (hint string) changed, for example Texture2D sampler to Texture3D.
					// Allow inheritance, Texture2D type sampler should also accept CompressedTexture2D.
					Object *cached_obj = cached;
					if (!cached_obj->is_class(pi.hint_string)) {
						is_uniform_type_compatible = false;
					}
				}
			}

			PropertyInfo info = pi;
			info.name = "shader_parameter/" + info.name;
			if (!is_uniform_cached || !is_uniform_type_compatible) {
				// Property has never been edited or its type changed, retrieve with default value.
				Variant default_value = RenderingServer::get_singleton()->shader_get_parameter_default(shader->get_rid(), pi.name);
				param_cache.insert(pi.name, default_value);
				remap_cache.insert(info.name, pi.name);
			}
			groups[last_group].push_back(info);
		}

		for (const String &group : vgroups) {
			List<PropertyInfo> &prop_infos = groups[group];
			for (const PropertyInfo &item : prop_infos) {
				p_list->push_back(item);
			}
		}
	}
}

bool ShaderMaterial::_property_can_revert(const StringName &p_name) const {
	if (shader.is_valid()) {
		if (remap_cache.has(p_name)) {
			return true;
		}
		const String sname = p_name;
		return sname == "render_priority" || sname == "next_pass";
	}
	return false;
}

bool ShaderMaterial::_property_get_revert(const StringName &p_name, Variant &r_property) const {
	if (shader.is_valid()) {
		const StringName *pr = remap_cache.getptr(p_name);
		if (pr) {
			r_property = RenderingServer::get_singleton()->shader_get_parameter_default(shader->get_rid(), *pr);
			return true;
		} else if (p_name == "render_priority") {
			r_property = 0;
			return true;
		} else if (p_name == "next_pass") {
			r_property = Variant();
			return true;
		}
	}
	return false;
}

void ShaderMaterial::set_shader(const Ref<Shader> &p_shader) {
	// Only connect/disconnect the signal when running in the editor.
	// This can be a slow operation, and `notify_property_list_changed()` (which is called by `_shader_changed()`)
	// does nothing in non-editor builds anyway. See GH-34741 for details.
	if (shader.is_valid() && Engine::get_singleton()->is_editor_hint()) {
		shader->disconnect_changed(callable_mp(this, &ShaderMaterial::_shader_changed));
	}

	shader = p_shader;

	RID rid;
	if (shader.is_valid()) {
		rid = shader->get_rid();

		if (Engine::get_singleton()->is_editor_hint()) {
			shader->connect_changed(callable_mp(this, &ShaderMaterial::_shader_changed));
		}
	}

	RID material_rid = _get_material();
	if (material_rid.is_valid()) {
		RS::get_singleton()->material_set_shader(material_rid, rid);
	}

	notify_property_list_changed(); //properties for shader exposed
	emit_changed();
}

Ref<Shader> ShaderMaterial::get_shader() const {
	return shader;
}

void ShaderMaterial::set_shader_parameter(const StringName &p_param, const Variant &p_value) {
	RID material_rid = _get_material();
	if (p_value.get_type() == Variant::NIL) {
		param_cache.erase(p_param);
		if (material_rid.is_valid()) {
			RS::get_singleton()->material_set_param(material_rid, p_param, Variant());
		}
	} else {
		Variant *v = param_cache.getptr(p_param);
		if (!v) {
			// Never assigned, also update the remap cache.
			remap_cache["shader_parameter/" + p_param.string()] = p_param;
			param_cache.insert(p_param, p_value);
		} else {
			*v = p_value;
		}

		if (p_value.get_type() == Variant::OBJECT) {
			RID tex_rid = p_value;
			if (tex_rid == RID()) {
				param_cache.erase(p_param);

				if (material_rid.is_valid()) {
					RS::get_singleton()->material_set_param(material_rid, p_param, Variant());
				}
			} else if (material_rid.is_valid()) {
				RS::get_singleton()->material_set_param(material_rid, p_param, tex_rid);
			}
		} else if (material_rid.is_valid()) {
			RS::get_singleton()->material_set_param(material_rid, p_param, p_value);
		}
	}
}

Variant ShaderMaterial::get_shader_parameter(const StringName &p_param) const {
	if (param_cache.has(p_param)) {
		return param_cache[p_param];
	} else {
		return Variant();
	}
}

void ShaderMaterial::_shader_changed() {
	notify_property_list_changed(); //update all properties
}

void ShaderMaterial::_check_material_rid() const {
	MutexLock lock(material_rid_mutex);
	if (_get_material().is_null()) {
		RID shader_rid = shader.is_valid() ? shader->get_rid() : RID();
		RID next_pass_rid;
		if (get_next_pass().is_valid()) {
			next_pass_rid = get_next_pass()->get_rid();
		}

		_set_material(RS::get_singleton()->material_create_from_shader(next_pass_rid, get_render_priority(), shader_rid));

		for (KeyValue<StringName, Variant> param : param_cache) {
			if (param.value.get_type() == Variant::OBJECT) {
				RID tex_rid = param.value;
				if (tex_rid.is_valid()) {
					RS::get_singleton()->material_set_param(_get_material(), param.key, tex_rid);
				} else {
					RS::get_singleton()->material_set_param(_get_material(), param.key, Variant());
				}
			} else {
				RS::get_singleton()->material_set_param(_get_material(), param.key, param.value);
			}
		}
	}
}

void ShaderMaterial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_shader", "shader"), &ShaderMaterial::set_shader);
	ClassDB::bind_method(D_METHOD("get_shader"), &ShaderMaterial::get_shader);
	ClassDB::bind_method(D_METHOD("set_shader_parameter", "param", "value"), &ShaderMaterial::set_shader_parameter);
	ClassDB::bind_method(D_METHOD("get_shader_parameter", "param"), &ShaderMaterial::get_shader_parameter);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "shader", PROPERTY_HINT_RESOURCE_TYPE, Shader::get_class_static()), "set_shader", "get_shader");
}

#ifdef TOOLS_ENABLED
void ShaderMaterial::get_argument_options(const StringName &p_function, int p_idx, List<String> *r_options) const {
	const String pf = p_function;
	if (p_idx == 0 && (pf == "get_shader_parameter" || pf == "set_shader_parameter")) {
		if (shader.is_valid()) {
			List<PropertyInfo> pl;
			shader->get_shader_uniform_list(&pl);
			for (const PropertyInfo &E : pl) {
				r_options->push_back(E.name.replace_first("shader_parameter/", "").quote());
			}
		}
	}
	Material::get_argument_options(p_function, p_idx, r_options);
}
#endif

bool ShaderMaterial::_can_do_next_pass() const {
	return shader.is_valid() && shader->get_mode() == Shader::MODE_SPATIAL;
}

bool ShaderMaterial::_can_use_render_priority() const {
	return shader.is_valid() && shader->get_mode() == Shader::MODE_SPATIAL;
}

Shader::Mode ShaderMaterial::get_shader_mode() const {
	if (shader.is_valid()) {
		return shader->get_mode();
	} else {
		return Shader::MODE_SPATIAL;
	}
}

RID ShaderMaterial::get_rid() const {
	_check_material_rid();
	return Material::get_rid();
}

RID ShaderMaterial::get_shader_rid() const {
	if (shader.is_valid()) {
		return shader->get_rid();
	} else {
		return RID();
	}
}

ShaderMaterial::ShaderMaterial() {
	// Material RID will be empty until it is required.
}

ShaderMaterial::~ShaderMaterial() {
}

/////////////////////////////////

HashMap<BaseMaterial3D::MaterialKey, BaseMaterial3D::ShaderData, BaseMaterial3D::MaterialKey> BaseMaterial3D::shader_map;
Mutex BaseMaterial3D::shader_map_mutex;
BaseMaterial3D::ShaderNames *BaseMaterial3D::shader_names = nullptr;
Mutex BaseMaterial3D::material_mutex;
SelfList<BaseMaterial3D>::List BaseMaterial3D::dirty_materials;

void BaseMaterial3D::init_shaders() {
	shader_names = memnew(ShaderNames);

	shader_names->albedo = "albedo";
	shader_names->specular = "specular";
	shader_names->roughness = "roughness";
	shader_names->metallic = "metallic";
	shader_names->emission = "emission";
	shader_names->emission_energy = "emission_energy";
	shader_names->emission_indirect_energy = "emission_indirect_energy";
	shader_names->normal_scale = "normal_scale";
	shader_names->rim = "rim";
	shader_names->rim_tint = "rim_tint";
	shader_names->clearcoat = "clearcoat";
	shader_names->clearcoat_roughness = "clearcoat_roughness";
	shader_names->anisotropy = "anisotropy_ratio";
	shader_names->heightmap_scale = "heightmap_scale";
	shader_names->subsurface_scattering_strength = "subsurface_scattering_strength";
	shader_names->backlight = "backlight";
	shader_names->refraction = "refraction";
	shader_names->transmission = "transmission";
	shader_names->transmission_ior = "transmission_ior";
	shader_names->transmission_attenuation_color = "transmission_attenuation_color";
	shader_names->transmission_attenuation_distance = "transmission_attenuation_distance";
	shader_names->transmission_thickness = "transmission_thickness";
	shader_names->transmission_scatter_strength = "transmission_scatter_strength";
	shader_names->transmission_scatter_color = "transmission_scatter_color";
	shader_names->transmission_scatter_anisotropy = "transmission_scatter_anisotropy";
	shader_names->hair_melanin = "hair_melanin";
	shader_names->hair_melanin_redness = "hair_melanin_redness";
	shader_names->hair_roughness = "hair_roughness";
	shader_names->hair_radial_roughness = "hair_radial_roughness";
	shader_names->hair_cuticle_tilt = "hair_cuticle_tilt";
	shader_names->hair_crown = "hair_crown";
	shader_names->hair_mask_texture_channel = "hair_mask_texture_channel";
	shader_names->wetness_porosity = "wetness_porosity";
	shader_names->wetness_amount = "wetness_amount";
	shader_names->snow_amount = "snow_amount";
	shader_names->snow_max_slope = "snow_max_slope";

	// Wetness reads WeatherMap3D's globals. A shader that names a global uniform only compiles
	// once the global exists, so they are declared here with no data (rect width 0 = no
	// weather, nothing wet); WeatherMap3D fills them. A project's own declaration wins.
	{
		Dictionary state_map;
		state_map["type"] = "sampler2D";
		state_map["value"] = "";
		GLOBAL_DEF(PropertyInfo(Variant::DICTIONARY, "shader_globals/weather_state_map", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), state_map);
		Dictionary map_rect;
		map_rect["type"] = "vec4";
		map_rect["value"] = Vector4();
		GLOBAL_DEF(PropertyInfo(Variant::DICTIONARY, "shader_globals/weather_map_rect", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), map_rect);
		// Snow reads SnowCover3D's globals the same way (rect width 0 = no snowpack, no snow).
		GLOBAL_DEF(PropertyInfo(Variant::DICTIONARY, "shader_globals/snow_map", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), state_map);
		GLOBAL_DEF(PropertyInfo(Variant::DICTIONARY, "shader_globals/snow_map_rect", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), map_rect);
		// Its catch map (same rect): the height the pack lies at, the canopy's load share and closure.
		GLOBAL_DEF(PropertyInfo(Variant::DICTIONARY, "shader_globals/snow_catch_map", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), state_map);
		// And its world tier, the coarse pack over the whole terrain drawn outside the near square.
		GLOBAL_DEF(PropertyInfo(Variant::DICTIONARY, "shader_globals/snow_world_map", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), state_map);
		GLOBAL_DEF(PropertyInfo(Variant::DICTIONARY, "shader_globals/snow_world_rect", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), map_rect);
		// Under the path tracer every ice grain a facet, per ray (docs/ideas/snow.md, follow-up 5). Off by
		// default: it needs many samples or the denoiser; off, the path tracer uses the raster's footprint grid.
		Dictionary exact;
		exact["type"] = "bool";
		exact["value"] = false;
		GLOBAL_DEF(PropertyInfo(Variant::DICTIONARY, "shader_globals/snow_exact_glints", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), exact);
		// Ground deformation reads GroundSurface3D's (rect width 0 = none: nothing lowered, nothing moved).
		GLOBAL_DEF(PropertyInfo(Variant::DICTIONARY, "shader_globals/ground_deform_map", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), state_map);
		GLOBAL_DEF(PropertyInfo(Variant::DICTIONARY, "shader_globals/ground_deform_rect", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), map_rect);
		GLOBAL_DEF(PropertyInfo(Variant::DICTIONARY, "shader_globals/ground_deform_height", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), state_map);
		GLOBAL_DEF(PropertyInfo(Variant::DICTIONARY, "shader_globals/ground_deform_height_rect", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), map_rect);
		GLOBAL_DEF(PropertyInfo(Variant::DICTIONARY, "shader_globals/ground_deform_skirt", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), map_rect);
	}
	shader_names->point_size = "point_size";
	shader_names->uv1_scale = "uv1_scale";
	shader_names->uv1_offset = "uv1_offset";
	shader_names->uv2_scale = "uv2_scale";
	shader_names->uv2_offset = "uv2_offset";
	shader_names->uv1_blend_sharpness = "uv1_blend_sharpness";
	shader_names->uv2_blend_sharpness = "uv2_blend_sharpness";

	shader_names->particles_anim_h_frames = "particles_anim_h_frames";
	shader_names->particles_anim_v_frames = "particles_anim_v_frames";
	shader_names->particles_anim_loop = "particles_anim_loop";
	shader_names->heightmap_min_layers = "heightmap_min_layers";
	shader_names->heightmap_max_layers = "heightmap_max_layers";
	shader_names->heightmap_flip = "heightmap_flip";

	shader_names->grow = "grow";

	shader_names->ao_light_affect = "ao_light_affect";

	shader_names->proximity_fade_distance = "proximity_fade_distance";
	shader_names->distance_fade_min = "distance_fade_min";
	shader_names->distance_fade_max = "distance_fade_max";

	shader_names->msdf_pixel_range = "msdf_pixel_range";
	shader_names->msdf_outline_size = "msdf_outline_size";

	shader_names->metallic_texture_channel = "metallic_texture_channel";
	shader_names->ao_texture_channel = "ao_texture_channel";
	shader_names->clearcoat_texture_channel = "clearcoat_texture_channel";
	shader_names->rim_texture_channel = "rim_texture_channel";
	shader_names->heightmap_texture_channel = "heightmap_texture_channel";
	shader_names->refraction_texture_channel = "refraction_texture_channel";

	shader_names->transmittance_color = "transmittance_color";
	shader_names->transmittance_depth = "transmittance_depth";
	shader_names->transmittance_boost = "transmittance_boost";

	shader_names->texture_names[TEXTURE_ALBEDO] = "texture_albedo";
	shader_names->texture_names[TEXTURE_METALLIC] = "texture_metallic";
	shader_names->texture_names[TEXTURE_ROUGHNESS] = "texture_roughness";
	shader_names->texture_names[TEXTURE_EMISSION] = "texture_emission";
	shader_names->texture_names[TEXTURE_NORMAL] = "texture_normal";
	shader_names->texture_names[TEXTURE_BENT_NORMAL] = "texture_bent_normal";
	shader_names->texture_names[TEXTURE_TRANSMISSION] = "texture_transmission";
	shader_names->texture_names[TEXTURE_HAIR_MASK] = "texture_hair_mask";
	shader_names->texture_names[TEXTURE_RIM] = "texture_rim";
	shader_names->texture_names[TEXTURE_CLEARCOAT] = "texture_clearcoat";
	shader_names->texture_names[TEXTURE_FLOWMAP] = "texture_flowmap";
	shader_names->texture_names[TEXTURE_AMBIENT_OCCLUSION] = "texture_ambient_occlusion";
	shader_names->texture_names[TEXTURE_HEIGHTMAP] = "texture_heightmap";
	shader_names->texture_names[TEXTURE_SUBSURFACE_SCATTERING] = "texture_subsurface_scattering";
	shader_names->texture_names[TEXTURE_SUBSURFACE_TRANSMITTANCE] = "texture_subsurface_transmittance";
	shader_names->texture_names[TEXTURE_BACKLIGHT] = "texture_backlight";
	shader_names->texture_names[TEXTURE_REFRACTION] = "texture_refraction";
	shader_names->texture_names[TEXTURE_DETAIL_MASK] = "texture_detail_mask";
	shader_names->texture_names[TEXTURE_DETAIL_ALBEDO] = "texture_detail_albedo";
	shader_names->texture_names[TEXTURE_DETAIL_NORMAL] = "texture_detail_normal";
	shader_names->texture_names[TEXTURE_ORM] = "texture_orm";

	shader_names->alpha_scissor_threshold = "alpha_scissor_threshold";
	shader_names->alpha_hash_scale = "alpha_hash_scale";

	shader_names->alpha_antialiasing_edge = "alpha_antialiasing_edge";
	shader_names->albedo_texture_size = "albedo_texture_size";
	shader_names->z_clip_scale = "z_clip_scale";
	shader_names->fov_override = "fov_override";

	shader_names->rt_base_material_state = RTBaseMaterialState::PARAM_NAME;
}

HashMap<uint64_t, Ref<StandardMaterial3D>> BaseMaterial3D::materials_for_2d;

void BaseMaterial3D::finish_shaders() {
	materials_for_2d.clear();

	dirty_materials.clear();

	memdelete(shader_names);
	shader_names = nullptr;
}

// Weather lines as shader code (docs/ideas/terrain-material.md). BaseMaterial3D writes these into its own
// shader, and get_wetness_shader_code() / get_snow_shader_code() hand the same text to any other shader
// (terrain shaders above all), so there is one source and the two never drift apart. The path tracer applies
// the same lines for BaseMaterial3D in scene_raytracing_raygen.glsl; a ShaderMaterial runs this code in its own
// hit shader.
String BaseMaterial3D::get_wetness_functions_code(bool p_from_weather) {
	String code;
	if (p_from_weather) {
		code += R"(global uniform sampler2D weather_state_map;
global uniform vec4 weather_map_rect;

// Ground wetness from the weather map (0 dry, 1 soaked); 0 where no weather map is set.
float weather_wetness_at(vec3 p_world_pos) {
	if (weather_map_rect.z <= 0.0) {
		return 0.0;
	}
	vec2 half_texel = 0.5 / vec2(textureSize(weather_state_map, 0));
	vec2 uv = clamp((p_world_pos.xz - weather_map_rect.xy) / weather_map_rect.zw, half_texel, 1.0 - half_texel);
	return textureLod(weather_state_map, uv, 0.0).g;
}
)";
	}
	code += R"(
// Wet surfaces, Lagarde 2013 ("Water drop 3b"): water in the pores darkens porous surfaces and smooths them.
// p_normal_y is the world normal's up component (undersides catch less rain).
void weather_wetness_apply(float p_wet, float p_normal_y, float p_metallic, float p_porosity, inout vec3 r_albedo, inout float r_roughness) {
	float wet = clamp(p_wet, 0.0, 1.0) * smoothstep(-0.6, 0.2, p_normal_y);
	float wet_factor = mix(1.0, 0.2, (1.0 - p_metallic) * p_porosity);
	r_albedo *= mix(1.0, wet_factor, wet);
	r_roughness *= mix(1.0, wet_factor, 0.5 * wet);
}
)";
	return code;
}

String BaseMaterial3D::get_snow_functions_code(bool p_from_weather) {
	String code;
	// Read by snow_glint_apply_specular() under the path tracer, with or without a snowpack.
	code += "global uniform bool snow_exact_glints;\n";
	if (p_from_weather) {
		code += R"(global uniform sampler2D snow_map;
global uniform sampler2D snow_catch_map;
global uniform vec4 snow_map_rect;
global uniform sampler2D snow_world_map;
global uniform vec4 snow_world_rect;

// The snowpack at a world position: (covered share, albedo, melt water, 1 where a pack answers). The world tier
// (the coarse pack over the whole terrain), then the near square over it, faded in across its outer tenth.
vec4 snow_cover_at(vec3 p_world_pos) {
	vec4 s = vec4(0.0);
	bool have = false;
	float under = 0.0;
	float crown = 0.0;
	float crown_load = 0.0;
	if (snow_world_rect.z > 0.0) {
		vec2 wuv = (p_world_pos.xz - snow_world_rect.xy) / snow_world_rect.zw;
		if (all(greaterThanEqual(wuv, vec2(0.0))) && all(lessThanEqual(wuv, vec2(1.0)))) {
			vec2 wh = 0.5 / vec2(textureSize(snow_world_map, 0));
			s = textureLod(snow_world_map, clamp(wuv, wh, 1.0 - wh), 0.0);
			have = true;
		}
	}
	if (snow_map_rect.z > 0.0) {
		vec2 nuv = (p_world_pos.xz - snow_map_rect.xy) / snow_map_rect.zw;
		vec2 ne = min(nuv, 1.0 - nuv);
		// With no world tier there is nothing outside the square: no data there, not its edge stretched out.
		float nw = snow_world_rect.z > 0.0 ? clamp(min(ne.x, ne.y) * 10.0, 0.0, 1.0) : step(0.0, min(ne.x, ne.y));
		if (nw > 0.0) {
			vec2 half_texel = 0.5 / vec2(textureSize(snow_map, 0));
			s = mix(s, textureLod(snow_map, clamp(nuv, half_texel, 1.0 - half_texel), 0.0), nw);
			have = true;
			// Shelter: well under the surface the pack lies on (a roof, an overhang) stays bare; well above it,
			// in a tree crown, the snow is what the canopy holds (docs/ideas/snow.md, follow-ups).
			vec4 c = textureLod(snow_catch_map, clamp(nuv, half_texel, 1.0 - half_texel), 0.0);
			if (c.a > 0.5) {
				float dy = p_world_pos.y - c.r;
				under = (1.0 - smoothstep(-0.8, -0.4, dy)) * nw;
				crown = smoothstep(1.5, 3.0, dy) * step(0.05, c.b) * nw;
				crown_load = c.g;
			}
		}
	}
	if (!have) {
		return vec4(0.0, 0.85, 0.0, 0.0);
	}
	// Snow-covered fraction from depth and density, Niu and Yang (2007):
	// tanh(depth / (2.5 z0 (rho / rho_new)^m)), z0 0.01 m, rho_new 100 kg/m3, m 1.
	float cover = tanh(max(s.r, 0.0) / (0.025 * max(s.a * 10.0, 0.5))) * (1.0 - under);
	// A crown shows its load: half its capacity whitens the upward needles.
	cover = mix(cover, min(crown_load * 2.0, 1.0), crown);
	return vec4(cover, s.g, clamp(s.b, 0.0, 1.0), 1.0);
}
)";
	}
	code += R"(
// Snow weight on a surface from the covered share. Snow slides off past p_max_slope (its angle of repose) and
// holds fully 20 degrees below (p_geo_normal, world). Thin snow settles first on the flattest facets of the
// detail normal (p_normal, world) and leaves the steeper ones bare (the local stability test of Fearing 2000,
// per facet): the covered share goes to the facets that face up the most, so the surface's detail shows
// through in patches; near full cover it buries them.
float snow_cover_weight(float p_cover, float p_max_slope, vec3 p_geo_normal, vec3 p_normal) {
	float cos_max = min(cos(radians(p_max_slope)), 0.999);
	float hold = smoothstep(cos_max, cos(radians(max(p_max_slope - 20.0, 0.0))), p_geo_normal.y);
	float cover = clamp(p_cover, 0.0, 1.0);
	float flat_share = clamp(1.0 - (1.0 - p_normal.y) / 0.3, 0.0, 1.0);
	return hold * max(smoothstep(1.0 - 1.2 * cover, 1.2 - 1.2 * cover, flat_share), smoothstep(0.9, 1.0, cover));
}

// Snow over a surface: albedo from the pack's age, ice F0 0.018 (specular 0.335, as F0 = 0.16 specular squared),
// rough when dry and smoother wet.
void snow_cover_apply(float p_snow, float p_albedo, float p_wet, inout vec3 r_albedo, inout float r_roughness, inout float r_metallic, inout float r_specular) {
	r_albedo = mix(r_albedo, vec3(p_albedo), p_snow);
	r_roughness = mix(r_roughness, mix(0.7, 0.35, p_wet), p_snow);
	r_metallic = mix(r_metallic, 0.0, p_snow);
	r_specular = mix(r_specular, 0.335, p_snow);
}

// Snow sparkle (docs/ideas/snow.md, stage 5; the same function is in scene_raytracing_raygen.glsl and
// SnowSurface3D's shader). A share of the ice grains are smooth facets tilted at random (GGX, alpha 0.4)
// that flash where they mirror a light. Counted per cell of the pixel's footprint (Zirr and Kaplanyan 2016;
// Deliot and Belcour 2023, reduced to one facet per cell): a cell holding n grains has a facet with chance
// min(share n, 1) covering share / chance of it, fading as more facets share the cell. Returns the facet's
// normal and its weight (0: none).
vec4 snow_glint(vec3 p_pos, vec3 p_n, float p_footprint) {
	const float grain = 0.002;
	const float share = 0.25;
	float level = floor(max(log2(max(p_footprint, grain) / grain), 0.0));
	if (level > 5.0) {
		return vec4(p_n, 0.0);
	}
	float cell = grain * exp2(level);
	ivec3 q = ivec3(floor(p_pos / cell));
	uint h = (uint(q.x) * 73856093u) ^ (uint(q.y) * 19349663u) ^ (uint(q.z) * 83492791u) ^ (uint(level) * 2654435761u);
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	float r0 = float(h & 0x3ffu) / 1023.0;
	float r1 = float((h >> 10u) & 0x3ffu) / 1024.0;
	float r2 = float((h >> 20u) & 0x3ffu) / 1023.0;
	float grains = exp2(2.0 * level);
	float chance = min(share * grains, 1.0);
	if (r0 >= chance) {
		return vec4(p_n, 0.0);
	}
	float tan2 = 0.16 * r1 / max(1.0 - r1, 1e-3);
	float ct = inversesqrt(1.0 + tan2);
	float st = sqrt(max(1.0 - ct * ct, 0.0));
	vec3 t = normalize(cross(p_n, abs(p_n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	vec3 b = cross(p_n, t);
	float phi = 6.2831853 * r2;
	vec3 f = normalize(p_n * ct + (t * cos(phi) + b * sin(phi)) * st);
	return vec4(f, share / chance / sqrt(max(share * grains, 1.0)));
}

// Sparkle on dry snow: the facet is a second, near-mirror lobe (the coat's, which takes NORMAL before
// NORMAL_MAP as its normal) weighted by the share of the pixel it covers, so the snow's own shading keeps its
// normal: NORMAL becomes the facet and NORMAL_MAP is solved to land back on the snow's normal. Ice F0 0.018 is
// 0.45 of the coat's 0.04. A film of melt water smooths the grains away. Call last, with the fragment's
// NORMAL, NORMAL_MAP, NORMAL_MAP_DEPTH, CLEARCOAT and CLEARCOAT_ROUGHNESS; p_footprint is the pixel's size in
// metres at the surface.
void snow_glint_apply(vec3 p_world_pos, vec3 p_geo_normal, float p_footprint, float p_snow, float p_wet, mat4 p_view, vec3 p_tangent, vec3 p_binormal, inout vec3 r_normal, inout vec3 r_normal_map, inout float r_normal_map_depth, inout float r_clearcoat, inout float r_clearcoat_roughness) {
	vec4 g = snow_glint(p_world_pos, p_geo_normal, p_footprint);
	float gw = g.w * step(0.5, p_snow) * (1.0 - smoothstep(0.05, 0.3, p_wet));
	if (gw <= 0.0) {
		return;
	}
	vec2 nm = r_normal_map.xy * 2.0 - 1.0;
	vec3 base = normalize(mix(r_normal, p_tangent * nm.x + p_binormal * nm.y + r_normal * sqrt(max(0.0, 1.0 - dot(nm, nm))), r_normal_map_depth));
	vec3 f = normalize((p_view * vec4(g.xyz, 0.0)).xyz);
	mat3 tbf = mat3(p_tangent, p_binormal, f);
	if (abs(determinant(tbf)) > 1e-3) {
		vec3 k = inverse(tbf) * base;
		if (k.z > 0.0) {
			k /= length(k);
			r_normal = f;
			r_normal_map = vec3(k.xy * 0.5 + 0.5, 1.0);
			r_normal_map_depth = 1.0;
			r_clearcoat = 0.45 * gw;
			r_clearcoat_roughness = 0.5;
		}
	}
}

// Exact sparkle under the path tracer (docs/ideas/snow.md, follow-up 5; the same function is in
// scene_raytracing_raygen.glsl and SnowSurface3D's shader): the facet of the 2 mm grain at a point. Every grain
// is an ice facet, its normal GGX-tilted (alpha 0.4); a pixel's samples land on different grains, so the path
// tracer integrates the footprint's own facets (the reference Jakob et al. 2014 and Yan et al. 2014 measure
// their counting models against), at any distance and on any bounce.
vec3 snow_grain_facet(vec3 p_pos, vec3 p_n) {
	const float grain = 0.002;
	ivec3 q = ivec3(floor(p_pos / grain));
	uint h = (uint(q.x) * 73856093u) ^ (uint(q.y) * 19349663u) ^ (uint(q.z) * 83492791u);
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	float r1 = float(h & 0xffffu) / 65536.0;
	float r2 = float(h >> 16u) / 65536.0;
	float tan2 = 0.16 * r1 / max(1.0 - r1, 1e-4);
	float ct = inversesqrt(1.0 + tan2);
	float st = sqrt(max(1.0 - ct * ct, 0.0));
	vec3 t = normalize(cross(p_n, abs(p_n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	vec3 b = cross(p_n, t);
	float phi = 6.2831853 * r2;
	return normalize(p_n * ct + (t * cos(phi) + b * sin(phi)) * st);
}

// snow_glint_apply that also owns the snow's specular (call it in place of snow_glint_apply): under the path
// tracer (with snow_exact_glints) the base specular lies on the facet of the grain hit (SPECULAR_NORMAL, view
// space; the diffuse keeps NORMAL) at the facet's own roughness; elsewhere as above.
void snow_glint_apply_specular(vec3 p_world_pos, vec3 p_geo_normal, float p_footprint, float p_snow, float p_wet, mat4 p_view, vec3 p_tangent, vec3 p_binormal, inout vec3 r_normal, inout vec3 r_normal_map, inout float r_normal_map_depth, inout float r_clearcoat, inout float r_clearcoat_roughness, inout float r_roughness, inout vec3 r_specular_normal) {
	if (!(IN_PATH_TRACER && snow_exact_glints)) {
		snow_glint_apply(p_world_pos, p_geo_normal, p_footprint, p_snow, p_wet, p_view, p_tangent, p_binormal, r_normal, r_normal_map, r_normal_map_depth, r_clearcoat, r_clearcoat_roughness);
		return;
	}
	float gw = step(0.5, p_snow) * (1.0 - smoothstep(0.05, 0.3, p_wet));
	if (gw <= 0.0) {
		return;
	}
	r_specular_normal = normalize((p_view * vec4(snow_grain_facet(p_world_pos, p_geo_normal), 0.0)).xyz);
	r_roughness = mix(r_roughness, 0.22, gw);
}
)";
	return code;
}

String BaseMaterial3D::get_wetness_shader_code() {
	return get_wetness_functions_code(true);
}

String BaseMaterial3D::get_snow_shader_code() {
	return get_snow_functions_code(true);
}

void BaseMaterial3D::_update_shader() {
	if (!_is_initialized()) {
		_mark_ready();
	}

	MaterialKey mk = _compute_key();
	if (mk == current_key) {
		return; //no update required in the end
	}

	{
		// State compiled into the generated code below, which the path tracer's fixed-function hit group never runs.
		RTBaseMaterialState rt;
		rt.uv1_triplanar = flags[FLAG_UV1_USE_TRIPLANAR];
		rt.uv1_world_triplanar = flags[FLAG_UV1_USE_WORLD_TRIPLANAR];
		rt.uv2_triplanar = flags[FLAG_UV2_USE_TRIPLANAR];
		rt.uv2_world_triplanar = flags[FLAG_UV2_USE_WORLD_TRIPLANAR];
		rt.uv2_vertex_transform = detail_uv == DETAIL_UV_2 && !flags[FLAG_UV2_USE_TRIPLANAR];
		rt.proximity_fade = proximity_fade_enabled;
		rt.distance_fade_mode = distance_fade;
		rt.emission = features[FEATURE_EMISSION];
		rt.emission_add = emission_op == EMISSION_OP_ADD;
		rt.emission_on_uv2 = flags[FLAG_EMISSION_ON_UV2];
		rt.orm = orm;
		rt.ambient_occlusion = features[FEATURE_AMBIENT_OCCLUSION];
		rt.roughness_texture_channel = roughness_texture_channel;
		rt.texture_filter = texture_filter;
		rt.clearcoat = features[FEATURE_CLEARCOAT];
		rt.backlight = features[FEATURE_BACKLIGHT];
		rt.transmission = features[FEATURE_TRANSMISSION];
		rt.refraction = features[FEATURE_REFRACTION];
		rt.transmission_volume = features[FEATURE_TRANSMISSION] && !transmission_thin_walled;
		rt.subsurface_scattering = features[FEATURE_SUBSURFACE_SCATTERING];
		rt.subsurface_skin = features[FEATURE_SUBSURFACE_SCATTERING] && flags[FLAG_SUBSURFACE_MODE_SKIN];
		rt.hair = features[FEATURE_HAIR];
		rt.hair_melanin = hair_color_mode == HAIR_COLOR_MELANIN;
		rt.hair_direction = hair_direction;
		rt.hair_cards = hair_cards;
		rt.wetness = features[FEATURE_WETNESS];
		rt.wetness_from_weather = wetness_from_weather;
		rt.snow = features[FEATURE_SNOW];
		rt.snow_from_weather = snow_from_weather;
		rt.ground_deform_patch = features[FEATURE_GROUND_DEFORM] && ground_deform_patch;
		_material_set_param(shader_names->rt_base_material_state, rt.encode());
	}

	{
		MutexLock lock(shader_map_mutex);
		ShaderData *v = shader_map.getptr(current_key);
		if (v) {
			v->users--;
			if (v->users == 0) {
				// Deallocate shader which is no longer in use.
				shader_rid = RID();
				RS::get_singleton()->free_rid(v->shader);
				shader_map.erase(current_key);
			}
		}

		current_key = mk;

		v = shader_map.getptr(mk);
		if (v) {
			shader_rid = v->shader;
			v->users++;

			if (_get_material().is_valid()) {
				RS::get_singleton()->material_set_shader(_get_material(), shader_rid);
			}

			return;
		}
	}

	// From this point, it is possible that multiple threads requesting the same key will
	// race to create the shader. The winner, which is the one found in shader_map, will be
	// used. The losers will free their shader.

	String texfilter_str;
	// Force linear filtering for the heightmap texture, as the heightmap effect
	// looks broken with nearest-neighbor filtering (with and without Deep Parallax).
	String texfilter_height_str;
	switch (texture_filter) {
		case TEXTURE_FILTER_NEAREST:
			texfilter_str = "filter_nearest";
			texfilter_height_str = "filter_linear";
			break;
		case TEXTURE_FILTER_LINEAR:
			texfilter_str = "filter_linear";
			texfilter_height_str = "filter_linear";
			break;
		case TEXTURE_FILTER_NEAREST_WITH_MIPMAPS:
			texfilter_str = "filter_nearest_mipmap";
			texfilter_height_str = "filter_linear_mipmap";
			break;
		case TEXTURE_FILTER_LINEAR_WITH_MIPMAPS:
			texfilter_str = "filter_linear_mipmap";
			texfilter_height_str = "filter_linear_mipmap";
			break;
		case TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC:
			texfilter_str = "filter_nearest_mipmap_anisotropic";
			texfilter_height_str = "filter_linear_mipmap_anisotropic";
			break;
		case TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC:
			texfilter_str = "filter_linear_mipmap_anisotropic";
			texfilter_height_str = "filter_linear_mipmap_anisotropic";
			break;
		case TEXTURE_FILTER_MAX:
			break; // Internal value, skip.
	}

	if (flags[FLAG_USE_TEXTURE_REPEAT]) {
		texfilter_str += ", repeat_enable";
		texfilter_height_str += ", repeat_enable";
	} else {
		texfilter_str += ", repeat_disable";
		texfilter_height_str += ", repeat_disable";
	}

	// Add a comment to describe the shader origin (useful when converting to ShaderMaterial).
	String code = vformat(
			"// NOTE: Shader automatically converted from " GODOT_VERSION_NAME " " GODOT_VERSION_FULL_CONFIG "'s %s.\n\n",
			orm ? "ORMMaterial3D" : "StandardMaterial3D");

	// Define shader type and render mode based on property values.
	code += "shader_type spatial;\nrender_mode ";
	switch (blend_mode) {
		case BLEND_MODE_MIX:
			code += "blend_mix";
			break;
		case BLEND_MODE_ADD:
			code += "blend_add";
			break;
		case BLEND_MODE_SUB:
			code += "blend_sub";
			break;
		case BLEND_MODE_MUL:
			code += "blend_mul";
			break;
		case BLEND_MODE_PREMULT_ALPHA:
			code += "blend_premul_alpha";
			break;
		case BLEND_MODE_OIT:
			code += "blend_oit";
			break;
		case BLEND_MODE_MAX:
			break; // Internal value, skip.
	}

	DepthDrawMode ddm = depth_draw_mode;
	if (features[FEATURE_REFRACTION] || features[FEATURE_TRANSMISSION]) {
		ddm = DEPTH_DRAW_ALWAYS;
	}

	switch (ddm) {
		case DEPTH_DRAW_OPAQUE_ONLY:
			code += ", depth_draw_opaque";
			break;
		case DEPTH_DRAW_ALWAYS:
			code += ", depth_draw_always";
			break;
		case DEPTH_DRAW_DISABLED:
			code += ", depth_draw_never";
			break;
		case DEPTH_DRAW_MAX:
			break; // Internal value, skip.
	}

	switch (cull_mode) {
		case CULL_BACK:
			code += ", cull_back";
			break;
		case CULL_FRONT:
			code += ", cull_front";
			break;
		case CULL_DISABLED:
			code += ", cull_disabled";
			break;
		case CULL_MAX:
			break; // Internal value, skip.
	}
	switch (diffuse_mode) {
		case DIFFUSE_BURLEY:
			code += ", diffuse_burley";
			break;
		case DIFFUSE_LAMBERT:
			code += ", diffuse_lambert";
			break;
		case DIFFUSE_LAMBERT_WRAP:
			code += ", diffuse_lambert_wrap";
			break;
		case DIFFUSE_TOON:
			code += ", diffuse_toon";
			break;
		case DIFFUSE_MAX:
			break; // Internal value, skip.
	}
	switch (specular_mode) {
		case SPECULAR_SCHLICK_GGX:
			code += ", specular_schlick_ggx";
			break;
		case SPECULAR_TOON:
			code += ", specular_toon";
			break;
		case SPECULAR_DISABLED:
			code += ", specular_disabled";
			break;
		case SPECULAR_MAX:
			break; // Internal value, skip.
	}
	if (features[FEATURE_SUBSURFACE_SCATTERING] && flags[FLAG_SUBSURFACE_MODE_SKIN]) {
		code += ", sss_mode_skin";
	}

	if (shading_mode == SHADING_MODE_UNSHADED) {
		code += ", unshaded";
	}
	switch (billboard_mode) {
		case BILLBOARD_ENABLED:
			code += ", billboard";
			break;
		case BILLBOARD_FIXED_Y:
			code += ", billboard_y";
			break;
		case BILLBOARD_PARTICLES:
			code += ", billboard_particles";
			break;
		case BILLBOARD_DISABLED:
		case BILLBOARD_MAX:
			break;
	}
	if (billboard_mode != BILLBOARD_DISABLED && flags[FLAG_BILLBOARD_KEEP_SCALE]) {
		code += ", billboard_keep_scale";
	}
	if (flags[FLAG_FIXED_SIZE]) {
		code += ", fixed_size";
	}
	if (flags[FLAG_DISABLE_DEPTH_TEST]) {
		code += ", depth_test_disabled";
	} else {
		switch (depth_test) {
			case DEPTH_TEST_DEFAULT:
				// depth_test_default is the default behavior, no need to emit it here.
				break;
			case DEPTH_TEST_INVERTED:
				code += ", depth_test_inverted";
				break;
			case DEPTH_TEST_MAX:
				break; // Internal value, skip.
		}
	}
	if (flags[FLAG_PARTICLE_TRAILS_MODE]) {
		code += ", particle_trails";
	}
	if (shading_mode == SHADING_MODE_PER_VERTEX) {
		code += ", vertex_lighting";
	}
	if (flags[FLAG_DONT_RECEIVE_SHADOWS]) {
		code += ", shadows_disabled";
	}
	if (flags[FLAG_DISABLE_AMBIENT_LIGHT]) {
		code += ", ambient_light_disabled";
	}
	if (flags[FLAG_USE_SHADOW_TO_OPACITY]) {
		code += ", shadow_to_opacity";
	}
	if (flags[FLAG_DISABLE_FOG]) {
		code += ", fog_disabled";
	}
	if (flags[FLAG_DISABLE_SPECULAR_OCCLUSION]) {
		code += ", specular_occlusion_disabled";
	}
	if (transparency == TRANSPARENCY_ALPHA_DEPTH_PRE_PASS) {
		code += ", depth_prepass_alpha";
	}

	// Although it's technically possible to do alpha antialiasing without using alpha hash or alpha scissor,
	// it is restricted in the base material because it has no use, and abusing it with regular Alpha blending can
	// saturate the MSAA mask.
	if (transparency == TRANSPARENCY_ALPHA_HASH || transparency == TRANSPARENCY_ALPHA_SCISSOR) {
		// Alpha antialiasing is only useful with ALPHA_HASH or ALPHA_SCISSOR.
		if (alpha_antialiasing_mode == ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE) {
			code += ", alpha_to_coverage";
		} else if (alpha_antialiasing_mode == ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE_AND_TO_ONE) {
			code += ", alpha_to_coverage_and_one";
		}
	}
	if (features[FEATURE_TRANSMISSION] || features[FEATURE_REFRACTION]) {
		// The path tracer traces the light through these surfaces itself (it has no screen to read),
		// so it hits them as opaque surfaces rather than alpha-blending them.
		code += ", rt_force_opaque";
	}

	code += ";\n";

	if (stencil_mode != STENCIL_MODE_DISABLED && stencil_flags != 0) {
		code += "stencil_mode ";

		if (stencil_flags & STENCIL_FLAG_READ) {
			code += "read";
		}

		if (stencil_flags & STENCIL_FLAG_WRITE) {
			if (stencil_flags & STENCIL_FLAG_READ) {
				code += ", ";
			}
			code += "write";
		}

		if (stencil_flags & STENCIL_FLAG_WRITE_DEPTH_FAIL) {
			if (stencil_flags & (STENCIL_FLAG_READ | STENCIL_FLAG_WRITE)) {
				code += ", ";
			}
			code += "write_depth_fail";
		}

		switch (stencil_compare) {
			case STENCIL_COMPARE_ALWAYS:
				code += ", compare_always";
				break;
			case STENCIL_COMPARE_LESS:
				code += ", compare_less";
				break;
			case STENCIL_COMPARE_EQUAL:
				code += ", compare_equal";
				break;
			case STENCIL_COMPARE_LESS_OR_EQUAL:
				code += ", compare_less_or_equal";
				break;
			case STENCIL_COMPARE_GREATER:
				code += ", compare_greater";
				break;
			case STENCIL_COMPARE_NOT_EQUAL:
				code += ", compare_not_equal";
				break;
			case STENCIL_COMPARE_GREATER_OR_EQUAL:
				code += ", compare_greater_or_equal";
				break;
			case STENCIL_COMPARE_MAX:
				break;
		}

		code += vformat(", %s;\n", stencil_reference);
	}

	// Generate list of uniforms.
	code += vformat(R"(
uniform vec4 albedo : source_color;
uniform sampler2D texture_albedo : source_color, %s;
)",
			texfilter_str);

	if (grow_enabled) {
		code += "uniform float grow : hint_range(-16.0, 16.0, 0.001);\n";
	}

	if (proximity_fade_enabled) {
		code += "uniform float proximity_fade_distance : hint_range(0.0, 4096.0, 0.01);\n";
	}
	if (distance_fade != DISTANCE_FADE_DISABLED) {
		code += R"(
uniform float distance_fade_min : hint_range(0.0, 4096.0, 0.01);
uniform float distance_fade_max : hint_range(0.0, 4096.0, 0.01);
)";
	}

	if (flags[FLAG_ALBEDO_TEXTURE_MSDF] && flags[FLAG_UV1_USE_TRIPLANAR]) {
		String msg = "MSDF is not supported on triplanar materials. Ignoring MSDF in favor of triplanar mapping.";
		if (textures[TEXTURE_ALBEDO].is_valid()) {
			WARN_PRINT(vformat("%s (albedo %s): " + msg, get_path(), textures[TEXTURE_ALBEDO]->get_path()));
		} else if (!get_path().is_empty()) {
			WARN_PRINT(vformat("%s: " + msg, get_path()));
		} else {
			WARN_PRINT(msg);
		}
	}

	if (flags[FLAG_ALBEDO_TEXTURE_MSDF] && !flags[FLAG_UV1_USE_TRIPLANAR]) {
		code += R"(
uniform float msdf_pixel_range : hint_range(1.0, 100.0, 1.0);
uniform float msdf_outline_size : hint_range(0.0, 250.0, 1.0);
)";
	}

	// Alpha scissor is only valid if there is no antialiasing edge.
	// Alpha hash is valid whenever, but not with alpha scissor.
	if (transparency == TRANSPARENCY_ALPHA_SCISSOR) {
		code += "uniform float alpha_scissor_threshold : hint_range(0.0, 1.0, 0.001);\n";
	} else if (transparency == TRANSPARENCY_ALPHA_HASH) {
		code += "uniform float alpha_hash_scale : hint_range(0.0, 2.0, 0.01);\n";
	}
	// If alpha antialiasing isn't off, add in the edge variable.
	if (alpha_antialiasing_mode != ALPHA_ANTIALIASING_OFF &&
			(transparency == TRANSPARENCY_ALPHA_SCISSOR || transparency == TRANSPARENCY_ALPHA_HASH)) {
		code += "uniform float alpha_antialiasing_edge : hint_range(0.0, 1.0, 0.01);\n";
	}

	code += "uniform ivec2 albedo_texture_size;\n";
	code += "uniform float point_size : hint_range(0.1, 128.0, 0.1);\n";

	if (!orm) {
		code += vformat(R"(
uniform float roughness : hint_range(0.0, 1.0);
uniform sampler2D texture_metallic : hint_default_white, %s;
uniform vec4 metallic_texture_channel;
)",
				texfilter_str);
		switch (roughness_texture_channel) {
			case TEXTURE_CHANNEL_RED: {
				code += vformat("uniform sampler2D texture_roughness : hint_roughness_r, %s;\n", texfilter_str);
			} break;
			case TEXTURE_CHANNEL_GREEN: {
				code += vformat("uniform sampler2D texture_roughness : hint_roughness_g, %s;\n", texfilter_str);
			} break;
			case TEXTURE_CHANNEL_BLUE: {
				code += vformat("uniform sampler2D texture_roughness : hint_roughness_b, %s;\n", texfilter_str);
			} break;
			case TEXTURE_CHANNEL_ALPHA: {
				code += vformat("uniform sampler2D texture_roughness : hint_roughness_a, %s;\n", texfilter_str);
			} break;
			case TEXTURE_CHANNEL_GRAYSCALE: {
				code += vformat("uniform sampler2D texture_roughness : hint_roughness_gray, %s;\n", texfilter_str);
			} break;
			case TEXTURE_CHANNEL_MAX:
				break; // Internal value, skip.
		}

		code += R"(
uniform float specular : hint_range(0.0, 1.0, 0.01);
uniform float metallic : hint_range(0.0, 1.0, 0.01);
)";
	} else {
		code += "uniform sampler2D texture_orm : hint_roughness_g, " + texfilter_str + ";\n";
	}

	if (billboard_mode == BILLBOARD_PARTICLES) {
		code += R"(
uniform int particles_anim_h_frames : hint_range(1, 128);
uniform int particles_anim_v_frames : hint_range(1, 128);
uniform bool particles_anim_loop;
)";
	}

	if (features[FEATURE_EMISSION]) {
		code += vformat(R"(
uniform sampler2D texture_emission : source_color, hint_default_black, %s;
uniform vec4 emission : source_color;
uniform float emission_energy : hint_range(0.0, 100.0, 0.01);
uniform float emission_indirect_energy : hint_range(0.0, 100.0, 0.01);
)",
				texfilter_str);
	}

	if (features[FEATURE_REFRACTION]) {
		code += vformat(R"(
uniform sampler2D texture_refraction : %s;
uniform float refraction : hint_range(-1.0, 1.0, 0.001);
uniform vec4 refraction_texture_channel;
)",
				texfilter_str);
	}

	if (features[FEATURE_TRANSMISSION]) {
		code += vformat(R"(
uniform float transmission : hint_range(0.0, 1.0, 0.01);
uniform sampler2D texture_transmission : hint_default_white, %s;
uniform float transmission_ior : hint_range(1.0, 3.0, 0.01);
uniform vec4 transmission_attenuation_color : source_color;
uniform float transmission_attenuation_distance : hint_range(0.0, 1000.0, 0.01);
uniform float transmission_thickness : hint_range(0.0, 100.0, 0.001);
uniform float transmission_scatter_strength : hint_range(0.0, 1.0, 0.01);
uniform vec4 transmission_scatter_color : source_color;
uniform float transmission_scatter_anisotropy : hint_range(-0.99, 0.99, 0.01);
)",
				texfilter_str);
	}

	if (features[FEATURE_HAIR]) {
		code += vformat(R"(
uniform float hair_melanin : hint_range(0.0, 1.0, 0.01);
uniform float hair_melanin_redness : hint_range(0.0, 1.0, 0.01);
uniform float hair_roughness : hint_range(0.0, 1.0, 0.01);
uniform float hair_radial_roughness : hint_range(0.0, 1.0, 0.01);
uniform float hair_cuticle_tilt : hint_range(-10.0, 10.0, 0.1);
uniform vec3 hair_crown;
uniform sampler2D texture_hair_mask : hint_default_white, %s;
uniform vec4 hair_mask_texture_channel;
varying vec2 hair_flow;
)",
				texfilter_str);
	}

	if (features[FEATURE_WETNESS]) {
		code += R"(
uniform float wetness_porosity : hint_range(0.0, 1.0, 0.01);
uniform float wetness_amount : hint_range(0.0, 1.0, 0.01);
)";
		code += get_wetness_functions_code(wetness_from_weather);
	}

	if (features[FEATURE_SNOW]) {
		code += R"(
uniform float snow_amount : hint_range(0.0, 1.0, 0.01);
uniform float snow_max_slope : hint_range(0.0, 90.0, 0.1);
)";
		code += get_snow_functions_code(snow_from_weather);
	}

	if (features[FEATURE_REFRACTION] || features[FEATURE_TRANSMISSION]) {
		code += "uniform sampler2D screen_texture : hint_screen_texture, repeat_disable, filter_linear_mipmap;\n";
	}

	if (features[FEATURE_REFRACTION] || (features[FEATURE_TRANSMISSION] && !transmission_thin_walled)) {
		code += "uniform sampler2D depth_texture : hint_depth_texture, repeat_disable, filter_nearest;\n";
	}

	if (features[FEATURE_NORMAL_MAPPING]) {
		code += vformat(R"(
uniform sampler2D texture_normal : hint_roughness_normal, %s;
uniform float normal_scale : hint_range(-16.0, 16.0);
)",
				texfilter_str);
	}
	if (features[FEATURE_BENT_NORMAL_MAPPING]) {
		code += vformat(R"(
uniform sampler2D texture_bent_normal : hint_roughness_normal, %s;
)",
				texfilter_str);
	}
	if (features[FEATURE_RIM]) {
		code += vformat(R"(
uniform float rim : hint_range(0.0, 1.0, 0.01);
uniform float rim_tint : hint_range(0.0, 1.0, 0.01);
uniform sampler2D texture_rim : hint_default_white, %s;
)",
				texfilter_str);
	}
	if (features[FEATURE_CLEARCOAT]) {
		code += vformat(R"(
uniform float clearcoat : hint_range(0.0, 1.0, 0.01);
uniform float clearcoat_roughness : hint_range(0.0, 1.0, 0.01);
uniform sampler2D texture_clearcoat : hint_default_white, %s;
)",
				texfilter_str);
	}
	if (features[FEATURE_ANISOTROPY]) {
		code += vformat(R"(
uniform float anisotropy_ratio : hint_range(0.0, 1.0, 0.01);
uniform sampler2D texture_flowmap : hint_anisotropy, %s;
)",
				texfilter_str);
	}
	if (features[FEATURE_AMBIENT_OCCLUSION]) {
		code += vformat(R"(
uniform sampler2D texture_ambient_occlusion : hint_default_white, %s;
uniform vec4 ao_texture_channel;
uniform float ao_light_affect : hint_range(0.0, 1.0, 0.01);
)",
				texfilter_str);
	}

	if (features[FEATURE_DETAIL]) {
		code += vformat(R"(
uniform sampler2D texture_detail_albedo : source_color, %s;
uniform sampler2D texture_detail_normal : hint_normal, %s;
uniform sampler2D texture_detail_mask : hint_default_white, %s;
)",
				texfilter_str, texfilter_str, texfilter_str);
	}

	if (features[FEATURE_SUBSURFACE_SCATTERING]) {
		code += vformat(R"(
uniform float subsurface_scattering_strength : hint_range(0.0, 1.0, 0.01);
uniform sampler2D texture_subsurface_scattering : hint_default_white, %s;
)",
				texfilter_str);
	}

	if (features[FEATURE_SUBSURFACE_TRANSMITTANCE]) {
		code += vformat(R"(
uniform vec4 transmittance_color : source_color;
uniform float transmittance_depth : hint_range(0.001, 8.0, 0.001);
uniform sampler2D texture_subsurface_transmittance : hint_default_white, %s;
uniform float transmittance_boost : hint_range(0.0, 1.0, 0.01);
)",
				texfilter_str);
	}

	if (features[FEATURE_BACKLIGHT]) {
		code += vformat(R"(
uniform vec4 backlight : source_color;
uniform sampler2D texture_backlight : hint_default_black, %s;
)",
				texfilter_str);
	}

	if (features[FEATURE_HEIGHT_MAPPING]) {
		code += vformat(R"(
uniform sampler2D texture_heightmap : hint_default_black, %s;
uniform float heightmap_scale : hint_range(-16.0, 16.0, 0.001);
uniform int heightmap_min_layers : hint_range(1, 64);
uniform int heightmap_max_layers : hint_range(1, 64);
uniform vec2 heightmap_flip;
)",
				texfilter_height_str);
	}
	if (flags[FLAG_UV1_USE_TRIPLANAR]) {
		code += "varying vec3 uv1_triplanar_pos;\n";
	}
	if (flags[FLAG_UV2_USE_TRIPLANAR]) {
		code += "varying vec3 uv2_triplanar_pos;\n";
	}
	if (flags[FLAG_UV1_USE_TRIPLANAR]) {
		code += R"(
uniform float uv1_blend_sharpness : hint_range(0.0, 150.0, 0.001);
varying vec3 uv1_power_normal;
)";
	}

	if (flags[FLAG_UV2_USE_TRIPLANAR]) {
		code += R"(uniform float uv2_blend_sharpness : hint_range(0.0, 150.0, 0.001);
varying vec3 uv2_power_normal;
)";
	}

	code += R"(
uniform vec3 uv1_scale;
uniform vec3 uv1_offset;
uniform vec3 uv2_scale;
uniform vec3 uv2_offset;
)";

	if (flags[FLAG_USE_Z_CLIP_SCALE]) {
		code += "uniform float z_clip_scale : hint_range(0.01, 1.0, 0.01);\n";
	}

	if (features[FEATURE_GROUND_DEFORM]) {
		code += _ground_deform_code(false, ground_deform_patch);
	}

	if (flags[FLAG_USE_FOV_OVERRIDE]) {
		code += "uniform float fov_override : hint_range(1.0, 179.0, 0.1);\n";
	}

	// Generate vertex shader.
	code += R"(
void vertex() {)";

	if (features[FEATURE_GROUND_DEFORM]) {
		// First, so everything below (triplanar positions, world normals) sees the moved vertex.
		code += "\n	ground_deform_vertex(MODEL_MATRIX, VERTEX, NORMAL, TANGENT, BINORMAL);\n";
	}

	if (flags[FLAG_SRGB_VERTEX_COLOR]) {
		code += R"(
	// Vertex Color is sRGB: Enabled
	if (!OUTPUT_IS_SRGB) {
		COLOR.rgb = mix(
				pow((COLOR.rgb + vec3(0.055)) * (1.0 / (1.0 + 0.055)), vec3(2.4)),
				COLOR.rgb * (1.0 / 12.92),
				lessThan(COLOR.rgb, vec3(0.04045)));
	}
)";
	}
	if (flags[FLAG_USE_POINT_SIZE]) {
		code += R"(
	// Use Point Size: Enabled
	POINT_SIZE = point_size;
)";
	}

	if (shading_mode == SHADING_MODE_PER_VERTEX) {
		code += R"(
	// Shading Mode: Per Vertex
	ROUGHNESS = roughness;
)";
	}

	if (!flags[FLAG_UV1_USE_TRIPLANAR]) {
		code += R"(
	UV = UV * uv1_scale.xy + uv1_offset.xy;
)";
	}

	if (detail_uv == DETAIL_UV_2 && !flags[FLAG_UV2_USE_TRIPLANAR]) {
		// Don't add a newline if the UV assignment above is already performed,
		// so that UV1 and UV2 are closer to each other.
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "\n";
		}
		code += R"(	// Detail UV Layer: UV2
	UV2 = UV2 * uv2_scale.xy + uv2_offset.xy;
)";
	}

	// The billboard and fixed_size transforms come from the render modes emitted above.
	if (billboard_mode == BILLBOARD_PARTICLES) {
		code += R"(
	// Billboard Mode: Particles (flipbook animation)
	float h_frames = float(particles_anim_h_frames);
	float v_frames = float(particles_anim_v_frames);
	float particle_total_frames = float(particles_anim_h_frames * particles_anim_v_frames);
	float particle_frame = floor(INSTANCE_CUSTOM.z * float(particle_total_frames));
	if (!particles_anim_loop) {
		particle_frame = clamp(particle_frame, 0.0, particle_total_frames - 1.0);
	} else {
		particle_frame = mod(particle_frame, particle_total_frames);
	}
	UV /= vec2(h_frames, v_frames);
	UV += vec2(mod(particle_frame, h_frames) / h_frames, floor((particle_frame + 0.5) / h_frames) / v_frames);
)";
	}

	if (flags[FLAG_UV1_USE_TRIPLANAR] || flags[FLAG_UV2_USE_TRIPLANAR]) {
		// Generate tangent and binormal in world space.
		if (flags[FLAG_UV1_USE_WORLD_TRIPLANAR]) {
			code += R"(
	vec3 normal = MODEL_NORMAL_MATRIX * NORMAL;
)";
		} else {
			code += R"(
	vec3 normal = NORMAL;
)";
		}
		code += R"(
	TANGENT = vec3(0.0, 0.0, -1.0) * abs(normal.x);
	TANGENT += vec3(1.0, 0.0, 0.0) * abs(normal.y);
	TANGENT += vec3(1.0, 0.0, 0.0) * abs(normal.z);
)";
		if (flags[FLAG_UV1_USE_WORLD_TRIPLANAR]) {
			code += "	TANGENT = inverse(MODEL_NORMAL_MATRIX) * normalize(TANGENT);\n";
		} else {
			code += "	TANGENT = normalize(TANGENT);\n";
		}

		code += R"(
	BINORMAL = vec3(0.0, 1.0, 0.0) * abs(normal.x);
	BINORMAL += vec3(0.0, 0.0, -1.0) * abs(normal.y);
	BINORMAL += vec3(0.0, 1.0, 0.0) * abs(normal.z);
)";
		if (flags[FLAG_UV1_USE_WORLD_TRIPLANAR]) {
			code += "	BINORMAL = inverse(MODEL_NORMAL_MATRIX) * normalize(BINORMAL);\n";
		} else {
			code += "	BINORMAL = normalize(BINORMAL);\n";
		}
	}

	if (flags[FLAG_UV1_USE_TRIPLANAR]) {
		if (flags[FLAG_UV1_USE_WORLD_TRIPLANAR]) {
			code += R"(
	// UV1 Triplanar: Enabled (with World Triplanar)
	uv1_power_normal = pow(abs(normal), vec3(uv1_blend_sharpness));
	uv1_triplanar_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz * uv1_scale + uv1_offset;
)";
		} else {
			code += R"(
	// UV1 Triplanar: Enabled
	uv1_power_normal = pow(abs(NORMAL), vec3(uv1_blend_sharpness));
	uv1_triplanar_pos = VERTEX * uv1_scale + uv1_offset;
)";
		}
		code += R"(	uv1_power_normal /= dot(uv1_power_normal, vec3(1.0));
	uv1_triplanar_pos *= vec3(1.0, -1.0, 1.0);
)";
	}

	if (flags[FLAG_UV2_USE_TRIPLANAR]) {
		if (flags[FLAG_UV2_USE_WORLD_TRIPLANAR]) {
			code += R"(
	// UV2 Triplanar: Enabled (with World Triplanar)
	uv2_power_normal = pow(abs(mat3(MODEL_MATRIX) * NORMAL), vec3(uv2_blend_sharpness));
	uv2_triplanar_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz * uv2_scale + uv2_offset;
)";
		} else {
			code += R"(
	// UV2 Triplanar: Enabled
	uv2_power_normal = pow(abs(NORMAL), vec3(uv2_blend_sharpness));
	uv2_triplanar_pos = VERTEX * uv2_scale + uv2_offset;
)";
		}
		code += R"(	uv2_power_normal /= dot(uv2_power_normal, vec3(1.0));
	uv2_triplanar_pos *= vec3(1.0, -1.0, 1.0);
)";
	}

	if (grow_enabled) {
		code += R"(
	// Grow: Enabled
	VERTEX += NORMAL * grow;
)";
	}

	if (flags[FLAG_USE_Z_CLIP_SCALE]) {
		code += R"(
	Z_CLIP_SCALE = z_clip_scale;
)";
	}

	if (flags[FLAG_USE_FOV_OVERRIDE]) {
		code += R"(
	if (!IN_SHADOW_PASS) {
		float flip_y = sign(PROJECTION_MATRIX[1][1]);
		float aspect = PROJECTION_MATRIX[1][1] / PROJECTION_MATRIX[0][0];
		float f = flip_y / tan(fov_override * PI / 360.0);
		PROJECTION_MATRIX[0][0] = f / aspect;
		PROJECTION_MATRIX[1][1] = f;
	}
)";
	}

	if (features[FEATURE_HAIR]) {
		// The strand direction in the mesh's tangent space, for the anisotropic highlight below.
		switch (hair_direction) {
			case HAIR_DIRECTION_CROWN: {
				code += R"(
	// Hair: strands run away from the crown along the surface.
	vec3 hair_dir = VERTEX - hair_crown;
	hair_dir -= dot(hair_dir, NORMAL) * NORMAL;
	hair_flow = vec2(dot(hair_dir, TANGENT), dot(hair_dir, BINORMAL));
)";
			} break;
			case HAIR_DIRECTION_TANGENT: {
				code += "	hair_flow = vec2(1.0, 0.0);\n";
			} break;
			default: {
				code += "	hair_flow = vec2(0.0, 1.0);\n";
			} break;
		}
	}

	// End of the vertex shader function.
	code += "}\n";

	if (flags[FLAG_ALBEDO_TEXTURE_MSDF] && !flags[FLAG_UV1_USE_TRIPLANAR]) {
		code += R"(
float msdf_median(float r, float g, float b) {
	return max(min(r, g), min(max(r, g), b));
}
)";
	}

	if (flags[FLAG_UV1_USE_TRIPLANAR] || flags[FLAG_UV2_USE_TRIPLANAR]) {
		code += R"(
vec4 triplanar_texture(sampler2D p_sampler, vec3 p_weights, vec3 p_triplanar_pos) {
	vec4 samp = vec4(0.0);
	samp += texture(p_sampler, p_triplanar_pos.xy) * p_weights.z;
	samp += texture(p_sampler, p_triplanar_pos.xz) * p_weights.y;
	samp += texture(p_sampler, p_triplanar_pos.zy * vec2(-1.0, 1.0)) * p_weights.x;
	return samp;
}
)";
	}

	// Generate fragment shader.
	code += R"(
void fragment() {)";

	if (!flags[FLAG_UV1_USE_TRIPLANAR]) {
		code += R"(
	vec2 base_uv = UV;
)";
	}

	if ((features[FEATURE_DETAIL] && detail_uv == DETAIL_UV_2) || (features[FEATURE_AMBIENT_OCCLUSION] && flags[FLAG_AO_ON_UV2]) || (features[FEATURE_EMISSION] && flags[FLAG_EMISSION_ON_UV2])) {
		// Don't add a newline if the UV assignment above is already performed,
		// so that UV1 and UV2 are closer to each other.
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "\n";
		}
		code += R"(	vec2 base_uv2 = UV2;
)";
	}

	if (features[FEATURE_HEIGHT_MAPPING] && flags[FLAG_UV1_USE_TRIPLANAR]) {
		// Display both resource name and albedo texture name.
		// Materials are often built-in to scenes, so displaying the resource name alone may not be meaningful.
		// On the other hand, albedo textures are almost always external to the scene.
		if (textures[TEXTURE_ALBEDO].is_valid()) {
			WARN_PRINT(vformat("%s (albedo %s): Height mapping is not supported on triplanar materials. Ignoring height mapping in favor of triplanar mapping.", get_path(), textures[TEXTURE_ALBEDO]->get_path()));
		} else if (!get_path().is_empty()) {
			WARN_PRINT(vformat("%s: Height mapping is not supported on triplanar materials. Ignoring height mapping in favor of triplanar mapping.", get_path()));
		} else {
			// Resource wasn't saved yet.
			WARN_PRINT("Height mapping is not supported on triplanar materials. Ignoring height mapping in favor of triplanar mapping.");
		}
	}

	// Heightmapping isn't supported at the same time as triplanar mapping.
	if (features[FEATURE_HEIGHT_MAPPING] && !flags[FLAG_UV1_USE_TRIPLANAR]) {
		// Binormal is negative due to mikktspace. Flipping it "unflips" it.
		code += R"(
	{
		// Height: Enabled
		vec3 view_dir = normalize(normalize(-VERTEX + EYE_OFFSET) * mat3(TANGENT * heightmap_flip.x, -BINORMAL * heightmap_flip.y, NORMAL));
)";

		if (deep_parallax) {
			// Multiply the heightmap scale by 0.01 to improve heightmap scale usability.
			code += R"(
		// Height Deep Parallax: Enabled
		float num_layers = mix(float(heightmap_max_layers), float(heightmap_min_layers), abs(dot(vec3(0.0, 0.0, 1.0), view_dir)));
		float layer_depth = 1.0 / num_layers;
		float current_layer_depth = 0.0;
		vec2 p = view_dir.xy * heightmap_scale * 0.01;
		vec2 delta = p / num_layers;
		vec2 ofs = base_uv;
)";
			if (flags[FLAG_INVERT_HEIGHTMAP]) {
				code += "		float depth = texture(texture_heightmap, ofs).r;\n";
			} else {
				code += "		float depth = 1.0 - texture(texture_heightmap, ofs).r;\n";
			}
			code += R"(
		float current_depth = 0.0;
		while (current_depth < depth) {
			ofs -= delta;
)";
			if (flags[FLAG_INVERT_HEIGHTMAP]) {
				code += "			depth = texture(texture_heightmap, ofs).r;\n";
			} else {
				code += "			depth = 1.0 - texture(texture_heightmap, ofs).r;\n";
			}
			code += R"(
			current_depth += layer_depth;
		}

		vec2 prev_ofs = ofs + delta;
		float after_depth = depth - current_depth;
)";
			if (flags[FLAG_INVERT_HEIGHTMAP]) {
				code += "		float before_depth = texture(texture_heightmap, prev_ofs).r - current_depth + layer_depth;\n";
			} else {
				code += "		float before_depth = (1.0 - texture(texture_heightmap, prev_ofs).r) - current_depth + layer_depth;\n";
			}
			code += R"(
		float weight = after_depth / (after_depth - before_depth);
		ofs = mix(ofs, prev_ofs, weight);
)";

		} else {
			if (flags[FLAG_INVERT_HEIGHTMAP]) {
				code += "		float depth = texture(texture_heightmap, base_uv).r;\n";
			} else {
				code += "		float depth = 1.0 - texture(texture_heightmap, base_uv).r;\n";
			}
			// Use offset limiting to improve the appearance of non-deep parallax.
			// This reduces the impression of depth, but avoids visible warping in the distance.
			// Multiply the heightmap scale by 0.01 to improve heightmap scale usability.
			code += "		vec2 ofs = base_uv - view_dir.xy * depth * heightmap_scale * 0.01;\n";
		}

		code += "		base_uv = ofs;\n";
		if (features[FEATURE_DETAIL] && detail_uv == DETAIL_UV_2) {
			code += "		base_uv2 -= ofs;\n";
		}

		code += "	}\n";
	}

	if (flags[FLAG_USE_POINT_SIZE]) {
		code += R"(
	// Use Point Size: Enabled
	vec4 albedo_tex = texture(texture_albedo, POINT_COORD);
)";
	} else {
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += R"(
	vec4 albedo_tex = triplanar_texture(texture_albedo, uv1_power_normal, uv1_triplanar_pos);
)";
		} else {
			code += R"(
	vec4 albedo_tex = texture(texture_albedo, base_uv);
)";
		}
	}

	if (flags[FLAG_ALBEDO_TEXTURE_MSDF] && !flags[FLAG_UV1_USE_TRIPLANAR]) {
		code += R"(
	{
		// Albedo Texture MSDF: Enabled
		if (!OUTPUT_IS_SRGB) {
			albedo_tex.rgb = mix(
					vec3(1.0 + 0.055) * pow(albedo_tex.rgb, vec3(1.0 / 2.4)) - vec3(0.055),
					vec3(12.92) * albedo_tex.rgb,
					lessThan(albedo_tex.rgb, vec3(0.0031308)));
		}
		vec2 msdf_size = vec2(msdf_pixel_range) / vec2(albedo_texture_size);
)";
		if (flags[FLAG_USE_POINT_SIZE]) {
			code += "		vec2 dest_size = vec2(1.0) / fwidth(POINT_COORD);\n";
		} else {
			code += "		vec2 dest_size = vec2(1.0) / fwidth(base_uv);\n";
		}
		code += R"(
		float px_size = max(0.5 * dot(msdf_size, dest_size), 1.0);
		float d = msdf_median(albedo_tex.r, albedo_tex.g, albedo_tex.b);
		if (msdf_outline_size > 0.0) {
			float cr = clamp(msdf_outline_size, 0.0, (msdf_pixel_range / 2.0) - 1.0) / msdf_pixel_range;
			d = min(d, albedo_tex.a);
			albedo_tex.a = clamp((d - 0.5 + cr) * px_size, 0.0, 1.0);
		} else {
			albedo_tex.a = clamp((d - 0.5) * px_size + 0.5, 0.0, 1.0);
		}
		albedo_tex.rgb = vec3(1.0);
	}
)";
	} else if (flags[FLAG_ALBEDO_TEXTURE_FORCE_SRGB]) {
		code += R"(
	// Albedo Texture Force sRGB: Enabled
	albedo_tex.rgb = mix(
			pow((albedo_tex.rgb + vec3(0.055)) * (1.0 / (1.0 + 0.055)), vec3(2.4)),
			albedo_tex.rgb.rgb * (1.0 / 12.92),
			lessThan(albedo_tex.rgb, vec3(0.04045)));
)";
	}

	if (flags[FLAG_ALBEDO_FROM_VERTEX_COLOR]) {
		code += R"(
	// Vertex Color Use as Albedo: Enabled
	albedo_tex *= COLOR;

)";
	}
	code += "	ALBEDO = albedo.rgb * albedo_tex.rgb;\n";

	if (!orm) {
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += R"(
	float metallic_tex = dot(triplanar_texture(texture_metallic, uv1_power_normal, uv1_triplanar_pos), metallic_texture_channel);
)";
		} else {
			code += R"(
	float metallic_tex = dot(texture(texture_metallic, base_uv), metallic_texture_channel);
)";
		}

		code += R"(	METALLIC = metallic_tex * metallic;
	SPECULAR = specular;
)";
		switch (roughness_texture_channel) {
			case TEXTURE_CHANNEL_RED: {
				code += R"(
	vec4 roughness_texture_channel = vec4(1.0, 0.0, 0.0, 0.0);
)";
			} break;
			case TEXTURE_CHANNEL_GREEN: {
				code += R"(
	vec4 roughness_texture_channel = vec4(0.0, 1.0, 0.0, 0.0);
)";
			} break;
			case TEXTURE_CHANNEL_BLUE: {
				code += R"(
	vec4 roughness_texture_channel = vec4(0.0, 0.0, 1.0, 0.0);
)";
			} break;
			case TEXTURE_CHANNEL_ALPHA: {
				code += R"(
	vec4 roughness_texture_channel = vec4(0.0, 0.0, 0.0, 1.0);
)";
			} break;
			case TEXTURE_CHANNEL_GRAYSCALE: {
				code += R"(
	vec4 roughness_texture_channel = vec4(0.333333, 0.333333, 0.333333, 0.0);
)";
			} break;
			case TEXTURE_CHANNEL_MAX:
				break; // Internal value, skip.
		}

		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	float roughness_tex = dot(triplanar_texture(texture_roughness, uv1_power_normal, uv1_triplanar_pos), roughness_texture_channel);\n";
		} else {
			code += "	float roughness_tex = dot(texture(texture_roughness, base_uv), roughness_texture_channel);\n";
		}
		code += R"(	ROUGHNESS = roughness_tex * roughness;
)";
	} else {
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += R"(
	vec4 orm_tex = triplanar_texture(texture_orm, uv1_power_normal, uv1_triplanar_pos);
)";
		} else {
			code += R"(
	vec4 orm_tex = texture(texture_orm, base_uv);
)";
		}

		code += R"(	ROUGHNESS = orm_tex.g;
 	METALLIC = orm_tex.b;
)";
	}

	if (features[FEATURE_NORMAL_MAPPING]) {
		code += R"(
	// Normal Map: Enabled
)";
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	NORMAL_MAP = triplanar_texture(texture_normal, uv1_power_normal, uv1_triplanar_pos).rgb;\n";
		} else {
			code += "	NORMAL_MAP = texture(texture_normal, base_uv).rgb;\n";
		}
		code += "	NORMAL_MAP_DEPTH = normal_scale;\n";
	}

	if (features[FEATURE_BENT_NORMAL_MAPPING]) {
		code += R"(
	// Bent Normal Map: Enabled
)";
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	BENT_NORMAL_MAP = triplanar_texture(texture_bent_normal, uv1_power_normal, uv1_triplanar_pos).rgb;\n";
		} else {
			code += "	BENT_NORMAL_MAP = texture(texture_bent_normal, base_uv).rgb;\n";
		}
	}

	if (features[FEATURE_EMISSION]) {
		code += R"(
	// Emission: Enabled
)";
		if (flags[FLAG_EMISSION_ON_UV2]) {
			if (flags[FLAG_UV2_USE_TRIPLANAR]) {
				code += "	vec3 emission_tex = triplanar_texture(texture_emission, uv2_power_normal, uv2_triplanar_pos).rgb;\n";
			} else {
				code += "	vec3 emission_tex = texture(texture_emission, base_uv2).rgb;\n";
			}
		} else {
			if (flags[FLAG_UV1_USE_TRIPLANAR]) {
				code += "	vec3 emission_tex = triplanar_texture(texture_emission, uv1_power_normal, uv1_triplanar_pos).rgb;\n";
			} else {
				code += "	vec3 emission_tex = texture(texture_emission, base_uv).rgb;\n";
			}
		}

		if (emission_op == EMISSION_OP_ADD) {
			code += R"(	// Emission Operator: Add
	EMISSION = (emission.rgb + emission_tex) * emission_energy;
)";
		} else {
			code += R"(	// Emission Operator: Multiply
	EMISSION = (emission.rgb * emission_tex) * emission_energy;
)";
		}
		code += "	EMISSION_INDIRECT = emission_indirect_energy;\n";
	}

	if (features[FEATURE_REFRACTION]) {
		if (features[FEATURE_NORMAL_MAPPING]) {
			code += R"(
	// Refraction: Enabled (with normal map texture)
	vec3 unpacked_normal = NORMAL_MAP;
	unpacked_normal.xy = unpacked_normal.xy * 2.0 - 1.0;
	unpacked_normal.z = sqrt(max(0.0, 1.0 - dot(unpacked_normal.xy, unpacked_normal.xy)));
	vec3 ref_normal = normalize(mix(
			NORMAL,
			TANGENT * unpacked_normal.x + BINORMAL * unpacked_normal.y + NORMAL * unpacked_normal.z,
			NORMAL_MAP_DEPTH));
)";
		} else {
			code += R"(
	// Refraction: Enabled
	vec3 ref_normal = NORMAL;
)";
		}
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	vec2 ref_ofs = SCREEN_UV - ref_normal.xy * dot(triplanar_texture(texture_refraction, uv1_power_normal, uv1_triplanar_pos), refraction_texture_channel) * refraction;\n";
		} else {
			code += "	vec2 ref_ofs = SCREEN_UV - ref_normal.xy * dot(texture(texture_refraction, base_uv), refraction_texture_channel) * refraction;\n";
		}
		code += R"(
	float ref_amount = 1.0 - albedo.a * albedo_tex.a;

	float refraction_depth_tex = textureLod(depth_texture, ref_ofs, 0.0).r;
	vec4 ndc = OUTPUT_IS_SRGB ? vec4(vec3(SCREEN_UV, refraction_depth_tex) * 2.0 - 1.0, 1.0) : vec4(SCREEN_UV * 2.0 - 1.0, refraction_depth_tex, 1.0);
	vec4 refraction_view_pos = INV_PROJECTION_MATRIX * ndc;
	refraction_view_pos.xyz /= refraction_view_pos.w;

	// If the depth buffer is lower then the model's Z position, use the refracted UV, otherwise use the normal screen UV.
	// At low depth differences, decrease refraction intensity to avoid sudden discontinuities.
	vec2 refraction_uv = mix(SCREEN_UV, ref_ofs, smoothstep(0.0, 1.0, VERTEX.z - refraction_view_pos.z));
)";
		if (!features[FEATURE_TRANSMISSION]) {
			// With Transmission, the transmission code below reads the screen at the refracted UV.
			code += R"(	EMISSION += textureLod(screen_texture, refraction_uv, ROUGHNESS * 8.0).rgb * ref_amount * EXPOSURE;
	ALBEDO *= 1.0 - ref_amount;
	// Force transparency on the material (required for refraction).
	ALPHA = 1.0;
)";
		}
	}

	if (features[FEATURE_TRANSMISSION]) {
		// Thin-walled transmission (glTF KHR_materials_transmission): the scene behind shows through, tinted
		// by the albedo, less the dielectric's Fresnel reflection, and blurred by roughness.
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	float transmission_tex = triplanar_texture(texture_transmission, uv1_power_normal, uv1_triplanar_pos).r;\n";
		} else {
			code += "	float transmission_tex = texture(texture_transmission, base_uv).r;\n";
		}
		const char *transmission_uv = features[FEATURE_REFRACTION] ? "refraction_uv" : "SCREEN_UV";
		code += "	vec3 transmission_absorption = vec3(1.0);\n	vec3 transmission_scatter = vec3(0.0);\n";
		if (!transmission_thin_walled) {
			// Into a medium (glTF KHR_materials_volume, KHR_materials_ior): the view ray refracts by the IOR over
			// the depth behind the surface, and the light it finds is absorbed over that path (Beer-Lambert).
			transmission_uv = "transmission_uv";
			code += R"(	float transmission_depth_tex = textureLod(depth_texture, SCREEN_UV, 0.0).r;
	vec4 transmission_ndc = OUTPUT_IS_SRGB ? vec4(vec3(SCREEN_UV, transmission_depth_tex) * 2.0 - 1.0, 1.0) : vec4(SCREEN_UV * 2.0 - 1.0, transmission_depth_tex, 1.0);
	vec4 transmission_back = INV_PROJECTION_MATRIX * transmission_ndc;
	transmission_back.xyz /= transmission_back.w;
	// The view ray (VIEW points at the eye even in orthogonal projection).
	vec3 transmission_incident = PROJECTION_MATRIX[3][3] == 1.0 ? vec3(0.0, 0.0, -1.0) : normalize(VERTEX);
	vec3 transmission_ray = refract(transmission_incident, NORMAL, 1.0 / max(transmission_ior, 1.0));
	vec3 transmission_start = VERTEX;
	float transmission_inside = -1.0; // path inside the medium; below 0: up to what lies behind
	if (transmission_thickness > 0.0) {
		// A solid of this thickness (glTF KHR_materials_volume thicknessFactor): the ray leaves it through a
		// second surface and bends back there. Filament's solid refraction model: the object as a sphere,
		// the exit point thickness * -dot(N, ray) along the refracted ray, the exit normal from the sphere.
		float transmission_nor = dot(NORMAL, transmission_ray);
		transmission_inside = transmission_thickness * max(-transmission_nor, 0.0);
		transmission_start = VERTEX + transmission_ray * transmission_inside;
		vec3 transmission_exit_n = normalize(transmission_nor * transmission_ray - NORMAL * 0.5);
		vec3 transmission_out = refract(transmission_ray, transmission_exit_n, max(transmission_ior, 1.0));
		// Total internal reflection keeps the ray inside: carry on along it.
		transmission_ray = dot(transmission_out, transmission_out) > 0.0 ? transmission_out : transmission_ray;
	}
	vec4 transmission_clip = PROJECTION_MATRIX * vec4(transmission_start + transmission_ray * min(max(distance(transmission_back.xyz, VERTEX) - max(transmission_inside, 0.0), 0.0), 1.0e4), 1.0);
	vec2 transmission_uv = clamp(transmission_clip.xy / transmission_clip.w * 0.5 + 0.5, vec2(0.0), vec2(1.0));
	transmission_depth_tex = textureLod(depth_texture, transmission_uv, 0.0).r;
	transmission_ndc = OUTPUT_IS_SRGB ? vec4(vec3(transmission_uv, transmission_depth_tex) * 2.0 - 1.0, 1.0) : vec4(transmission_uv * 2.0 - 1.0, transmission_depth_tex, 1.0);
	vec4 transmission_found = INV_PROJECTION_MATRIX * transmission_ndc;
	transmission_found.xyz /= transmission_found.w;
	if (transmission_found.z > VERTEX.z) {
		// The refracted point is in front of the surface: read straight behind it instead.
		transmission_uv = SCREEN_UV;
		transmission_found = transmission_back;
	}
	if (transmission_attenuation_distance > 0.0) {
		float transmission_path = transmission_inside >= 0.0 ? transmission_inside : min(distance(transmission_found.xyz, VERTEX), 1.0e4);
		transmission_absorption = pow(max(transmission_attenuation_color.rgb, vec3(1.0e-4)), vec3(transmission_path / transmission_attenuation_distance));
	}
	// What the medium scatters back looks, after many bounces, like a diffuse surface of its multi-scatter
	// colour (Kulla and Conty 2017, as glTF KHR_materials_scatter defines it), in the share the depth hides.
	transmission_scatter = transmission_scatter_color.rgb * (transmission_scatter_strength * (1.0 - transmission_absorption));
)";
		}
		code += vformat(R"(	// Transmission: Enabled
	float transmission_amount = clamp(transmission * transmission_tex, 0.0, 1.0) * (1.0 - METALLIC);
	float transmission_f0 = 0.16 * SPECULAR * SPECULAR;
	float transmission_fresnel = transmission_f0 + (1.0 - transmission_f0) * pow(1.0 - clamp(dot(NORMAL, VIEW), 0.0, 1.0), 5.0);
	EMISSION += textureLod(screen_texture, %s, ROUGHNESS * 8.0).rgb * ALBEDO * transmission_absorption * (transmission_amount * (1.0 - transmission_fresnel)) * EXPOSURE;
	ALBEDO *= 1.0 - transmission_amount;
	ALBEDO += transmission_scatter * transmission_amount;
	// Force transparency on the material (required to read the screen behind it).
	ALPHA = 1.0;
)",
				transmission_uv);
	} else if (features[FEATURE_REFRACTION]) {
		// Refraction already set ALPHA.
	} else if (transparency != TRANSPARENCY_DISABLED || flags[FLAG_USE_SHADOW_TO_OPACITY] || (distance_fade == DISTANCE_FADE_PIXEL_ALPHA) || proximity_fade_enabled) {
		code += "	ALPHA *= albedo.a * albedo_tex.a;\n";
	}
	if (transparency == TRANSPARENCY_ALPHA_HASH) {
		code += "	ALPHA_HASH_SCALE = alpha_hash_scale;\n";
	} else if (transparency == TRANSPARENCY_ALPHA_SCISSOR) {
		code += "	ALPHA_SCISSOR_THRESHOLD = alpha_scissor_threshold;\n";
	}
	if (alpha_antialiasing_mode != ALPHA_ANTIALIASING_OFF && (transparency == TRANSPARENCY_ALPHA_HASH || transparency == TRANSPARENCY_ALPHA_SCISSOR)) {
		code += "	ALPHA_ANTIALIASING_EDGE = alpha_antialiasing_edge;\n";
		code += "	ALPHA_TEXTURE_COORDINATE = UV * vec2(albedo_texture_size);\n";
	}

	if (proximity_fade_enabled) {
		code += R"(
	// Proximity Fade: Enabled
	float proximity_depth_tex = SCENE_DEPTH;
	vec4 ndc = OUTPUT_IS_SRGB ? vec4(vec3(SCREEN_UV, proximity_depth_tex) * 2.0 - 1.0, 1.0) : vec4(SCREEN_UV * 2.0 - 1.0, proximity_depth_tex, 1.0);
	vec4 proximity_view_pos = INV_PROJECTION_MATRIX * ndc;
	proximity_view_pos.xyz /= proximity_view_pos.w;
	ALPHA *= clamp(1.0 - smoothstep(proximity_view_pos.z + proximity_fade_distance, proximity_view_pos.z, VERTEX.z), 0.0, 1.0);
)";
	}

	if (distance_fade != DISTANCE_FADE_DISABLED) {
		// Use the slightly more expensive circular fade (distance to the object) instead of linear
		// (Z distance), so that the fade is always the same regardless of the camera angle.
		if ((distance_fade == DISTANCE_FADE_OBJECT_DITHER || distance_fade == DISTANCE_FADE_PIXEL_DITHER)) {
			code += "\n	{";

			if (distance_fade == DISTANCE_FADE_OBJECT_DITHER) {
				code += R"(
		// Distance Fade: Object Dither
		float fade_distance = length((VIEW_MATRIX * MODEL_MATRIX[3]));
)";
			} else {
				code += R"(
		// Distance Fade: Pixel Dither
		float fade_distance = length(VERTEX);
)";
			}
			code += R"(
		// Use interleaved gradient noise, which is fast but still looks good.
		const vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
		float fade = clamp(smoothstep(distance_fade_min, distance_fade_max, fade_distance), 0.0, 1.0);
		// Use a hard cap to prevent a few stray pixels from remaining when past the fade-out distance.
		if (fade < 0.001 || fade < fract(magic.z * fract(dot(FRAGCOORD.xy, magic.xy)))) {
			discard;
		}
	}
)";
		} else {
			code += R"(
	// Distance Fade: Pixel Alpha
	ALPHA *= clamp(smoothstep(distance_fade_min, distance_fade_max, length(VERTEX)), 0.0, 1.0);
)";
		}
	}

	if (features[FEATURE_RIM]) {
		code += R"(
	// Rim: Enabled
)";
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	vec2 rim_tex = triplanar_texture(texture_rim, uv1_power_normal, uv1_triplanar_pos).xy;\n";
		} else {
			code += "	vec2 rim_tex = texture(texture_rim, base_uv).xy;\n";
		}
		code += R"(	RIM = rim * rim_tex.x;
	RIM_TINT = rim_tint * rim_tex.y;
)";
	}

	if (features[FEATURE_CLEARCOAT]) {
		code += R"(
	// Clearcoat: Enabled
)";
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	vec2 clearcoat_tex = triplanar_texture(texture_clearcoat, uv1_power_normal, uv1_triplanar_pos).xy;\n";
		} else {
			code += "	vec2 clearcoat_tex = texture(texture_clearcoat, base_uv).xy;\n";
		}
		code += R"(	CLEARCOAT = clearcoat * clearcoat_tex.x;
	CLEARCOAT_ROUGHNESS = clearcoat_roughness * clearcoat_tex.y;
)";
	}

	if (features[FEATURE_ANISOTROPY]) {
		code += R"(
	// Anisotropy: Enabled
)";
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	vec3 anisotropy_tex = triplanar_texture(texture_flowmap, uv1_power_normal, uv1_triplanar_pos).rga;\n";
		} else {
			code += "	vec3 anisotropy_tex = texture(texture_flowmap, base_uv).rga;\n";
		}
		code += R"(	ANISOTROPY = anisotropy_ratio * anisotropy_tex.b;
	ANISOTROPY_FLOW = anisotropy_tex.rg * 2.0 - 1.0;
)";
	}

	if (features[FEATURE_HAIR]) {
		// The rasterizer's stand-in for the path tracer's fibre scattering: GGX stretched across
		// the strands (rough around the fibre, smooth along it), as Kajiya-Kay's highlight band.
		code += "	// Hair: Enabled\n";
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	float hair_mask = dot(triplanar_texture(texture_hair_mask, uv1_power_normal, uv1_triplanar_pos), hair_mask_texture_channel);\n";
		} else {
			code += "	float hair_mask = dot(texture(texture_hair_mask, base_uv), hair_mask_texture_channel);\n";
		}
		if (hair_color_mode == HAIR_COLOR_MELANIN) {
			// The colour a mass of such fibres shows: Chiang et al. 2016's colour-to-absorption fit, inverted.
			code += R"(	float hair_m = -log(max(1.0 - hair_melanin, 1e-4));
	vec3 hair_sigma = hair_m * ((1.0 - hair_melanin_redness) * vec3(0.506, 0.841, 1.653) + hair_melanin_redness * vec3(0.343, 0.733, 1.924));
	float hair_bn = clamp(hair_radial_roughness, 0.01, 1.0);
	float hair_d = 5.969 - 0.215 * hair_bn + 2.532 * hair_bn * hair_bn - 10.73 * pow(hair_bn, 3.0) + 5.574 * pow(hair_bn, 4.0) + 0.245 * pow(hair_bn, 5.0);
	ALBEDO = mix(ALBEDO, exp(-sqrt(hair_sigma) * hair_d), hair_mask);
)";
		}
		code += R"(	vec2 hair_dir_ts = normalize(hair_flow + vec2(1e-6, 0.0));
	ROUGHNESS = mix(ROUGHNESS, hair_roughness, hair_mask);
	METALLIC *= 1.0 - hair_mask;
	ANISOTROPY = mix(ANISOTROPY, 0.8, hair_mask);
	ANISOTROPY_FLOW = mix(ANISOTROPY_FLOW, vec2(-hair_dir_ts.y, hair_dir_ts.x), hair_mask);
)";
	}

	if (features[FEATURE_AMBIENT_OCCLUSION]) {
		code += R"(
	// Ambient Occlusion: Enabled
)";
		if (!orm) {
			if (flags[FLAG_AO_ON_UV2]) {
				if (flags[FLAG_UV2_USE_TRIPLANAR]) {
					code += "	AO = dot(triplanar_texture(texture_ambient_occlusion, uv2_power_normal, uv2_triplanar_pos), ao_texture_channel);\n";
				} else {
					code += "	AO = dot(texture(texture_ambient_occlusion, base_uv2), ao_texture_channel);\n";
				}
			} else {
				if (flags[FLAG_UV1_USE_TRIPLANAR]) {
					code += "	AO = dot(triplanar_texture(texture_ambient_occlusion, uv1_power_normal, uv1_triplanar_pos), ao_texture_channel);\n";
				} else {
					code += "	AO = dot(texture(texture_ambient_occlusion, base_uv), ao_texture_channel);\n";
				}
			}
		} else {
			code += "	AO = orm_tex.r;\n";
		}

		code += "	AO_LIGHT_AFFECT = ao_light_affect;\n";
	}

	if (features[FEATURE_SUBSURFACE_SCATTERING]) {
		code += R"(
	// Subsurface Scattering: Enabled
)";
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	float sss_tex = triplanar_texture(texture_subsurface_scattering, uv1_power_normal, uv1_triplanar_pos).r;\n";
		} else {
			code += "	float sss_tex = texture(texture_subsurface_scattering, base_uv).r;\n";
		}
		code += "	SSS_STRENGTH = subsurface_scattering_strength * sss_tex;\n";
	}

	if (features[FEATURE_SUBSURFACE_TRANSMITTANCE]) {
		code += R"(
	// Subsurface Scattering Transmittance: Enabled
)";
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	vec4 trans_color_tex = triplanar_texture(texture_subsurface_transmittance, uv1_power_normal, uv1_triplanar_pos);\n";
		} else {
			code += "	vec4 trans_color_tex = texture(texture_subsurface_transmittance, base_uv);\n";
		}
		code += "	SSS_TRANSMITTANCE_COLOR = transmittance_color * trans_color_tex;\n";

		code += R"(	SSS_TRANSMITTANCE_DEPTH = transmittance_depth;
	SSS_TRANSMITTANCE_BOOST = transmittance_boost;
)";
	}

	if (features[FEATURE_BACKLIGHT]) {
		code += R"(
	// Backlight: Enabled
)";
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	vec3 backlight_tex = triplanar_texture(texture_backlight, uv1_power_normal, uv1_triplanar_pos).rgb;\n";
		} else {
			code += "	vec3 backlight_tex = texture(texture_backlight, base_uv).rgb;\n";
		}
		code += "	BACKLIGHT = (backlight.rgb + backlight_tex);\n";
	}

	if (features[FEATURE_DETAIL]) {
		code += R"(
	// Detail: Enabled
)";
		const bool triplanar = (flags[FLAG_UV1_USE_TRIPLANAR] && detail_uv == DETAIL_UV_1) || (flags[FLAG_UV2_USE_TRIPLANAR] && detail_uv == DETAIL_UV_2);
		if (triplanar) {
			const String tp_uv = detail_uv == DETAIL_UV_1 ? "uv1" : "uv2";
			code += vformat(R"(	vec4 detail_tex = triplanar_texture(texture_detail_albedo, %s_power_normal, %s_triplanar_pos);
	vec4 detail_norm_tex = triplanar_texture(texture_detail_normal, %s_power_normal, %s_triplanar_pos);
)",
					tp_uv, tp_uv, tp_uv, tp_uv);
		} else {
			const String det_uv = detail_uv == DETAIL_UV_1 ? "base_uv" : "base_uv2";
			code += vformat(R"(	vec4 detail_tex = texture(texture_detail_albedo, %s);
	vec4 detail_norm_tex = texture(texture_detail_normal, %s);
)",
					det_uv, det_uv);
		}

		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	vec4 detail_mask_tex = triplanar_texture(texture_detail_mask, uv1_power_normal, uv1_triplanar_pos);\n";
		} else {
			code += "	vec4 detail_mask_tex = texture(texture_detail_mask, base_uv);\n";
		}

		switch (detail_blend_mode) {
			case BLEND_MODE_MIX: {
				code += R"(
	// Detail Blend Mode: Mix
	vec3 detail = mix(ALBEDO.rgb, detail_tex.rgb, detail_tex.a);
)";
			} break;
			case BLEND_MODE_ADD: {
				code += R"(
	// Detail Blend Mode: Add
	vec3 detail = mix(ALBEDO.rgb, ALBEDO.rgb + detail_tex.rgb, detail_tex.a);
)";
			} break;
			case BLEND_MODE_SUB: {
				code += R"(
	// Detail Blend Mode: Subtract
	vec3 detail = mix(ALBEDO.rgb, ALBEDO.rgb - detail_tex.rgb, detail_tex.a);
)";
			} break;
			case BLEND_MODE_MUL: {
				code += R"(
	// Detail Blend Mode: Multiply
	vec3 detail = mix(ALBEDO.rgb, ALBEDO.rgb * detail_tex.rgb, detail_tex.a);
)";
			} break;
			case BLEND_MODE_PREMULT_ALPHA:
			case BLEND_MODE_OIT: {
				// This is unlikely to ever be used for detail textures, and in order for it to function in the editor, another bit must be used in MaterialKey,
				// but there are only 5 bits left, so I'm going to leave this disabled unless it's actually requested.
				//code += "\tvec3 detail = (1.0-detail_tex.a)*ALBEDO.rgb+detail_tex.rgb;\n";
			} break;
			case BLEND_MODE_MAX:
				break; // Internal value, skip.
		}

		code += R"(	vec3 detail_norm = mix(NORMAL_MAP, detail_norm_tex.rgb, detail_tex.a);
	NORMAL_MAP = mix(NORMAL_MAP, detail_norm, detail_mask_tex.r);
	ALBEDO.rgb = mix(ALBEDO.rgb, detail, detail_mask_tex.r);
)";
	}

	if (features[FEATURE_GROUND_DEFORM] && ground_deform_patch) {
		// Turned ground (docs/ideas/ground-deform.md): darker and, wet, smoother where freshly dug; the path
		// tracer applies the same factors (scene_raytracing_raygen.glsl).
		code += R"(	{
		vec2 gd_tint = ground_deform_tint((INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz);
		ALBEDO *= gd_tint.x;
		ROUGHNESS = clamp(ROUGHNESS * gd_tint.y, 0.0, 1.0);
	}
)";
	}

	if (features[FEATURE_WETNESS]) {
		// Wet surfaces, Lagarde 2013 ("Water drop 3b"): water in the pores darkens porous
		// surfaces and smooths them. The path tracer applies the same lines
		// (scene_raytracing_raygen.glsl), from the same inputs.
		code += R"(
	// Wetness: Enabled
	float wet = wetness_amount;
)";
		if (wetness_from_weather) {
			code += "\twet *= weather_wetness_at((INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz);\n";
		}
		code += R"(	weather_wetness_apply(wet, (INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).y, METALLIC, wetness_porosity, ALBEDO, ROUGHNESS);
)";
	}

	if (features[FEATURE_SNOW]) {
		// Snow cover (docs/ideas/snow.md). The path tracer applies the same lines from the same
		// inputs (scene_raytracing_raygen.glsl), so both renderers show the same snow.
		code += R"(
	// Snow: Enabled
	{
		vec3 snow_n_geo = normalize((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
		vec3 snow_n = snow_n_geo;
)";
		if (features[FEATURE_NORMAL_MAPPING]) {
			code += R"(		vec3 snow_nm = vec3(NORMAL_MAP.xy * 2.0 - 1.0, 0.0);
		snow_nm.z = sqrt(max(0.0, 1.0 - dot(snow_nm.xy, snow_nm.xy)));
		snow_n = normalize((INV_VIEW_MATRIX * vec4(normalize(mix(NORMAL, TANGENT * snow_nm.x + BINORMAL * snow_nm.y + NORMAL * snow_nm.z, NORMAL_MAP_DEPTH)), 0.0)).xyz);
)";
		}
		code += R"(		// (cover share, albedo, melt water, 1 where a pack answers)
		vec4 snow_s = vec4(snow_amount, 0.85, 0.0, 1.0);
)";
		if (snow_from_weather) {
			code += R"(		snow_s = snow_cover_at((INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz);
		snow_s.x *= snow_amount;
)";
		}
		code += R"(		float snow = snow_cover_weight(snow_s.x, snow_max_slope, snow_n_geo, snow_n);
		float snow_wet = snow_s.z;
		float snow_specular = SPECULAR;
		snow_cover_apply(snow, snow_s.y, snow_wet, ALBEDO, ROUGHNESS, METALLIC, snow_specular);
		SPECULAR = snow_specular;
)";
		if (features[FEATURE_NORMAL_MAPPING]) {
			code += "\t\tNORMAL_MAP = mix(NORMAL_MAP, vec3(0.5, 0.5, 1.0), snow);\n";
		}
		code += R"(		float snow_pix = 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
		float snow_fp = PROJECTION_MATRIX[3][3] != 0.0 ? snow_pix : length(VERTEX) * snow_pix;
		snow_glint_apply((INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz, snow_n_geo, snow_fp, snow, snow_wet, VIEW_MATRIX, TANGENT, BINORMAL, NORMAL, NORMAL_MAP, NORMAL_MAP_DEPTH, CLEARCOAT, CLEARCOAT_ROUGHNESS);
	}
)";
	}

	code += "}\n";

	// We must create the shader outside the shader_map_mutex to avoid potential deadlocks with
	// other tasks in the WorkerThreadPool simultaneously creating materials, which
	// may also hold the shared shader_map_mutex lock.
	RID new_shader = RS::get_singleton()->shader_create_from_code(code);
	RS::get_singleton()->shader_set_builtin_standard_3d(new_shader, true);

	MutexLock lock(shader_map_mutex);

	ShaderData *v = shader_map.getptr(mk);
	if (unlikely(v)) {
		// We raced and managed to create the same key concurrently, so we'll free the shader we just created,
		// given we know it isn't used, and use the winner.
		RS::get_singleton()->free_rid(new_shader);
	} else {
		ShaderData shader_data;
		shader_data.shader = new_shader;
		// ShaderData will be inserted with a users count of 0, but we
		// increment unconditionally outside this if block, whilst still under lock.
		v = &shader_map.insert(mk, shader_data)->value;
	}

	shader_rid = v->shader;
	v->users++;

	if (_get_material().is_valid()) {
		RS::get_singleton()->material_set_shader(_get_material(), shader_rid);
	}
}

void BaseMaterial3D::_check_material_rid() {
	MutexLock lock(material_rid_mutex);
	if (_get_material().is_null()) {
		RID next_pass_rid;
		if (get_next_pass().is_valid()) {
			next_pass_rid = get_next_pass()->get_rid();
		}

		_set_material(RS::get_singleton()->material_create_from_shader(next_pass_rid, get_render_priority(), shader_rid));

		for (KeyValue<StringName, Variant> param : pending_params) {
			RS::get_singleton()->material_set_param(_get_material(), param.key, param.value);
		}

		pending_params.clear();
	}
}

void BaseMaterial3D::flush_changes() {
	SelfList<BaseMaterial3D>::List copy;
	{
		MutexLock lock(material_mutex);
		while (SelfList<BaseMaterial3D> *E = dirty_materials.first()) {
			dirty_materials.remove(E);
			copy.add(E);
		}
	}

	while (SelfList<BaseMaterial3D> *E = copy.first()) {
		E->self()->_update_shader();
		copy.remove(E);
	}
}

void BaseMaterial3D::_queue_shader_change() {
	if (!_is_initialized()) {
		return;
	}

	MutexLock lock(material_mutex);

	if (!element.in_list()) {
		dirty_materials.add(&element);
	}
}

void BaseMaterial3D::_material_set_param(const StringName &p_name, const Variant &p_value) {
	if (_get_material().is_valid()) {
		RS::get_singleton()->material_set_param(_get_material(), p_name, p_value);
	} else {
		pending_params[p_name] = p_value;
	}
}

void BaseMaterial3D::set_albedo(const Color &p_albedo) {
	albedo = p_albedo;
	_material_set_param(shader_names->albedo, p_albedo);
}

Color BaseMaterial3D::get_albedo() const {
	return albedo;
}

void BaseMaterial3D::set_specular(float p_specular) {
	specular = p_specular;
	_material_set_param(shader_names->specular, p_specular);
}

float BaseMaterial3D::get_specular() const {
	return specular;
}

void BaseMaterial3D::set_roughness(float p_roughness) {
	roughness = p_roughness;
	_material_set_param(shader_names->roughness, p_roughness);
}

float BaseMaterial3D::get_roughness() const {
	return roughness;
}

void BaseMaterial3D::set_metallic(float p_metallic) {
	metallic = p_metallic;
	_material_set_param(shader_names->metallic, p_metallic);
}

float BaseMaterial3D::get_metallic() const {
	return metallic;
}

void BaseMaterial3D::set_emission(const Color &p_emission) {
	emission = p_emission;
	_material_set_param(shader_names->emission, p_emission);
}

Color BaseMaterial3D::get_emission() const {
	return emission;
}

void BaseMaterial3D::set_emission_energy_multiplier(float p_emission_energy_multiplier) {
	emission_energy_multiplier = p_emission_energy_multiplier;

	if (GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/use_physical_light_units")) {
		_material_set_param(shader_names->emission_energy, p_emission_energy_multiplier * emission_intensity);
	} else {
		_material_set_param(shader_names->emission_energy, p_emission_energy_multiplier);
	}
}

float BaseMaterial3D::get_emission_energy_multiplier() const {
	return emission_energy_multiplier;
}

void BaseMaterial3D::set_emission_intensity(float p_emission_intensity) {
	ERR_FAIL_COND_EDMSG(!GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/use_physical_light_units"), "Cannot set material emission intensity when Physical Light Units disabled.");
	emission_intensity = p_emission_intensity;
	_material_set_param(shader_names->emission_energy, emission_energy_multiplier * emission_intensity);
}

float BaseMaterial3D::get_emission_intensity() const {
	return emission_intensity;
}

void BaseMaterial3D::set_emission_indirect_energy(float p_emission_indirect_energy) {
	emission_indirect_energy = p_emission_indirect_energy;
	_material_set_param(shader_names->emission_indirect_energy, p_emission_indirect_energy);
}

float BaseMaterial3D::get_emission_indirect_energy() const {
	return emission_indirect_energy;
}

void BaseMaterial3D::set_normal_scale(float p_normal_scale) {
	normal_scale = p_normal_scale;
	_material_set_param(shader_names->normal_scale, p_normal_scale);
}

float BaseMaterial3D::get_normal_scale() const {
	return normal_scale;
}

void BaseMaterial3D::set_rim(float p_rim) {
	rim = p_rim;
	_material_set_param(shader_names->rim, p_rim);
}

float BaseMaterial3D::get_rim() const {
	return rim;
}

void BaseMaterial3D::set_rim_tint(float p_rim_tint) {
	rim_tint = p_rim_tint;
	_material_set_param(shader_names->rim_tint, p_rim_tint);
}

float BaseMaterial3D::get_rim_tint() const {
	return rim_tint;
}

void BaseMaterial3D::set_ao_light_affect(float p_ao_light_affect) {
	ao_light_affect = p_ao_light_affect;
	_material_set_param(shader_names->ao_light_affect, p_ao_light_affect);
}

float BaseMaterial3D::get_ao_light_affect() const {
	return ao_light_affect;
}

void BaseMaterial3D::set_clearcoat(float p_clearcoat) {
	clearcoat = p_clearcoat;
	_material_set_param(shader_names->clearcoat, p_clearcoat);
}

float BaseMaterial3D::get_clearcoat() const {
	return clearcoat;
}

void BaseMaterial3D::set_clearcoat_roughness(float p_clearcoat_roughness) {
	clearcoat_roughness = p_clearcoat_roughness;
	_material_set_param(shader_names->clearcoat_roughness, p_clearcoat_roughness);
}

float BaseMaterial3D::get_clearcoat_roughness() const {
	return clearcoat_roughness;
}

void BaseMaterial3D::set_anisotropy(float p_anisotropy) {
	anisotropy = p_anisotropy;
	_material_set_param(shader_names->anisotropy, p_anisotropy);
}

float BaseMaterial3D::get_anisotropy() const {
	return anisotropy;
}

void BaseMaterial3D::set_heightmap_scale(float p_heightmap_scale) {
	heightmap_scale = p_heightmap_scale;
	_material_set_param(shader_names->heightmap_scale, p_heightmap_scale);
}

float BaseMaterial3D::get_heightmap_scale() const {
	return heightmap_scale;
}

void BaseMaterial3D::set_subsurface_scattering_strength(float p_subsurface_scattering_strength) {
	subsurface_scattering_strength = p_subsurface_scattering_strength;
	_material_set_param(shader_names->subsurface_scattering_strength, subsurface_scattering_strength);
}

float BaseMaterial3D::get_subsurface_scattering_strength() const {
	return subsurface_scattering_strength;
}

void BaseMaterial3D::set_transmittance_color(const Color &p_color) {
	transmittance_color = p_color;
	_material_set_param(shader_names->transmittance_color, p_color);
}

Color BaseMaterial3D::get_transmittance_color() const {
	return transmittance_color;
}

void BaseMaterial3D::set_transmittance_depth(float p_depth) {
	transmittance_depth = p_depth;
	_material_set_param(shader_names->transmittance_depth, p_depth);
}

float BaseMaterial3D::get_transmittance_depth() const {
	return transmittance_depth;
}

void BaseMaterial3D::set_transmittance_boost(float p_boost) {
	transmittance_boost = p_boost;
	_material_set_param(shader_names->transmittance_boost, p_boost);
}

float BaseMaterial3D::get_transmittance_boost() const {
	return transmittance_boost;
}

void BaseMaterial3D::set_backlight(const Color &p_backlight) {
	backlight = p_backlight;
	_material_set_param(shader_names->backlight, backlight);
}

Color BaseMaterial3D::get_backlight() const {
	return backlight;
}

void BaseMaterial3D::set_refraction(float p_refraction) {
	refraction = p_refraction;
	_material_set_param(shader_names->refraction, refraction);
}

float BaseMaterial3D::get_refraction() const {
	return refraction;
}

void BaseMaterial3D::set_transmission(float p_transmission) {
	transmission = p_transmission;
	_material_set_param(shader_names->transmission, transmission);
}

float BaseMaterial3D::get_transmission() const {
	return transmission;
}

void BaseMaterial3D::set_transmission_thin_walled(bool p_thin_walled) {
	transmission_thin_walled = p_thin_walled;
	_queue_shader_change();
	notify_property_list_changed();
}

bool BaseMaterial3D::is_transmission_thin_walled() const {
	return transmission_thin_walled;
}

void BaseMaterial3D::set_transmission_ior(float p_ior) {
	transmission_ior = CLAMP(p_ior, 1.0f, 3.0f);
	_material_set_param(shader_names->transmission_ior, transmission_ior);
}

float BaseMaterial3D::get_transmission_ior() const {
	return transmission_ior;
}

void BaseMaterial3D::set_transmission_attenuation_color(const Color &p_color) {
	transmission_attenuation_color = p_color;
	_material_set_param(shader_names->transmission_attenuation_color, transmission_attenuation_color);
}

Color BaseMaterial3D::get_transmission_attenuation_color() const {
	return transmission_attenuation_color;
}

void BaseMaterial3D::set_transmission_attenuation_distance(float p_distance) {
	transmission_attenuation_distance = MAX(p_distance, 0.0f);
	_material_set_param(shader_names->transmission_attenuation_distance, transmission_attenuation_distance);
}

float BaseMaterial3D::get_transmission_attenuation_distance() const {
	return transmission_attenuation_distance;
}

void BaseMaterial3D::set_transmission_thickness(float p_thickness) {
	transmission_thickness = MAX(p_thickness, 0.0f);
	_material_set_param(shader_names->transmission_thickness, transmission_thickness);
}

float BaseMaterial3D::get_transmission_thickness() const {
	return transmission_thickness;
}

void BaseMaterial3D::set_transmission_scatter_strength(float p_strength) {
	transmission_scatter_strength = CLAMP(p_strength, 0.0f, 1.0f);
	_material_set_param(shader_names->transmission_scatter_strength, transmission_scatter_strength);
}

float BaseMaterial3D::get_transmission_scatter_strength() const {
	return transmission_scatter_strength;
}

void BaseMaterial3D::set_hair_color_mode(HairColorMode p_mode) {
	ERR_FAIL_INDEX(p_mode, HAIR_COLOR_MAX);
	hair_color_mode = p_mode;
	_queue_shader_change();
	notify_property_list_changed();
}

BaseMaterial3D::HairColorMode BaseMaterial3D::get_hair_color_mode() const {
	return hair_color_mode;
}

void BaseMaterial3D::set_hair_melanin(float p_melanin) {
	hair_melanin = CLAMP(p_melanin, 0.0f, 1.0f);
	_material_set_param(shader_names->hair_melanin, hair_melanin);
}

float BaseMaterial3D::get_hair_melanin() const {
	return hair_melanin;
}

void BaseMaterial3D::set_hair_melanin_redness(float p_redness) {
	hair_melanin_redness = CLAMP(p_redness, 0.0f, 1.0f);
	_material_set_param(shader_names->hair_melanin_redness, hair_melanin_redness);
}

float BaseMaterial3D::get_hair_melanin_redness() const {
	return hair_melanin_redness;
}

void BaseMaterial3D::set_hair_roughness(float p_roughness) {
	hair_roughness = CLAMP(p_roughness, 0.0f, 1.0f);
	_material_set_param(shader_names->hair_roughness, hair_roughness);
}

float BaseMaterial3D::get_hair_roughness() const {
	return hair_roughness;
}

void BaseMaterial3D::set_hair_radial_roughness(float p_roughness) {
	hair_radial_roughness = CLAMP(p_roughness, 0.0f, 1.0f);
	_material_set_param(shader_names->hair_radial_roughness, hair_radial_roughness);
}

float BaseMaterial3D::get_hair_radial_roughness() const {
	return hair_radial_roughness;
}

void BaseMaterial3D::set_hair_cuticle_tilt(float p_degrees) {
	hair_cuticle_tilt = CLAMP(p_degrees, -10.0f, 10.0f);
	_material_set_param(shader_names->hair_cuticle_tilt, hair_cuticle_tilt);
}

float BaseMaterial3D::get_hair_cuticle_tilt() const {
	return hair_cuticle_tilt;
}

void BaseMaterial3D::set_hair_direction(HairDirection p_direction) {
	ERR_FAIL_INDEX(p_direction, HAIR_DIRECTION_MAX);
	hair_direction = p_direction;
	_queue_shader_change();
	notify_property_list_changed();
}

BaseMaterial3D::HairDirection BaseMaterial3D::get_hair_direction() const {
	return hair_direction;
}

void BaseMaterial3D::set_hair_crown(const Vector3 &p_crown) {
	hair_crown = p_crown;
	_material_set_param(shader_names->hair_crown, hair_crown);
}

Vector3 BaseMaterial3D::get_hair_crown() const {
	return hair_crown;
}

void BaseMaterial3D::set_hair_cards(bool p_cards) {
	hair_cards = p_cards;
	_queue_shader_change();
}

bool BaseMaterial3D::is_hair_cards() const {
	return hair_cards;
}

void BaseMaterial3D::set_transmission_scatter_color(const Color &p_color) {
	transmission_scatter_color = p_color;
	_material_set_param(shader_names->transmission_scatter_color, transmission_scatter_color);
}

Color BaseMaterial3D::get_transmission_scatter_color() const {
	return transmission_scatter_color;
}

void BaseMaterial3D::set_transmission_scatter_anisotropy(float p_anisotropy) {
	transmission_scatter_anisotropy = CLAMP(p_anisotropy, -0.99f, 0.99f);
	_material_set_param(shader_names->transmission_scatter_anisotropy, transmission_scatter_anisotropy);
}

float BaseMaterial3D::get_transmission_scatter_anisotropy() const {
	return transmission_scatter_anisotropy;
}

void BaseMaterial3D::set_detail_uv(DetailUV p_detail_uv) {
	if (detail_uv == p_detail_uv) {
		return;
	}

	detail_uv = p_detail_uv;
	_queue_shader_change();
}

BaseMaterial3D::DetailUV BaseMaterial3D::get_detail_uv() const {
	return detail_uv;
}

void BaseMaterial3D::set_blend_mode(BlendMode p_mode) {
	if (blend_mode == p_mode) {
		return;
	}

	blend_mode = p_mode;
	_queue_shader_change();
}

BaseMaterial3D::BlendMode BaseMaterial3D::get_blend_mode() const {
	return blend_mode;
}

void BaseMaterial3D::set_detail_blend_mode(BlendMode p_mode) {
	detail_blend_mode = p_mode;
	_queue_shader_change();
}

BaseMaterial3D::BlendMode BaseMaterial3D::get_detail_blend_mode() const {
	return detail_blend_mode;
}

void BaseMaterial3D::set_transparency(Transparency p_transparency) {
	if (transparency == p_transparency) {
		return;
	}

	transparency = p_transparency;
	_queue_shader_change();
	notify_property_list_changed();
}

BaseMaterial3D::Transparency BaseMaterial3D::get_transparency() const {
	return transparency;
}

void BaseMaterial3D::set_alpha_antialiasing(AlphaAntiAliasing p_alpha_aa) {
	if (alpha_antialiasing_mode == p_alpha_aa) {
		return;
	}

	alpha_antialiasing_mode = p_alpha_aa;
	_queue_shader_change();
	notify_property_list_changed();
}

BaseMaterial3D::AlphaAntiAliasing BaseMaterial3D::get_alpha_antialiasing() const {
	return alpha_antialiasing_mode;
}

void BaseMaterial3D::set_shading_mode(ShadingMode p_shading_mode) {
	if (shading_mode == p_shading_mode) {
		return;
	}

	shading_mode = p_shading_mode;
	_queue_shader_change();
	notify_property_list_changed();
}

BaseMaterial3D::ShadingMode BaseMaterial3D::get_shading_mode() const {
	return shading_mode;
}

void BaseMaterial3D::set_depth_draw_mode(DepthDrawMode p_mode) {
	if (depth_draw_mode == p_mode) {
		return;
	}

	depth_draw_mode = p_mode;
	_queue_shader_change();
}

BaseMaterial3D::DepthDrawMode BaseMaterial3D::get_depth_draw_mode() const {
	return depth_draw_mode;
}

void BaseMaterial3D::set_depth_test(DepthTest p_func) {
	if (depth_test == p_func) {
		return;
	}

	depth_test = p_func;
	_queue_shader_change();
}

BaseMaterial3D::DepthTest BaseMaterial3D::get_depth_test() const {
	return depth_test;
}

void BaseMaterial3D::set_cull_mode(CullMode p_mode) {
	if (cull_mode == p_mode) {
		return;
	}

	cull_mode = p_mode;
	_queue_shader_change();
}

BaseMaterial3D::CullMode BaseMaterial3D::get_cull_mode() const {
	return cull_mode;
}

void BaseMaterial3D::set_diffuse_mode(DiffuseMode p_mode) {
	if (diffuse_mode == p_mode) {
		return;
	}

	diffuse_mode = p_mode;
	_queue_shader_change();
}

BaseMaterial3D::DiffuseMode BaseMaterial3D::get_diffuse_mode() const {
	return diffuse_mode;
}

void BaseMaterial3D::set_specular_mode(SpecularMode p_mode) {
	if (specular_mode == p_mode) {
		return;
	}

	specular_mode = p_mode;
	_queue_shader_change();
}

BaseMaterial3D::SpecularMode BaseMaterial3D::get_specular_mode() const {
	return specular_mode;
}

void BaseMaterial3D::set_flag(Flags p_flag, bool p_enabled) {
	ERR_FAIL_INDEX(p_flag, FLAG_MAX);

	if (flags[p_flag] == p_enabled) {
		return;
	}

	flags[p_flag] = p_enabled;

	if (
			p_flag == FLAG_USE_SHADOW_TO_OPACITY ||
			p_flag == FLAG_USE_TEXTURE_REPEAT ||
			p_flag == FLAG_SUBSURFACE_MODE_SKIN ||
			p_flag == FLAG_USE_POINT_SIZE ||
			p_flag == FLAG_UV1_USE_TRIPLANAR ||
			p_flag == FLAG_UV2_USE_TRIPLANAR ||
			p_flag == FLAG_USE_Z_CLIP_SCALE ||
			p_flag == FLAG_USE_FOV_OVERRIDE ||
			p_flag == FLAG_DISABLE_DEPTH_TEST) {
		notify_property_list_changed();
	}

	if (p_flag == FLAG_PARTICLE_TRAILS_MODE) {
		update_configuration_warning();
	}

	_queue_shader_change();
}

bool BaseMaterial3D::get_flag(Flags p_flag) const {
	ERR_FAIL_INDEX_V(p_flag, FLAG_MAX, false);
	return flags[p_flag];
}

void BaseMaterial3D::set_feature(Feature p_feature, bool p_enabled) {
	ERR_FAIL_INDEX(p_feature, FEATURE_MAX);
	if (features[p_feature] == p_enabled) {
		return;
	}

	features[p_feature] = p_enabled;
	_queue_shader_change();
}

bool BaseMaterial3D::get_feature(Feature p_feature) const {
	ERR_FAIL_INDEX_V(p_feature, FEATURE_MAX, false);
	return features[p_feature];
}

void BaseMaterial3D::set_texture(TextureParam p_param, const Ref<Texture2D> &p_texture) {
	ERR_FAIL_INDEX(p_param, TEXTURE_MAX);

	textures[p_param] = p_texture;
	Variant rid = p_texture.is_valid() ? Variant(p_texture->get_rid()) : Variant();
	_material_set_param(shader_names->texture_names[p_param], rid);

	if (p_texture.is_valid() && p_param == TEXTURE_ALBEDO) {
		_material_set_param(shader_names->albedo_texture_size, Vector2i(p_texture->get_width(), p_texture->get_height()));
	}

	notify_property_list_changed();
	_queue_shader_change();
}

Ref<Texture2D> BaseMaterial3D::get_texture(TextureParam p_param) const {
	ERR_FAIL_INDEX_V(p_param, TEXTURE_MAX, Ref<Texture2D>());
	return textures[p_param];
}

Ref<Texture2D> BaseMaterial3D::get_texture_by_name(const StringName &p_name) const {
	for (int i = 0; i < (int)BaseMaterial3D::TEXTURE_MAX; i++) {
		TextureParam param = TextureParam(i);
		if (p_name == shader_names->texture_names[param]) {
			return textures[param];
		}
	}
	return Ref<Texture2D>();
}

void BaseMaterial3D::set_texture_filter(TextureFilter p_filter) {
	texture_filter = p_filter;
	_queue_shader_change();
}

BaseMaterial3D::TextureFilter BaseMaterial3D::get_texture_filter() const {
	return texture_filter;
}

void BaseMaterial3D::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == "emission_intensity" && !GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/use_physical_light_units")) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}

	if (p_property.name.begins_with("particles_anim_") && billboard_mode != BILLBOARD_PARTICLES) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}

	if (Engine::get_singleton()->is_editor_hint()) {
		if (p_property.name == "billboard_keep_scale" && billboard_mode == BILLBOARD_DISABLED) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "grow_amount" && !grow_enabled) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "point_size" && !flags[FLAG_USE_POINT_SIZE]) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if ((p_property.name == "transmission_ior" || p_property.name == "transmission_attenuation_color" || p_property.name == "transmission_attenuation_distance" || p_property.name == "transmission_thickness" ||
					p_property.name == "transmission_scatter_strength" || p_property.name == "transmission_scatter_color" || p_property.name == "transmission_scatter_anisotropy") &&
				transmission_thin_walled) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if ((p_property.name == "hair_melanin" || p_property.name == "hair_melanin_redness") && hair_color_mode != HAIR_COLOR_MELANIN) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "hair_crown" && hair_direction != HAIR_DIRECTION_CROWN) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "hair_cards" && hair_direction == HAIR_DIRECTION_STRANDS) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR; // Strands always let light through.
		}

		if (p_property.name == "proximity_fade_distance" && !proximity_fade_enabled) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "msdf_pixel_range" && !flags[FLAG_ALBEDO_TEXTURE_MSDF]) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "msdf_outline_size" && !flags[FLAG_ALBEDO_TEXTURE_MSDF]) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if ((p_property.name == "distance_fade_max_distance" || p_property.name == "distance_fade_min_distance") && distance_fade == DISTANCE_FADE_DISABLED) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if ((p_property.name == "uv1_triplanar_sharpness" || p_property.name == "uv1_world_triplanar") && !flags[FLAG_UV1_USE_TRIPLANAR]) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if ((p_property.name == "uv2_triplanar_sharpness" || p_property.name == "uv2_world_triplanar") && !flags[FLAG_UV2_USE_TRIPLANAR]) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "z_clip_scale" && !flags[FLAG_USE_Z_CLIP_SCALE]) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "fov_override" && !flags[FLAG_USE_FOV_OVERRIDE]) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	}

	// you can only enable anti-aliasing (in materials) on alpha scissor and alpha hash
	const bool can_select_aa = (transparency == TRANSPARENCY_ALPHA_SCISSOR || transparency == TRANSPARENCY_ALPHA_HASH);
	// alpha anti aliasiasing is only enabled when you can select aa
	const bool alpha_aa_enabled = (alpha_antialiasing_mode != ALPHA_ANTIALIASING_OFF) && can_select_aa;

	// alpha scissor slider isn't needed when alpha antialiasing is enabled
	if (p_property.name == "alpha_scissor_threshold" && transparency != TRANSPARENCY_ALPHA_SCISSOR) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}

	// alpha hash scale slider is only needed if transparency is alpha hash
	if (p_property.name == "alpha_hash_scale" && transparency != TRANSPARENCY_ALPHA_HASH) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}

	if (p_property.name == "alpha_antialiasing_mode" && !can_select_aa) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}

	// we can't choose an antialiasing mode if alpha isn't possible
	if (p_property.name == "alpha_antialiasing_edge" && !alpha_aa_enabled) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}

	if (p_property.name == "blend_mode" && alpha_aa_enabled) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}

	if ((p_property.name == "heightmap_min_layers" || p_property.name == "heightmap_max_layers") && !deep_parallax) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}

	if (Engine::get_singleton()->is_editor_hint()) {
		if (p_property.name == "depth_test" && flags[FLAG_DISABLE_DEPTH_TEST]) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "stencil_reference" && stencil_mode == STENCIL_MODE_DISABLED) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if ((p_property.name == "stencil_flags" || p_property.name == "stencil_compare") && stencil_mode != STENCIL_MODE_CUSTOM) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "stencil_color" && stencil_mode != STENCIL_MODE_OUTLINE && stencil_mode != STENCIL_MODE_XRAY) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "stencil_outline_thickness" && stencil_mode != STENCIL_MODE_OUTLINE) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	}

	if (flags[FLAG_SUBSURFACE_MODE_SKIN] && (p_property.name == "subsurf_scatter_transmittance_color" || p_property.name == "subsurf_scatter_transmittance_texture")) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}

	if (orm) {
		if (p_property.name == "shading_mode") {
			// Vertex not supported in ORM mode, since no individual roughness.
			p_property.hint_string = "Unshaded,Per-Pixel";
		}
		if (p_property.name.begins_with("roughness") || p_property.name.begins_with("metallic") || p_property.name.begins_with("ao_texture")) {
			p_property.usage = PROPERTY_USAGE_NONE;
		}

	} else {
		if (p_property.name == "orm_texture") {
			p_property.usage = PROPERTY_USAGE_NONE;
		}
	}

	if (shading_mode != SHADING_MODE_PER_PIXEL) {
		if (shading_mode != SHADING_MODE_PER_VERTEX) {
			//these may still work per vertex
			if (p_property.name.begins_with("ao")) {
				p_property.usage = PROPERTY_USAGE_NONE;
			}
			if (p_property.name.begins_with("emission")) {
				p_property.usage = PROPERTY_USAGE_NONE;
			}

			if (p_property.name.begins_with("metallic")) {
				p_property.usage = PROPERTY_USAGE_NONE;
			}
			if (p_property.name.begins_with("rim")) {
				p_property.usage = PROPERTY_USAGE_NONE;
			}

			if (p_property.name.begins_with("roughness")) {
				p_property.usage = PROPERTY_USAGE_NONE;
			}

			if (p_property.name.begins_with("subsurf_scatter")) {
				p_property.usage = PROPERTY_USAGE_NONE;
			}
		}

		//these definitely only need per pixel
		if (p_property.name.begins_with("anisotropy")) {
			p_property.usage = PROPERTY_USAGE_NONE;
		}

		if (p_property.name.begins_with("clearcoat")) {
			p_property.usage = PROPERTY_USAGE_NONE;
		}

		if (p_property.name.begins_with("normal")) {
			p_property.usage = PROPERTY_USAGE_NONE;
		}

		if (p_property.name.begins_with("bent_normal")) {
			p_property.usage = PROPERTY_USAGE_NONE;
		}

		if (p_property.name.begins_with("backlight")) {
			p_property.usage = PROPERTY_USAGE_NONE;
		}

		if (p_property.name.begins_with("transmittance")) {
			p_property.usage = PROPERTY_USAGE_NONE;
		}
	}
}

void BaseMaterial3D::set_point_size(float p_point_size) {
	point_size = p_point_size;
	_material_set_param(shader_names->point_size, p_point_size);
}

float BaseMaterial3D::get_point_size() const {
	return point_size;
}

void BaseMaterial3D::set_uv1_scale(const Vector3 &p_scale) {
	uv1_scale = p_scale;
	_material_set_param(shader_names->uv1_scale, p_scale);
}

Vector3 BaseMaterial3D::get_uv1_scale() const {
	return uv1_scale;
}

void BaseMaterial3D::set_uv1_offset(const Vector3 &p_offset) {
	uv1_offset = p_offset;
	_material_set_param(shader_names->uv1_offset, p_offset);
}

Vector3 BaseMaterial3D::get_uv1_offset() const {
	return uv1_offset;
}

void BaseMaterial3D::set_uv1_triplanar_blend_sharpness(float p_sharpness) {
	// Negative values or values higher than 150 can result in NaNs, leading to broken rendering.
	uv1_triplanar_sharpness = CLAMP(p_sharpness, 0.0, 150.0);
	_material_set_param(shader_names->uv1_blend_sharpness, uv1_triplanar_sharpness);
}

float BaseMaterial3D::get_uv1_triplanar_blend_sharpness() const {
	return uv1_triplanar_sharpness;
}

void BaseMaterial3D::set_uv2_scale(const Vector3 &p_scale) {
	uv2_scale = p_scale;
	_material_set_param(shader_names->uv2_scale, p_scale);
}

Vector3 BaseMaterial3D::get_uv2_scale() const {
	return uv2_scale;
}

void BaseMaterial3D::set_uv2_offset(const Vector3 &p_offset) {
	uv2_offset = p_offset;
	_material_set_param(shader_names->uv2_offset, p_offset);
}

Vector3 BaseMaterial3D::get_uv2_offset() const {
	return uv2_offset;
}

void BaseMaterial3D::set_uv2_triplanar_blend_sharpness(float p_sharpness) {
	// Negative values or values higher than 150 can result in NaNs, leading to broken rendering.
	uv2_triplanar_sharpness = CLAMP(p_sharpness, 0.0, 150.0);
	_material_set_param(shader_names->uv2_blend_sharpness, uv2_triplanar_sharpness);
}

float BaseMaterial3D::get_uv2_triplanar_blend_sharpness() const {
	return uv2_triplanar_sharpness;
}

void BaseMaterial3D::set_billboard_mode(BillboardMode p_mode) {
	billboard_mode = p_mode;
	_queue_shader_change();
	notify_property_list_changed();
}

BaseMaterial3D::BillboardMode BaseMaterial3D::get_billboard_mode() const {
	return billboard_mode;
}

void BaseMaterial3D::set_particles_anim_h_frames(int p_frames) {
	particles_anim_h_frames = p_frames;
	_material_set_param(shader_names->particles_anim_h_frames, p_frames);
}

int BaseMaterial3D::get_particles_anim_h_frames() const {
	return particles_anim_h_frames;
}

void BaseMaterial3D::set_particles_anim_v_frames(int p_frames) {
	particles_anim_v_frames = p_frames;
	_material_set_param(shader_names->particles_anim_v_frames, p_frames);
}

int BaseMaterial3D::get_particles_anim_v_frames() const {
	return particles_anim_v_frames;
}

void BaseMaterial3D::set_particles_anim_loop(bool p_loop) {
	particles_anim_loop = p_loop;
	_material_set_param(shader_names->particles_anim_loop, particles_anim_loop);
}

bool BaseMaterial3D::get_particles_anim_loop() const {
	return particles_anim_loop;
}

void BaseMaterial3D::set_heightmap_deep_parallax(bool p_enable) {
	deep_parallax = p_enable;
	_queue_shader_change();
	notify_property_list_changed();
}

bool BaseMaterial3D::is_heightmap_deep_parallax_enabled() const {
	return deep_parallax;
}

void BaseMaterial3D::set_heightmap_deep_parallax_min_layers(int p_layer) {
	deep_parallax_min_layers = p_layer;
	_material_set_param(shader_names->heightmap_min_layers, p_layer);
}

int BaseMaterial3D::get_heightmap_deep_parallax_min_layers() const {
	return deep_parallax_min_layers;
}

void BaseMaterial3D::set_heightmap_deep_parallax_max_layers(int p_layer) {
	deep_parallax_max_layers = p_layer;
	_material_set_param(shader_names->heightmap_max_layers, p_layer);
}

int BaseMaterial3D::get_heightmap_deep_parallax_max_layers() const {
	return deep_parallax_max_layers;
}

void BaseMaterial3D::set_heightmap_deep_parallax_flip_tangent(bool p_flip) {
	heightmap_parallax_flip_tangent = p_flip;
	_material_set_param(shader_names->heightmap_flip, Vector2(heightmap_parallax_flip_tangent ? -1 : 1, heightmap_parallax_flip_binormal ? -1 : 1));
}

bool BaseMaterial3D::get_heightmap_deep_parallax_flip_tangent() const {
	return heightmap_parallax_flip_tangent;
}

void BaseMaterial3D::set_heightmap_deep_parallax_flip_binormal(bool p_flip) {
	heightmap_parallax_flip_binormal = p_flip;
	_material_set_param(shader_names->heightmap_flip, Vector2(heightmap_parallax_flip_tangent ? -1 : 1, heightmap_parallax_flip_binormal ? -1 : 1));
}

bool BaseMaterial3D::get_heightmap_deep_parallax_flip_binormal() const {
	return heightmap_parallax_flip_binormal;
}

void BaseMaterial3D::set_grow_enabled(bool p_enable) {
	grow_enabled = p_enable;
	_queue_shader_change();
	notify_property_list_changed();
}

bool BaseMaterial3D::is_grow_enabled() const {
	return grow_enabled;
}

void BaseMaterial3D::set_alpha_scissor_threshold(float p_threshold) {
	alpha_scissor_threshold = p_threshold;
	_material_set_param(shader_names->alpha_scissor_threshold, p_threshold);
}

float BaseMaterial3D::get_alpha_scissor_threshold() const {
	return alpha_scissor_threshold;
}

void BaseMaterial3D::set_alpha_hash_scale(float p_scale) {
	alpha_hash_scale = p_scale;
	_material_set_param(shader_names->alpha_hash_scale, p_scale);
}

float BaseMaterial3D::get_alpha_hash_scale() const {
	return alpha_hash_scale;
}

void BaseMaterial3D::set_alpha_antialiasing_edge(float p_edge) {
	alpha_antialiasing_edge = p_edge;
	_material_set_param(shader_names->alpha_antialiasing_edge, p_edge);
}

float BaseMaterial3D::get_alpha_antialiasing_edge() const {
	return alpha_antialiasing_edge;
}

void BaseMaterial3D::set_grow(float p_grow) {
	grow = p_grow;
	_material_set_param(shader_names->grow, p_grow);
}

float BaseMaterial3D::get_grow() const {
	return grow;
}

static Vector4 _get_texture_mask(BaseMaterial3D::TextureChannel p_channel) {
	static const Vector4 masks[5] = {
		Vector4(1, 0, 0, 0),
		Vector4(0, 1, 0, 0),
		Vector4(0, 0, 1, 0),
		Vector4(0, 0, 0, 1),
		Vector4(0.3333333, 0.3333333, 0.3333333, 0),
	};

	return masks[p_channel];
}

void BaseMaterial3D::set_metallic_texture_channel(TextureChannel p_channel) {
	ERR_FAIL_INDEX(p_channel, 5);
	metallic_texture_channel = p_channel;
	_material_set_param(shader_names->metallic_texture_channel, _get_texture_mask(p_channel));
}

BaseMaterial3D::TextureChannel BaseMaterial3D::get_metallic_texture_channel() const {
	return metallic_texture_channel;
}

void BaseMaterial3D::set_roughness_texture_channel(TextureChannel p_channel) {
	ERR_FAIL_INDEX(p_channel, 5);
	roughness_texture_channel = p_channel;
	_queue_shader_change();
}

BaseMaterial3D::TextureChannel BaseMaterial3D::get_roughness_texture_channel() const {
	return roughness_texture_channel;
}

void BaseMaterial3D::set_ao_texture_channel(TextureChannel p_channel) {
	ERR_FAIL_INDEX(p_channel, 5);
	ao_texture_channel = p_channel;
	_material_set_param(shader_names->ao_texture_channel, _get_texture_mask(p_channel));
}

BaseMaterial3D::TextureChannel BaseMaterial3D::get_ao_texture_channel() const {
	return ao_texture_channel;
}

void BaseMaterial3D::set_hair_mask_texture_channel(TextureChannel p_channel) {
	ERR_FAIL_INDEX(int(p_channel), 5);
	hair_mask_texture_channel = p_channel;
	_material_set_param(shader_names->hair_mask_texture_channel, _get_texture_mask(p_channel));
}

BaseMaterial3D::TextureChannel BaseMaterial3D::get_hair_mask_texture_channel() const {
	return hair_mask_texture_channel;
}

void BaseMaterial3D::set_wetness_porosity(float p_porosity) {
	wetness_porosity = CLAMP(p_porosity, 0.0f, 1.0f);
	_material_set_param(shader_names->wetness_porosity, wetness_porosity);
}

float BaseMaterial3D::get_wetness_porosity() const {
	return wetness_porosity;
}

void BaseMaterial3D::set_wetness_amount(float p_amount) {
	wetness_amount = CLAMP(p_amount, 0.0f, 1.0f);
	_material_set_param(shader_names->wetness_amount, wetness_amount);
}

float BaseMaterial3D::get_wetness_amount() const {
	return wetness_amount;
}

void BaseMaterial3D::set_wetness_from_weather(bool p_enable) {
	if (wetness_from_weather == p_enable) {
		return;
	}
	wetness_from_weather = p_enable;
	_queue_shader_change();
}

bool BaseMaterial3D::is_wetness_from_weather() const {
	return wetness_from_weather;
}

void BaseMaterial3D::set_snow_amount(float p_amount) {
	snow_amount = CLAMP(p_amount, 0.0f, 1.0f);
	_material_set_param(shader_names->snow_amount, snow_amount);
}

float BaseMaterial3D::get_snow_amount() const {
	return snow_amount;
}

void BaseMaterial3D::set_snow_max_slope(float p_degrees) {
	snow_max_slope = CLAMP(p_degrees, 0.0f, 90.0f);
	_material_set_param(shader_names->snow_max_slope, snow_max_slope);
}

float BaseMaterial3D::get_snow_max_slope() const {
	return snow_max_slope;
}

void BaseMaterial3D::set_snow_from_weather(bool p_enable) {
	if (snow_from_weather == p_enable) {
		return;
	}
	snow_from_weather = p_enable;
	_queue_shader_change();
}

bool BaseMaterial3D::is_snow_from_weather() const {
	return snow_from_weather;
}

void BaseMaterial3D::set_ground_deform_patch(bool p_enable) {
	if (ground_deform_patch == p_enable) {
		return;
	}
	ground_deform_patch = p_enable;
	_queue_shader_change();
}

bool BaseMaterial3D::is_ground_deform_patch() const {
	return ground_deform_patch;
}

String BaseMaterial3D::_ground_deform_code(bool p_patch_uniform, bool p_patch) {
	String code = R"(
// Ground deformation (docs/ideas/ground-deform.md): GroundSurface3D's field of dents and rims over its
// ground lattice. On the patch (GroundSurface3D's copy of this material) a vertex moves to the deformed
// surface; on the terrain it is lowered under the patch, so prints below the ground show in both renderers.
global uniform sampler2D ground_deform_map; // dent, rim (m), albedo and roughness factors
global uniform vec4 ground_deform_rect; // x, z, size, texel (m); size 0 = none
global uniform sampler2D ground_deform_height; // the ground, m
global uniform vec4 ground_deform_height_rect;
global uniform vec4 ground_deform_skirt; // depth, margin, fade (m)
)";
	if (p_patch_uniform) {
		code += "uniform bool ground_deform_patch = false;\n";
	} else {
		code += vformat("const bool ground_deform_patch = %s;\n", p_patch ? "true" : "false");
	}
	code += R"(
vec4 ground_deform_sample(vec2 p_xz) {
	vec2 uv = (p_xz - ground_deform_rect.xy) / ground_deform_rect.z;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return vec4(0.0, 0.0, 1.0, 1.0);
	}
	return textureLod(ground_deform_map, uv, 0.0);
}
float ground_deform_surface(vec2 p_xz) {
	vec4 d = ground_deform_sample(p_xz);
	return textureLod(ground_deform_height, (p_xz - ground_deform_height_rect.xy) / ground_deform_height_rect.zw, 0.0).r + d.g - d.r;
}
// The share of the skirt the terrain is lowered by at p_xz: none at the field's edge, all past margin + fade.
float ground_deform_lowered(vec2 p_xz) {
	vec2 e = min(p_xz - ground_deform_rect.xy, ground_deform_rect.xy + ground_deform_rect.z - p_xz);
	return smoothstep(ground_deform_skirt.y, ground_deform_skirt.y + ground_deform_skirt.z, min(e.x, e.y));
}
void ground_deform_vertex(mat4 p_model, inout vec3 r_vertex, inout vec3 r_normal, inout vec3 r_tangent, inout vec3 r_binormal) {
	if (ground_deform_rect.z <= 0.0) {
		return;
	}
	vec3 w = (p_model * vec4(r_vertex, 1.0)).xyz;
	mat4 to_model = inverse(p_model);
	if (ground_deform_patch) {
		// The deformed surface, its normal and tangents by central differences over one field texel. Where
		// the terrain is not lowered (the field's outer margin) the patch tucks 5 cm under it.
		float e = max(ground_deform_rect.w, 1e-3);
		float hx = ground_deform_surface(w.xz + vec2(e, 0.0)) - ground_deform_surface(w.xz - vec2(e, 0.0));
		float hz = ground_deform_surface(w.xz + vec2(0.0, e)) - ground_deform_surface(w.xz - vec2(0.0, e));
		w.y = ground_deform_surface(w.xz) - 0.05 * (1.0 - ground_deform_lowered(w.xz));
		mat3 n_to_model = transpose(mat3(p_model));
		r_normal = normalize(n_to_model * normalize(vec3(-hx, 2.0 * e, -hz)));
		r_tangent = normalize(n_to_model * normalize(vec3(2.0 * e, hx, 0.0)));
		r_binormal = normalize(n_to_model * normalize(vec3(0.0, hz, 2.0 * e)));
	} else {
		w.y -= ground_deform_skirt.x * ground_deform_lowered(w.xz);
	}
	r_vertex = (to_model * vec4(w, 1.0)).xyz;
}
// Albedo and roughness factors of turned ground at a world position (1, 1 off the patch).
vec2 ground_deform_tint(vec3 p_world) {
	if (!ground_deform_patch || ground_deform_rect.z <= 0.0) {
		return vec2(1.0);
	}
	return ground_deform_sample(p_world.xz).ba;
}
)";
	return code;
}

String BaseMaterial3D::get_ground_deform_shader_code() {
	return _ground_deform_code(true, false);
}

void BaseMaterial3D::set_refraction_texture_channel(TextureChannel p_channel) {
	ERR_FAIL_INDEX(p_channel, 5);
	refraction_texture_channel = p_channel;
	_material_set_param(shader_names->refraction_texture_channel, _get_texture_mask(p_channel));
}

BaseMaterial3D::TextureChannel BaseMaterial3D::get_refraction_texture_channel() const {
	return refraction_texture_channel;
}

void BaseMaterial3D::set_z_clip_scale(float p_z_clip_scale) {
	z_clip_scale = p_z_clip_scale;
	_material_set_param(shader_names->z_clip_scale, p_z_clip_scale);
}

float BaseMaterial3D::get_z_clip_scale() const {
	return z_clip_scale;
}

void BaseMaterial3D::set_fov_override(float p_fov_override) {
	fov_override = p_fov_override;
	_material_set_param(shader_names->fov_override, p_fov_override);
}

float BaseMaterial3D::get_fov_override() const {
	return fov_override;
}

Ref<Material> BaseMaterial3D::get_material_for_2d(bool p_shaded, Transparency p_transparency, bool p_double_sided, bool p_billboard, bool p_billboard_y, bool p_msdf, bool p_no_depth, bool p_fixed_size, TextureFilter p_filter, AlphaAntiAliasing p_alpha_antialiasing_mode, bool p_texture_repeat, RID *r_shader_rid) {
	uint64_t key = 0;
	key |= ((int8_t)p_shaded & 0x01) << 0;
	key |= ((int8_t)p_transparency & 0x07) << 1; // Bits 1-3.
	key |= ((int8_t)p_double_sided & 0x01) << 4;
	key |= ((int8_t)p_billboard & 0x01) << 5;
	key |= ((int8_t)p_billboard_y & 0x01) << 6;
	key |= ((int8_t)p_msdf & 0x01) << 7;
	key |= ((int8_t)p_no_depth & 0x01) << 8;
	key |= ((int8_t)p_fixed_size & 0x01) << 9;
	key |= ((int8_t)p_filter & 0x07) << 10; // Bits 10-12.
	key |= ((int8_t)p_alpha_antialiasing_mode & 0x07) << 13; // Bits 13-15.
	key |= ((int8_t)p_texture_repeat & 0x01) << 16;

	if (materials_for_2d.has(key)) {
		if (r_shader_rid) {
			*r_shader_rid = materials_for_2d[key]->get_shader_rid();
		}
		return materials_for_2d[key];
	}

	Ref<StandardMaterial3D> material;
	material.instantiate();

	material->set_shading_mode(p_shaded ? SHADING_MODE_PER_PIXEL : SHADING_MODE_UNSHADED);
	material->set_transparency(p_transparency);
	material->set_cull_mode(p_double_sided ? CULL_DISABLED : CULL_BACK);
	material->set_flag(FLAG_SRGB_VERTEX_COLOR, true);
	material->set_flag(FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	material->set_flag(FLAG_ALBEDO_TEXTURE_MSDF, p_msdf);
	material->set_flag(FLAG_DISABLE_DEPTH_TEST, p_no_depth);
	material->set_flag(FLAG_FIXED_SIZE, p_fixed_size);
	material->set_flag(FLAG_USE_TEXTURE_REPEAT, p_texture_repeat);
	material->set_alpha_antialiasing(p_alpha_antialiasing_mode);
	material->set_texture_filter(p_filter);
	if (p_billboard || p_billboard_y) {
		material->set_flag(FLAG_BILLBOARD_KEEP_SCALE, true);
		material->set_billboard_mode(p_billboard_y ? BILLBOARD_FIXED_Y : BILLBOARD_ENABLED);
	}

	materials_for_2d[key] = material;

	if (r_shader_rid) {
		*r_shader_rid = materials_for_2d[key]->get_shader_rid();
	}

	return materials_for_2d[key];
}

void BaseMaterial3D::set_on_top_of_alpha() {
	set_transparency(TRANSPARENCY_DISABLED);
	set_render_priority(RENDER_PRIORITY_MAX);
	set_flag(FLAG_DISABLE_DEPTH_TEST, true);
}

void BaseMaterial3D::set_proximity_fade_enabled(bool p_enable) {
	proximity_fade_enabled = p_enable;
	_queue_shader_change();
	notify_property_list_changed();
}

bool BaseMaterial3D::is_proximity_fade_enabled() const {
	return proximity_fade_enabled;
}

void BaseMaterial3D::set_proximity_fade_distance(float p_distance) {
	proximity_fade_distance = MAX(p_distance, 0.01);
	_material_set_param(shader_names->proximity_fade_distance, proximity_fade_distance);
}

float BaseMaterial3D::get_proximity_fade_distance() const {
	return proximity_fade_distance;
}

void BaseMaterial3D::set_msdf_pixel_range(float p_range) {
	msdf_pixel_range = p_range;
	_material_set_param(shader_names->msdf_pixel_range, p_range);
}

float BaseMaterial3D::get_msdf_pixel_range() const {
	return msdf_pixel_range;
}

void BaseMaterial3D::set_msdf_outline_size(float p_size) {
	msdf_outline_size = p_size;
	_material_set_param(shader_names->msdf_outline_size, p_size);
}

float BaseMaterial3D::get_msdf_outline_size() const {
	return msdf_outline_size;
}

void BaseMaterial3D::set_distance_fade(DistanceFadeMode p_mode) {
	distance_fade = p_mode;
	_queue_shader_change();
	notify_property_list_changed();
}

BaseMaterial3D::DistanceFadeMode BaseMaterial3D::get_distance_fade() const {
	return distance_fade;
}

void BaseMaterial3D::set_distance_fade_max_distance(float p_distance) {
	distance_fade_max_distance = p_distance;
	_material_set_param(shader_names->distance_fade_max, distance_fade_max_distance);
}

float BaseMaterial3D::get_distance_fade_max_distance() const {
	return distance_fade_max_distance;
}

void BaseMaterial3D::set_distance_fade_min_distance(float p_distance) {
	distance_fade_min_distance = p_distance;
	_material_set_param(shader_names->distance_fade_min, distance_fade_min_distance);
}

float BaseMaterial3D::get_distance_fade_min_distance() const {
	return distance_fade_min_distance;
}

void BaseMaterial3D::set_emission_operator(EmissionOperator p_op) {
	if (emission_op == p_op) {
		return;
	}
	emission_op = p_op;
	_queue_shader_change();
}

BaseMaterial3D::EmissionOperator BaseMaterial3D::get_emission_operator() const {
	return emission_op;
}

RID BaseMaterial3D::get_rid() const {
	const_cast<BaseMaterial3D *>(this)->_update_shader();
	const_cast<BaseMaterial3D *>(this)->_check_material_rid();
	return _get_material();
}

void BaseMaterial3D::_prepare_stencil_effect() {
	const Ref<Material> current_next_pass = get_next_pass();

	if (stencil_mode == STENCIL_MODE_DISABLED || stencil_mode == STENCIL_MODE_CUSTOM) {
		if (current_next_pass.is_valid() && current_next_pass->has_meta("_stencil_owned")) {
			set_next_pass(current_next_pass->get_next_pass());
		}
		return;
	}

	Ref<BaseMaterial3D> stencil_next_pass;

	if (current_next_pass.is_null() || !current_next_pass->has_meta("_stencil_owned")) {
		stencil_next_pass = Ref<BaseMaterial3D>(memnew(StandardMaterial3D));
		stencil_next_pass->set_meta("_stencil_owned", true);
		stencil_next_pass->set_next_pass(current_next_pass);
		set_next_pass(stencil_next_pass);
	} else {
		stencil_next_pass = current_next_pass;
	}

	switch (stencil_mode) {
		case STENCIL_MODE_DISABLED:
			break;
		case STENCIL_MODE_OUTLINE:
			set_stencil_flags(STENCIL_FLAG_WRITE);
			set_stencil_compare(STENCIL_COMPARE_ALWAYS);
			stencil_next_pass->set_render_priority(get_render_priority() + 1);
			stencil_next_pass->set_shading_mode(SHADING_MODE_UNSHADED);
			stencil_next_pass->set_transparency(TRANSPARENCY_ALPHA);
			stencil_next_pass->set_flag(FLAG_DISABLE_DEPTH_TEST, false);
			stencil_next_pass->set_grow_enabled(true);
			stencil_next_pass->set_grow(stencil_effect_outline_thickness);
			stencil_next_pass->set_albedo(stencil_effect_color);
			stencil_next_pass->set_stencil_mode(STENCIL_MODE_CUSTOM);
			stencil_next_pass->set_stencil_flags(STENCIL_FLAG_READ);
			stencil_next_pass->set_stencil_compare(STENCIL_COMPARE_NOT_EQUAL);
			stencil_next_pass->set_stencil_reference(stencil_reference);
			break;
		case STENCIL_MODE_XRAY:
			set_stencil_flags(STENCIL_FLAG_WRITE);
			set_stencil_compare(STENCIL_COMPARE_ALWAYS);
			stencil_next_pass->set_render_priority(get_render_priority() + 1);
			stencil_next_pass->set_shading_mode(SHADING_MODE_UNSHADED);
			stencil_next_pass->set_transparency(TRANSPARENCY_ALPHA);
			stencil_next_pass->set_flag(FLAG_DISABLE_DEPTH_TEST, true);
			stencil_next_pass->set_grow_enabled(false);
			stencil_next_pass->set_grow(0);
			stencil_next_pass->set_albedo(stencil_effect_color);
			stencil_next_pass->set_stencil_mode(STENCIL_MODE_CUSTOM);
			stencil_next_pass->set_stencil_flags(STENCIL_FLAG_READ);
			stencil_next_pass->set_stencil_compare(STENCIL_COMPARE_NOT_EQUAL);
			stencil_next_pass->set_stencil_reference(stencil_reference);
			break;
		case STENCIL_MODE_CUSTOM:
			break;
		case STENCIL_MODE_MAX:
			break;
	}
}

Ref<BaseMaterial3D> BaseMaterial3D::_get_stencil_next_pass() const {
	const Ref<Material> current_next_pass = get_next_pass();
	Ref<BaseMaterial3D> stencil_next_pass;

	if (current_next_pass.is_valid() && current_next_pass->has_meta("_stencil_owned")) {
		stencil_next_pass = current_next_pass;
	}

	return stencil_next_pass;
}

void BaseMaterial3D::set_stencil_mode(StencilMode p_stencil_mode) {
	if (stencil_mode == p_stencil_mode) {
		return;
	}

	if (p_stencil_mode == StencilMode::STENCIL_MODE_OUTLINE || p_stencil_mode == StencilMode::STENCIL_MODE_XRAY) {
		ERR_FAIL_COND_EDMSG(get_render_priority() >= RENDER_PRIORITY_MAX,
				vformat("Cannot use stencil mode Outline or Xray, when render priority is RENDER_PRIORITY_MAX(%d).", RENDER_PRIORITY_MAX));
	}

	stencil_mode = p_stencil_mode;
	_prepare_stencil_effect();
	_queue_shader_change();
	notify_property_list_changed();
}

BaseMaterial3D::StencilMode BaseMaterial3D::get_stencil_mode() const {
	return stencil_mode;
}

void BaseMaterial3D::set_stencil_flags(int p_stencil_flags) {
	if (stencil_flags == p_stencil_flags) {
		return;
	}

	// If enabling read while already writing, switch to read only.
	if ((p_stencil_flags & STENCIL_FLAG_READ) && (stencil_flags & (STENCIL_FLAG_WRITE | STENCIL_FLAG_WRITE_DEPTH_FAIL))) {
		p_stencil_flags = p_stencil_flags & STENCIL_FLAG_READ;
	}

	// If enabling write while already reading, switch to write or write_depth_fail.
	if ((p_stencil_flags & (STENCIL_FLAG_WRITE | STENCIL_FLAG_WRITE_DEPTH_FAIL)) && (stencil_flags & STENCIL_FLAG_READ)) {
		p_stencil_flags = p_stencil_flags & (STENCIL_FLAG_WRITE | STENCIL_FLAG_WRITE_DEPTH_FAIL);
	}

	// If enabling read+write while already doing neither, only allow read.
	if ((p_stencil_flags & STENCIL_FLAG_READ) && (p_stencil_flags & (STENCIL_FLAG_WRITE | STENCIL_FLAG_WRITE_DEPTH_FAIL))) {
		p_stencil_flags = p_stencil_flags & STENCIL_FLAG_READ;
	}

	stencil_flags = p_stencil_flags;
	_queue_shader_change();
}

int BaseMaterial3D::get_stencil_flags() const {
	return stencil_flags;
}

void BaseMaterial3D::set_stencil_compare(BaseMaterial3D::StencilCompare p_op) {
	if (stencil_compare == p_op) {
		return;
	}

	stencil_compare = p_op;
	_queue_shader_change();
}

BaseMaterial3D::StencilCompare BaseMaterial3D::get_stencil_compare() const {
	return stencil_compare;
}

void BaseMaterial3D::set_stencil_reference(int p_reference) {
	if (stencil_reference == p_reference) {
		return;
	}

	stencil_reference = p_reference;
	_queue_shader_change();

	Ref<BaseMaterial3D> stencil_next_pass = _get_stencil_next_pass();
	if (stencil_next_pass.is_valid()) {
		stencil_next_pass->set_stencil_reference(p_reference);
	}
}

int BaseMaterial3D::get_stencil_reference() const {
	return stencil_reference;
}

void BaseMaterial3D::set_stencil_effect_color(const Color &p_color) {
	if (stencil_effect_color == p_color) {
		return;
	}

	stencil_effect_color = p_color;

	Ref<BaseMaterial3D> stencil_next_pass = _get_stencil_next_pass();
	if (stencil_next_pass.is_valid()) {
		stencil_next_pass->set_albedo(p_color);
	}
}

Color BaseMaterial3D::get_stencil_effect_color() const {
	return stencil_effect_color;
}

void BaseMaterial3D::set_stencil_effect_outline_thickness(float p_outline_thickness) {
	if (stencil_effect_outline_thickness == p_outline_thickness) {
		return;
	}

	stencil_effect_outline_thickness = p_outline_thickness;

	Ref<BaseMaterial3D> stencil_next_pass = _get_stencil_next_pass();
	if (stencil_next_pass.is_valid()) {
		stencil_next_pass->set_grow(p_outline_thickness);
	}
}

float BaseMaterial3D::get_stencil_effect_outline_thickness() const {
	return stencil_effect_outline_thickness;
}

RID BaseMaterial3D::get_shader_rid() const {
	const_cast<BaseMaterial3D *>(this)->_update_shader();
	return shader_rid;
}

Shader::Mode BaseMaterial3D::get_shader_mode() const {
	return Shader::MODE_SPATIAL;
}

void BaseMaterial3D::_bind_methods() {
	ClassDB::bind_static_method("BaseMaterial3D", D_METHOD("get_wetness_shader_code"), &BaseMaterial3D::get_wetness_shader_code);
	ClassDB::bind_static_method("BaseMaterial3D", D_METHOD("get_snow_shader_code"), &BaseMaterial3D::get_snow_shader_code);
	static_assert(sizeof(MaterialKey) == 16, "MaterialKey should be 16 bytes");

	ClassDB::bind_method(D_METHOD("set_albedo", "albedo"), &BaseMaterial3D::set_albedo);
	ClassDB::bind_method(D_METHOD("get_albedo"), &BaseMaterial3D::get_albedo);

	ClassDB::bind_method(D_METHOD("set_transparency", "transparency"), &BaseMaterial3D::set_transparency);
	ClassDB::bind_method(D_METHOD("get_transparency"), &BaseMaterial3D::get_transparency);

	ClassDB::bind_method(D_METHOD("set_alpha_antialiasing", "alpha_aa"), &BaseMaterial3D::set_alpha_antialiasing);
	ClassDB::bind_method(D_METHOD("get_alpha_antialiasing"), &BaseMaterial3D::get_alpha_antialiasing);

	ClassDB::bind_method(D_METHOD("set_alpha_antialiasing_edge", "edge"), &BaseMaterial3D::set_alpha_antialiasing_edge);
	ClassDB::bind_method(D_METHOD("get_alpha_antialiasing_edge"), &BaseMaterial3D::get_alpha_antialiasing_edge);

	ClassDB::bind_method(D_METHOD("set_shading_mode", "shading_mode"), &BaseMaterial3D::set_shading_mode);
	ClassDB::bind_method(D_METHOD("get_shading_mode"), &BaseMaterial3D::get_shading_mode);

	ClassDB::bind_method(D_METHOD("set_specular", "specular"), &BaseMaterial3D::set_specular);
	ClassDB::bind_method(D_METHOD("get_specular"), &BaseMaterial3D::get_specular);

	ClassDB::bind_method(D_METHOD("set_metallic", "metallic"), &BaseMaterial3D::set_metallic);
	ClassDB::bind_method(D_METHOD("get_metallic"), &BaseMaterial3D::get_metallic);

	ClassDB::bind_method(D_METHOD("set_roughness", "roughness"), &BaseMaterial3D::set_roughness);
	ClassDB::bind_method(D_METHOD("get_roughness"), &BaseMaterial3D::get_roughness);

	ClassDB::bind_method(D_METHOD("set_emission", "emission"), &BaseMaterial3D::set_emission);
	ClassDB::bind_method(D_METHOD("get_emission"), &BaseMaterial3D::get_emission);

	ClassDB::bind_method(D_METHOD("set_emission_energy_multiplier", "emission_energy_multiplier"), &BaseMaterial3D::set_emission_energy_multiplier);
	ClassDB::bind_method(D_METHOD("get_emission_energy_multiplier"), &BaseMaterial3D::get_emission_energy_multiplier);

	ClassDB::bind_method(D_METHOD("set_emission_intensity", "emission_energy_multiplier"), &BaseMaterial3D::set_emission_intensity);
	ClassDB::bind_method(D_METHOD("get_emission_intensity"), &BaseMaterial3D::get_emission_intensity);

	ClassDB::bind_method(D_METHOD("set_emission_indirect_energy", "emission_indirect_energy"), &BaseMaterial3D::set_emission_indirect_energy);
	ClassDB::bind_method(D_METHOD("get_emission_indirect_energy"), &BaseMaterial3D::get_emission_indirect_energy);

	ClassDB::bind_method(D_METHOD("set_normal_scale", "normal_scale"), &BaseMaterial3D::set_normal_scale);
	ClassDB::bind_method(D_METHOD("get_normal_scale"), &BaseMaterial3D::get_normal_scale);

	ClassDB::bind_method(D_METHOD("set_rim", "rim"), &BaseMaterial3D::set_rim);
	ClassDB::bind_method(D_METHOD("get_rim"), &BaseMaterial3D::get_rim);

	ClassDB::bind_method(D_METHOD("set_rim_tint", "rim_tint"), &BaseMaterial3D::set_rim_tint);
	ClassDB::bind_method(D_METHOD("get_rim_tint"), &BaseMaterial3D::get_rim_tint);

	ClassDB::bind_method(D_METHOD("set_clearcoat", "clearcoat"), &BaseMaterial3D::set_clearcoat);
	ClassDB::bind_method(D_METHOD("get_clearcoat"), &BaseMaterial3D::get_clearcoat);

	ClassDB::bind_method(D_METHOD("set_clearcoat_roughness", "clearcoat_roughness"), &BaseMaterial3D::set_clearcoat_roughness);
	ClassDB::bind_method(D_METHOD("get_clearcoat_roughness"), &BaseMaterial3D::get_clearcoat_roughness);

	ClassDB::bind_method(D_METHOD("set_anisotropy", "anisotropy"), &BaseMaterial3D::set_anisotropy);
	ClassDB::bind_method(D_METHOD("get_anisotropy"), &BaseMaterial3D::get_anisotropy);

	ClassDB::bind_method(D_METHOD("set_heightmap_scale", "heightmap_scale"), &BaseMaterial3D::set_heightmap_scale);
	ClassDB::bind_method(D_METHOD("get_heightmap_scale"), &BaseMaterial3D::get_heightmap_scale);

	ClassDB::bind_method(D_METHOD("set_subsurface_scattering_strength", "strength"), &BaseMaterial3D::set_subsurface_scattering_strength);
	ClassDB::bind_method(D_METHOD("get_subsurface_scattering_strength"), &BaseMaterial3D::get_subsurface_scattering_strength);

	ClassDB::bind_method(D_METHOD("set_transmittance_color", "color"), &BaseMaterial3D::set_transmittance_color);
	ClassDB::bind_method(D_METHOD("get_transmittance_color"), &BaseMaterial3D::get_transmittance_color);

	ClassDB::bind_method(D_METHOD("set_transmittance_depth", "depth"), &BaseMaterial3D::set_transmittance_depth);
	ClassDB::bind_method(D_METHOD("get_transmittance_depth"), &BaseMaterial3D::get_transmittance_depth);

	ClassDB::bind_method(D_METHOD("set_transmittance_boost", "boost"), &BaseMaterial3D::set_transmittance_boost);
	ClassDB::bind_method(D_METHOD("get_transmittance_boost"), &BaseMaterial3D::get_transmittance_boost);

	ClassDB::bind_method(D_METHOD("set_backlight", "backlight"), &BaseMaterial3D::set_backlight);
	ClassDB::bind_method(D_METHOD("get_backlight"), &BaseMaterial3D::get_backlight);

	ClassDB::bind_method(D_METHOD("set_refraction", "refraction"), &BaseMaterial3D::set_refraction);
	ClassDB::bind_method(D_METHOD("get_refraction"), &BaseMaterial3D::get_refraction);

	ClassDB::bind_method(D_METHOD("set_transmission", "transmission"), &BaseMaterial3D::set_transmission);
	ClassDB::bind_method(D_METHOD("get_transmission"), &BaseMaterial3D::get_transmission);

	ClassDB::bind_method(D_METHOD("set_transmission_thin_walled", "thin_walled"), &BaseMaterial3D::set_transmission_thin_walled);
	ClassDB::bind_method(D_METHOD("is_transmission_thin_walled"), &BaseMaterial3D::is_transmission_thin_walled);

	ClassDB::bind_method(D_METHOD("set_transmission_ior", "ior"), &BaseMaterial3D::set_transmission_ior);
	ClassDB::bind_method(D_METHOD("get_transmission_ior"), &BaseMaterial3D::get_transmission_ior);

	ClassDB::bind_method(D_METHOD("set_transmission_attenuation_color", "color"), &BaseMaterial3D::set_transmission_attenuation_color);
	ClassDB::bind_method(D_METHOD("get_transmission_attenuation_color"), &BaseMaterial3D::get_transmission_attenuation_color);

	ClassDB::bind_method(D_METHOD("set_transmission_attenuation_distance", "distance"), &BaseMaterial3D::set_transmission_attenuation_distance);
	ClassDB::bind_method(D_METHOD("get_transmission_attenuation_distance"), &BaseMaterial3D::get_transmission_attenuation_distance);
	ClassDB::bind_method(D_METHOD("set_transmission_thickness", "thickness"), &BaseMaterial3D::set_transmission_thickness);
	ClassDB::bind_method(D_METHOD("get_transmission_thickness"), &BaseMaterial3D::get_transmission_thickness);

	ClassDB::bind_method(D_METHOD("set_transmission_scatter_strength", "strength"), &BaseMaterial3D::set_transmission_scatter_strength);
	ClassDB::bind_method(D_METHOD("get_transmission_scatter_strength"), &BaseMaterial3D::get_transmission_scatter_strength);

	ClassDB::bind_method(D_METHOD("set_transmission_scatter_color", "color"), &BaseMaterial3D::set_transmission_scatter_color);
	ClassDB::bind_method(D_METHOD("get_transmission_scatter_color"), &BaseMaterial3D::get_transmission_scatter_color);

	ClassDB::bind_method(D_METHOD("set_transmission_scatter_anisotropy", "anisotropy"), &BaseMaterial3D::set_transmission_scatter_anisotropy);
	ClassDB::bind_method(D_METHOD("get_transmission_scatter_anisotropy"), &BaseMaterial3D::get_transmission_scatter_anisotropy);

	ClassDB::bind_method(D_METHOD("set_hair_color_mode", "mode"), &BaseMaterial3D::set_hair_color_mode);
	ClassDB::bind_method(D_METHOD("get_hair_color_mode"), &BaseMaterial3D::get_hair_color_mode);
	ClassDB::bind_method(D_METHOD("set_hair_melanin", "melanin"), &BaseMaterial3D::set_hair_melanin);
	ClassDB::bind_method(D_METHOD("get_hair_melanin"), &BaseMaterial3D::get_hair_melanin);
	ClassDB::bind_method(D_METHOD("set_hair_melanin_redness", "redness"), &BaseMaterial3D::set_hair_melanin_redness);
	ClassDB::bind_method(D_METHOD("get_hair_melanin_redness"), &BaseMaterial3D::get_hair_melanin_redness);
	ClassDB::bind_method(D_METHOD("set_hair_roughness", "roughness"), &BaseMaterial3D::set_hair_roughness);
	ClassDB::bind_method(D_METHOD("get_hair_roughness"), &BaseMaterial3D::get_hair_roughness);
	ClassDB::bind_method(D_METHOD("set_hair_radial_roughness", "roughness"), &BaseMaterial3D::set_hair_radial_roughness);
	ClassDB::bind_method(D_METHOD("get_hair_radial_roughness"), &BaseMaterial3D::get_hair_radial_roughness);
	ClassDB::bind_method(D_METHOD("set_hair_cuticle_tilt", "degrees"), &BaseMaterial3D::set_hair_cuticle_tilt);
	ClassDB::bind_method(D_METHOD("get_hair_cuticle_tilt"), &BaseMaterial3D::get_hair_cuticle_tilt);
	ClassDB::bind_method(D_METHOD("set_hair_direction", "direction"), &BaseMaterial3D::set_hair_direction);
	ClassDB::bind_method(D_METHOD("get_hair_direction"), &BaseMaterial3D::get_hair_direction);
	ClassDB::bind_method(D_METHOD("set_hair_crown", "crown"), &BaseMaterial3D::set_hair_crown);
	ClassDB::bind_method(D_METHOD("get_hair_crown"), &BaseMaterial3D::get_hair_crown);
	ClassDB::bind_method(D_METHOD("set_hair_cards", "cards"), &BaseMaterial3D::set_hair_cards);
	ClassDB::bind_method(D_METHOD("is_hair_cards"), &BaseMaterial3D::is_hair_cards);
	ClassDB::bind_method(D_METHOD("set_hair_mask_texture_channel", "channel"), &BaseMaterial3D::set_hair_mask_texture_channel);
	ClassDB::bind_method(D_METHOD("get_hair_mask_texture_channel"), &BaseMaterial3D::get_hair_mask_texture_channel);

	ClassDB::bind_method(D_METHOD("set_wetness_porosity", "porosity"), &BaseMaterial3D::set_wetness_porosity);
	ClassDB::bind_method(D_METHOD("get_wetness_porosity"), &BaseMaterial3D::get_wetness_porosity);
	ClassDB::bind_method(D_METHOD("set_wetness_amount", "amount"), &BaseMaterial3D::set_wetness_amount);
	ClassDB::bind_method(D_METHOD("get_wetness_amount"), &BaseMaterial3D::get_wetness_amount);
	ClassDB::bind_method(D_METHOD("set_wetness_from_weather", "enable"), &BaseMaterial3D::set_wetness_from_weather);
	ClassDB::bind_method(D_METHOD("is_wetness_from_weather"), &BaseMaterial3D::is_wetness_from_weather);
	ClassDB::bind_method(D_METHOD("set_snow_amount", "amount"), &BaseMaterial3D::set_snow_amount);
	ClassDB::bind_method(D_METHOD("get_snow_amount"), &BaseMaterial3D::get_snow_amount);
	ClassDB::bind_method(D_METHOD("set_snow_max_slope", "degrees"), &BaseMaterial3D::set_snow_max_slope);
	ClassDB::bind_method(D_METHOD("get_snow_max_slope"), &BaseMaterial3D::get_snow_max_slope);
	ClassDB::bind_method(D_METHOD("set_snow_from_weather", "enable"), &BaseMaterial3D::set_snow_from_weather);
	ClassDB::bind_method(D_METHOD("set_ground_deform_patch", "enable"), &BaseMaterial3D::set_ground_deform_patch);
	ClassDB::bind_method(D_METHOD("is_ground_deform_patch"), &BaseMaterial3D::is_ground_deform_patch);
	ClassDB::bind_static_method("BaseMaterial3D", D_METHOD("get_ground_deform_shader_code"), &BaseMaterial3D::get_ground_deform_shader_code);
	ClassDB::bind_method(D_METHOD("is_snow_from_weather"), &BaseMaterial3D::is_snow_from_weather);

	ClassDB::bind_method(D_METHOD("set_point_size", "point_size"), &BaseMaterial3D::set_point_size);
	ClassDB::bind_method(D_METHOD("get_point_size"), &BaseMaterial3D::get_point_size);

	ClassDB::bind_method(D_METHOD("set_detail_uv", "detail_uv"), &BaseMaterial3D::set_detail_uv);
	ClassDB::bind_method(D_METHOD("get_detail_uv"), &BaseMaterial3D::get_detail_uv);

	ClassDB::bind_method(D_METHOD("set_blend_mode", "blend_mode"), &BaseMaterial3D::set_blend_mode);
	ClassDB::bind_method(D_METHOD("get_blend_mode"), &BaseMaterial3D::get_blend_mode);

	ClassDB::bind_method(D_METHOD("set_depth_draw_mode", "depth_draw_mode"), &BaseMaterial3D::set_depth_draw_mode);
	ClassDB::bind_method(D_METHOD("get_depth_draw_mode"), &BaseMaterial3D::get_depth_draw_mode);

	ClassDB::bind_method(D_METHOD("set_depth_test", "depth_test"), &BaseMaterial3D::set_depth_test);
	ClassDB::bind_method(D_METHOD("get_depth_test"), &BaseMaterial3D::get_depth_test);

	ClassDB::bind_method(D_METHOD("set_cull_mode", "cull_mode"), &BaseMaterial3D::set_cull_mode);
	ClassDB::bind_method(D_METHOD("get_cull_mode"), &BaseMaterial3D::get_cull_mode);

	ClassDB::bind_method(D_METHOD("set_diffuse_mode", "diffuse_mode"), &BaseMaterial3D::set_diffuse_mode);
	ClassDB::bind_method(D_METHOD("get_diffuse_mode"), &BaseMaterial3D::get_diffuse_mode);

	ClassDB::bind_method(D_METHOD("set_specular_mode", "specular_mode"), &BaseMaterial3D::set_specular_mode);
	ClassDB::bind_method(D_METHOD("get_specular_mode"), &BaseMaterial3D::get_specular_mode);

	ClassDB::bind_method(D_METHOD("set_flag", "flag", "enable"), &BaseMaterial3D::set_flag);
	ClassDB::bind_method(D_METHOD("get_flag", "flag"), &BaseMaterial3D::get_flag);

	ClassDB::bind_method(D_METHOD("set_texture_filter", "mode"), &BaseMaterial3D::set_texture_filter);
	ClassDB::bind_method(D_METHOD("get_texture_filter"), &BaseMaterial3D::get_texture_filter);

	ClassDB::bind_method(D_METHOD("set_feature", "feature", "enable"), &BaseMaterial3D::set_feature);
	ClassDB::bind_method(D_METHOD("get_feature", "feature"), &BaseMaterial3D::get_feature);

	ClassDB::bind_method(D_METHOD("set_texture", "param", "texture"), &BaseMaterial3D::set_texture);
	ClassDB::bind_method(D_METHOD("get_texture", "param"), &BaseMaterial3D::get_texture);

	ClassDB::bind_method(D_METHOD("set_detail_blend_mode", "detail_blend_mode"), &BaseMaterial3D::set_detail_blend_mode);
	ClassDB::bind_method(D_METHOD("get_detail_blend_mode"), &BaseMaterial3D::get_detail_blend_mode);

	ClassDB::bind_method(D_METHOD("set_uv1_scale", "scale"), &BaseMaterial3D::set_uv1_scale);
	ClassDB::bind_method(D_METHOD("get_uv1_scale"), &BaseMaterial3D::get_uv1_scale);

	ClassDB::bind_method(D_METHOD("set_uv1_offset", "offset"), &BaseMaterial3D::set_uv1_offset);
	ClassDB::bind_method(D_METHOD("get_uv1_offset"), &BaseMaterial3D::get_uv1_offset);

	ClassDB::bind_method(D_METHOD("set_uv1_triplanar_blend_sharpness", "sharpness"), &BaseMaterial3D::set_uv1_triplanar_blend_sharpness);
	ClassDB::bind_method(D_METHOD("get_uv1_triplanar_blend_sharpness"), &BaseMaterial3D::get_uv1_triplanar_blend_sharpness);

	ClassDB::bind_method(D_METHOD("set_uv2_scale", "scale"), &BaseMaterial3D::set_uv2_scale);
	ClassDB::bind_method(D_METHOD("get_uv2_scale"), &BaseMaterial3D::get_uv2_scale);

	ClassDB::bind_method(D_METHOD("set_uv2_offset", "offset"), &BaseMaterial3D::set_uv2_offset);
	ClassDB::bind_method(D_METHOD("get_uv2_offset"), &BaseMaterial3D::get_uv2_offset);

	ClassDB::bind_method(D_METHOD("set_uv2_triplanar_blend_sharpness", "sharpness"), &BaseMaterial3D::set_uv2_triplanar_blend_sharpness);
	ClassDB::bind_method(D_METHOD("get_uv2_triplanar_blend_sharpness"), &BaseMaterial3D::get_uv2_triplanar_blend_sharpness);

	ClassDB::bind_method(D_METHOD("set_billboard_mode", "mode"), &BaseMaterial3D::set_billboard_mode);
	ClassDB::bind_method(D_METHOD("get_billboard_mode"), &BaseMaterial3D::get_billboard_mode);

	ClassDB::bind_method(D_METHOD("set_particles_anim_h_frames", "frames"), &BaseMaterial3D::set_particles_anim_h_frames);
	ClassDB::bind_method(D_METHOD("get_particles_anim_h_frames"), &BaseMaterial3D::get_particles_anim_h_frames);

	ClassDB::bind_method(D_METHOD("set_particles_anim_v_frames", "frames"), &BaseMaterial3D::set_particles_anim_v_frames);
	ClassDB::bind_method(D_METHOD("get_particles_anim_v_frames"), &BaseMaterial3D::get_particles_anim_v_frames);

	ClassDB::bind_method(D_METHOD("set_particles_anim_loop", "loop"), &BaseMaterial3D::set_particles_anim_loop);
	ClassDB::bind_method(D_METHOD("get_particles_anim_loop"), &BaseMaterial3D::get_particles_anim_loop);

	ClassDB::bind_method(D_METHOD("set_heightmap_deep_parallax", "enable"), &BaseMaterial3D::set_heightmap_deep_parallax);
	ClassDB::bind_method(D_METHOD("is_heightmap_deep_parallax_enabled"), &BaseMaterial3D::is_heightmap_deep_parallax_enabled);

	ClassDB::bind_method(D_METHOD("set_heightmap_deep_parallax_min_layers", "layer"), &BaseMaterial3D::set_heightmap_deep_parallax_min_layers);
	ClassDB::bind_method(D_METHOD("get_heightmap_deep_parallax_min_layers"), &BaseMaterial3D::get_heightmap_deep_parallax_min_layers);

	ClassDB::bind_method(D_METHOD("set_heightmap_deep_parallax_max_layers", "layer"), &BaseMaterial3D::set_heightmap_deep_parallax_max_layers);
	ClassDB::bind_method(D_METHOD("get_heightmap_deep_parallax_max_layers"), &BaseMaterial3D::get_heightmap_deep_parallax_max_layers);

	ClassDB::bind_method(D_METHOD("set_heightmap_deep_parallax_flip_tangent", "flip"), &BaseMaterial3D::set_heightmap_deep_parallax_flip_tangent);
	ClassDB::bind_method(D_METHOD("get_heightmap_deep_parallax_flip_tangent"), &BaseMaterial3D::get_heightmap_deep_parallax_flip_tangent);

	ClassDB::bind_method(D_METHOD("set_heightmap_deep_parallax_flip_binormal", "flip"), &BaseMaterial3D::set_heightmap_deep_parallax_flip_binormal);
	ClassDB::bind_method(D_METHOD("get_heightmap_deep_parallax_flip_binormal"), &BaseMaterial3D::get_heightmap_deep_parallax_flip_binormal);

	ClassDB::bind_method(D_METHOD("set_grow", "amount"), &BaseMaterial3D::set_grow);
	ClassDB::bind_method(D_METHOD("get_grow"), &BaseMaterial3D::get_grow);

	ClassDB::bind_method(D_METHOD("set_emission_operator", "operator"), &BaseMaterial3D::set_emission_operator);
	ClassDB::bind_method(D_METHOD("get_emission_operator"), &BaseMaterial3D::get_emission_operator);

	ClassDB::bind_method(D_METHOD("set_ao_light_affect", "amount"), &BaseMaterial3D::set_ao_light_affect);
	ClassDB::bind_method(D_METHOD("get_ao_light_affect"), &BaseMaterial3D::get_ao_light_affect);

	ClassDB::bind_method(D_METHOD("set_alpha_scissor_threshold", "threshold"), &BaseMaterial3D::set_alpha_scissor_threshold);
	ClassDB::bind_method(D_METHOD("get_alpha_scissor_threshold"), &BaseMaterial3D::get_alpha_scissor_threshold);

	ClassDB::bind_method(D_METHOD("set_alpha_hash_scale", "threshold"), &BaseMaterial3D::set_alpha_hash_scale);
	ClassDB::bind_method(D_METHOD("get_alpha_hash_scale"), &BaseMaterial3D::get_alpha_hash_scale);

	ClassDB::bind_method(D_METHOD("set_grow_enabled", "enable"), &BaseMaterial3D::set_grow_enabled);
	ClassDB::bind_method(D_METHOD("is_grow_enabled"), &BaseMaterial3D::is_grow_enabled);

	ClassDB::bind_method(D_METHOD("set_metallic_texture_channel", "channel"), &BaseMaterial3D::set_metallic_texture_channel);
	ClassDB::bind_method(D_METHOD("get_metallic_texture_channel"), &BaseMaterial3D::get_metallic_texture_channel);

	ClassDB::bind_method(D_METHOD("set_roughness_texture_channel", "channel"), &BaseMaterial3D::set_roughness_texture_channel);
	ClassDB::bind_method(D_METHOD("get_roughness_texture_channel"), &BaseMaterial3D::get_roughness_texture_channel);

	ClassDB::bind_method(D_METHOD("set_ao_texture_channel", "channel"), &BaseMaterial3D::set_ao_texture_channel);
	ClassDB::bind_method(D_METHOD("get_ao_texture_channel"), &BaseMaterial3D::get_ao_texture_channel);

	ClassDB::bind_method(D_METHOD("set_refraction_texture_channel", "channel"), &BaseMaterial3D::set_refraction_texture_channel);
	ClassDB::bind_method(D_METHOD("get_refraction_texture_channel"), &BaseMaterial3D::get_refraction_texture_channel);

	ClassDB::bind_method(D_METHOD("set_proximity_fade_enabled", "enabled"), &BaseMaterial3D::set_proximity_fade_enabled);
	ClassDB::bind_method(D_METHOD("is_proximity_fade_enabled"), &BaseMaterial3D::is_proximity_fade_enabled);

	ClassDB::bind_method(D_METHOD("set_proximity_fade_distance", "distance"), &BaseMaterial3D::set_proximity_fade_distance);
	ClassDB::bind_method(D_METHOD("get_proximity_fade_distance"), &BaseMaterial3D::get_proximity_fade_distance);

	ClassDB::bind_method(D_METHOD("set_msdf_pixel_range", "range"), &BaseMaterial3D::set_msdf_pixel_range);
	ClassDB::bind_method(D_METHOD("get_msdf_pixel_range"), &BaseMaterial3D::get_msdf_pixel_range);

	ClassDB::bind_method(D_METHOD("set_msdf_outline_size", "size"), &BaseMaterial3D::set_msdf_outline_size);
	ClassDB::bind_method(D_METHOD("get_msdf_outline_size"), &BaseMaterial3D::get_msdf_outline_size);

	ClassDB::bind_method(D_METHOD("set_distance_fade", "mode"), &BaseMaterial3D::set_distance_fade);
	ClassDB::bind_method(D_METHOD("get_distance_fade"), &BaseMaterial3D::get_distance_fade);

	ClassDB::bind_method(D_METHOD("set_distance_fade_max_distance", "distance"), &BaseMaterial3D::set_distance_fade_max_distance);
	ClassDB::bind_method(D_METHOD("get_distance_fade_max_distance"), &BaseMaterial3D::get_distance_fade_max_distance);

	ClassDB::bind_method(D_METHOD("set_distance_fade_min_distance", "distance"), &BaseMaterial3D::set_distance_fade_min_distance);
	ClassDB::bind_method(D_METHOD("get_distance_fade_min_distance"), &BaseMaterial3D::get_distance_fade_min_distance);

	ClassDB::bind_method(D_METHOD("set_z_clip_scale", "scale"), &BaseMaterial3D::set_z_clip_scale);
	ClassDB::bind_method(D_METHOD("get_z_clip_scale"), &BaseMaterial3D::get_z_clip_scale);

	ClassDB::bind_method(D_METHOD("set_fov_override", "scale"), &BaseMaterial3D::set_fov_override);
	ClassDB::bind_method(D_METHOD("get_fov_override"), &BaseMaterial3D::get_fov_override);

	ClassDB::bind_method(D_METHOD("set_stencil_mode", "stencil_mode"), &BaseMaterial3D::set_stencil_mode);
	ClassDB::bind_method(D_METHOD("get_stencil_mode"), &BaseMaterial3D::get_stencil_mode);

	ClassDB::bind_method(D_METHOD("set_stencil_flags", "stencil_flags"), &BaseMaterial3D::set_stencil_flags);
	ClassDB::bind_method(D_METHOD("get_stencil_flags"), &BaseMaterial3D::get_stencil_flags);

	ClassDB::bind_method(D_METHOD("set_stencil_compare", "stencil_compare"), &BaseMaterial3D::set_stencil_compare);
	ClassDB::bind_method(D_METHOD("get_stencil_compare"), &BaseMaterial3D::get_stencil_compare);

	ClassDB::bind_method(D_METHOD("set_stencil_reference", "stencil_reference"), &BaseMaterial3D::set_stencil_reference);
	ClassDB::bind_method(D_METHOD("get_stencil_reference"), &BaseMaterial3D::get_stencil_reference);

	ClassDB::bind_method(D_METHOD("set_stencil_effect_color", "stencil_color"), &BaseMaterial3D::set_stencil_effect_color);
	ClassDB::bind_method(D_METHOD("get_stencil_effect_color"), &BaseMaterial3D::get_stencil_effect_color);

	ClassDB::bind_method(D_METHOD("set_stencil_effect_outline_thickness", "stencil_outline_thickness"), &BaseMaterial3D::set_stencil_effect_outline_thickness);
	ClassDB::bind_method(D_METHOD("get_stencil_effect_outline_thickness"), &BaseMaterial3D::get_stencil_effect_outline_thickness);

	ADD_GROUP("Transparency", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "transparency", PROPERTY_HINT_ENUM, "Disabled,Alpha,Alpha Scissor,Alpha Hash,Depth Pre-Pass"), "set_transparency", "get_transparency");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "alpha_scissor_threshold", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_alpha_scissor_threshold", "get_alpha_scissor_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "alpha_hash_scale", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_alpha_hash_scale", "get_alpha_hash_scale");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "alpha_antialiasing_mode", PROPERTY_HINT_ENUM, "Disabled,Alpha Edge Blend,Alpha Edge Clip"), "set_alpha_antialiasing", "get_alpha_antialiasing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "alpha_antialiasing_edge", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_alpha_antialiasing_edge", "get_alpha_antialiasing_edge");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "blend_mode", PROPERTY_HINT_ENUM, "Mix,Add,Subtract,Multiply,Premultiplied Alpha,Weighted OIT"), "set_blend_mode", "get_blend_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cull_mode", PROPERTY_HINT_ENUM, "Back,Front,Disabled"), "set_cull_mode", "get_cull_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "depth_draw_mode", PROPERTY_HINT_ENUM, "Opaque Only,Always,Never"), "set_depth_draw_mode", "get_depth_draw_mode");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "no_depth_test"), "set_flag", "get_flag", FLAG_DISABLE_DEPTH_TEST);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "depth_test", PROPERTY_HINT_ENUM, "Default,Inverted"), "set_depth_test", "get_depth_test");

	ADD_GROUP("Shading", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shading_mode", PROPERTY_HINT_ENUM, "Unshaded,Per-Pixel,Per-Vertex"), "set_shading_mode", "get_shading_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "diffuse_mode", PROPERTY_HINT_ENUM, "Burley,Lambert,Lambert Wrap,Toon"), "set_diffuse_mode", "get_diffuse_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "specular_mode", PROPERTY_HINT_ENUM, "SchlickGGX,Toon,Disabled"), "set_specular_mode", "get_specular_mode");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "disable_ambient_light"), "set_flag", "get_flag", FLAG_DISABLE_AMBIENT_LIGHT);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "disable_fog"), "set_flag", "get_flag", FLAG_DISABLE_FOG);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "disable_specular_occlusion"), "set_flag", "get_flag", FLAG_DISABLE_SPECULAR_OCCLUSION);

	ADD_GROUP("Vertex Color", "vertex_color");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "vertex_color_use_as_albedo"), "set_flag", "get_flag", FLAG_ALBEDO_FROM_VERTEX_COLOR);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "vertex_color_is_srgb"), "set_flag", "get_flag", FLAG_SRGB_VERTEX_COLOR);

	ADD_GROUP("Albedo", "albedo_");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "albedo_color"), "set_albedo", "get_albedo");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "albedo_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_ALBEDO);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "albedo_texture_force_srgb"), "set_flag", "get_flag", FLAG_ALBEDO_TEXTURE_FORCE_SRGB);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "albedo_texture_msdf"), "set_flag", "get_flag", FLAG_ALBEDO_TEXTURE_MSDF);

	ADD_GROUP("ORM", "orm_");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "orm_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_ORM);

	ADD_GROUP("Metallic", "metallic_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "metallic", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_metallic", "get_metallic");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "metallic_specular", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_specular", "get_specular");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "metallic_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_METALLIC);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "metallic_texture_channel", PROPERTY_HINT_ENUM, "Red,Green,Blue,Alpha,Gray"), "set_metallic_texture_channel", "get_metallic_texture_channel");

	ADD_GROUP("Roughness", "roughness_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_roughness", "get_roughness");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "roughness_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_ROUGHNESS);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "roughness_texture_channel", PROPERTY_HINT_ENUM, "Red,Green,Blue,Alpha,Gray"), "set_roughness_texture_channel", "get_roughness_texture_channel");

	ADD_GROUP("Emission", "emission_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "emission_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_EMISSION);
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "emission", PROPERTY_HINT_COLOR_NO_ALPHA), "set_emission", "get_emission");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "emission_energy_multiplier", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_emission_energy_multiplier", "get_emission_energy_multiplier");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "emission_intensity", PROPERTY_HINT_RANGE, "0,100000.0,0.01,or_greater,suffix:nt"), "set_emission_intensity", "get_emission_intensity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "emission_indirect_energy", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_emission_indirect_energy", "get_emission_indirect_energy");

	ADD_PROPERTY(PropertyInfo(Variant::INT, "emission_operator", PROPERTY_HINT_ENUM, "Add,Multiply"), "set_emission_operator", "get_emission_operator");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "emission_on_uv2"), "set_flag", "get_flag", FLAG_EMISSION_ON_UV2);
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "emission_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_EMISSION);

	ADD_GROUP("Normal Map", "normal_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "normal_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_NORMAL_MAPPING);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "normal_scale", PROPERTY_HINT_RANGE, "-16,16,0.01"), "set_normal_scale", "get_normal_scale");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "normal_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_NORMAL);

	ADD_GROUP("Bent Normal Map", "bent_normal_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "bent_normal_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_BENT_NORMAL_MAPPING);
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "bent_normal_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_BENT_NORMAL);

	ADD_GROUP("Rim", "rim_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "rim_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_RIM);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rim", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_rim", "get_rim");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rim_tint", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_rim_tint", "get_rim_tint");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "rim_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_RIM);

	ADD_GROUP("Clearcoat", "clearcoat_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "clearcoat_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_CLEARCOAT);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clearcoat", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_clearcoat", "get_clearcoat");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clearcoat_roughness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_clearcoat_roughness", "get_clearcoat_roughness");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "clearcoat_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_CLEARCOAT);

	ADD_GROUP("Anisotropy", "anisotropy_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "anisotropy_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_ANISOTROPY);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "anisotropy", PROPERTY_HINT_RANGE, "-1,1,0.01"), "set_anisotropy", "get_anisotropy");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "anisotropy_flowmap", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_FLOWMAP);

	ADD_GROUP("Ambient Occlusion", "ao_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "ao_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_AMBIENT_OCCLUSION);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ao_light_affect", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_ao_light_affect", "get_ao_light_affect");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "ao_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_AMBIENT_OCCLUSION);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "ao_on_uv2"), "set_flag", "get_flag", FLAG_AO_ON_UV2);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ao_texture_channel", PROPERTY_HINT_ENUM, "Red,Green,Blue,Alpha,Gray"), "set_ao_texture_channel", "get_ao_texture_channel");

	ADD_GROUP("Height", "heightmap_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "heightmap_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_HEIGHT_MAPPING);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "heightmap_scale", PROPERTY_HINT_RANGE, "-16,16,0.001"), "set_heightmap_scale", "get_heightmap_scale");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "heightmap_deep_parallax"), "set_heightmap_deep_parallax", "is_heightmap_deep_parallax_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "heightmap_min_layers", PROPERTY_HINT_RANGE, "1,64,1"), "set_heightmap_deep_parallax_min_layers", "get_heightmap_deep_parallax_min_layers");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "heightmap_max_layers", PROPERTY_HINT_RANGE, "1,64,1"), "set_heightmap_deep_parallax_max_layers", "get_heightmap_deep_parallax_max_layers");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "heightmap_flip_tangent"), "set_heightmap_deep_parallax_flip_tangent", "get_heightmap_deep_parallax_flip_tangent");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "heightmap_flip_binormal"), "set_heightmap_deep_parallax_flip_binormal", "get_heightmap_deep_parallax_flip_binormal");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "heightmap_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_HEIGHTMAP);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "heightmap_flip_texture"), "set_flag", "get_flag", FLAG_INVERT_HEIGHTMAP);

	ADD_GROUP("Subsurf Scatter", "subsurf_scatter_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "subsurf_scatter_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_SUBSURFACE_SCATTERING);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "subsurf_scatter_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_subsurface_scattering_strength", "get_subsurface_scattering_strength");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "subsurf_scatter_skin_mode"), "set_flag", "get_flag", FLAG_SUBSURFACE_MODE_SKIN);
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "subsurf_scatter_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_SUBSURFACE_SCATTERING);

	ADD_SUBGROUP("Transmittance", "subsurf_scatter_transmittance_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "subsurf_scatter_transmittance_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_SUBSURFACE_TRANSMITTANCE);
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "subsurf_scatter_transmittance_color"), "set_transmittance_color", "get_transmittance_color");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "subsurf_scatter_transmittance_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_SUBSURFACE_TRANSMITTANCE);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "subsurf_scatter_transmittance_depth", PROPERTY_HINT_RANGE, "0.001,8,0.001,or_greater"), "set_transmittance_depth", "get_transmittance_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "subsurf_scatter_transmittance_boost", PROPERTY_HINT_RANGE, "0.00,1.0,0.01"), "set_transmittance_boost", "get_transmittance_boost");

	ADD_GROUP("Back Lighting", "backlight_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "backlight_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_BACKLIGHT);
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "backlight", PROPERTY_HINT_COLOR_NO_ALPHA), "set_backlight", "get_backlight");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "backlight_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_BACKLIGHT);

	ADD_GROUP("Refraction", "refraction_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "refraction_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_REFRACTION);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "refraction_scale", PROPERTY_HINT_RANGE, "-1,1,0.01"), "set_refraction", "get_refraction");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "refraction_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_REFRACTION);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "refraction_texture_channel", PROPERTY_HINT_ENUM, "Red,Green,Blue,Alpha,Gray"), "set_refraction_texture_channel", "get_refraction_texture_channel");

	ADD_GROUP("Transmission", "transmission_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "transmission_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_TRANSMISSION);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "transmission_amount", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_transmission", "get_transmission");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "transmission_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_TRANSMISSION);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "transmission_thin_walled"), "set_transmission_thin_walled", "is_transmission_thin_walled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "transmission_ior", PROPERTY_HINT_RANGE, "1,3,0.01"), "set_transmission_ior", "get_transmission_ior");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "transmission_attenuation_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_transmission_attenuation_color", "get_transmission_attenuation_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "transmission_attenuation_distance", PROPERTY_HINT_RANGE, "0,1000,0.01,or_greater,suffix:m"), "set_transmission_attenuation_distance", "get_transmission_attenuation_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "transmission_thickness", PROPERTY_HINT_RANGE, "0,10,0.001,or_greater,suffix:m"), "set_transmission_thickness", "get_transmission_thickness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "transmission_scatter_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_transmission_scatter_strength", "get_transmission_scatter_strength");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "transmission_scatter_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_transmission_scatter_color", "get_transmission_scatter_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "transmission_scatter_anisotropy", PROPERTY_HINT_RANGE, "-0.99,0.99,0.01"), "set_transmission_scatter_anisotropy", "get_transmission_scatter_anisotropy");

	ADD_GROUP("Hair", "hair_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "hair_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_HAIR);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "hair_color_mode", PROPERTY_HINT_ENUM, "Albedo,Melanin"), "set_hair_color_mode", "get_hair_color_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "hair_melanin", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_hair_melanin", "get_hair_melanin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "hair_melanin_redness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_hair_melanin_redness", "get_hair_melanin_redness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "hair_roughness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_hair_roughness", "get_hair_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "hair_radial_roughness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_hair_radial_roughness", "get_hair_radial_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "hair_cuticle_tilt", PROPERTY_HINT_RANGE, "-10,10,0.1,degrees"), "set_hair_cuticle_tilt", "get_hair_cuticle_tilt");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "hair_direction", PROPERTY_HINT_ENUM, "Crown,Tangent,Bitangent,Strands"), "set_hair_direction", "get_hair_direction");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "hair_crown", PROPERTY_HINT_NONE, "suffix:m"), "set_hair_crown", "get_hair_crown");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "hair_cards"), "set_hair_cards", "is_hair_cards");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "hair_mask_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_HAIR_MASK);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "hair_mask_texture_channel", PROPERTY_HINT_ENUM, "Red,Green,Blue,Alpha,Gray"), "set_hair_mask_texture_channel", "get_hair_mask_texture_channel");

	ADD_GROUP("Wetness", "wetness_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "wetness_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_WETNESS);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wetness_porosity", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_wetness_porosity", "get_wetness_porosity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wetness_amount", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_wetness_amount", "get_wetness_amount");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wetness_from_weather"), "set_wetness_from_weather", "is_wetness_from_weather");

	ADD_GROUP("Snow", "snow_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "snow_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_SNOW);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "snow_amount", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_snow_amount", "get_snow_amount");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "snow_max_slope", PROPERTY_HINT_RANGE, "0,90,0.1,degrees"), "set_snow_max_slope", "get_snow_max_slope");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "snow_from_weather"), "set_snow_from_weather", "is_snow_from_weather");

	ADD_GROUP("Ground Deform", "ground_deform_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "ground_deform_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_GROUND_DEFORM);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ground_deform_patch"), "set_ground_deform_patch", "is_ground_deform_patch");

	ADD_GROUP("Detail", "detail_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "detail_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_DETAIL);
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "detail_mask", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_DETAIL_MASK);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "detail_blend_mode", PROPERTY_HINT_ENUM, "Mix,Add,Subtract,Multiply"), "set_detail_blend_mode", "get_detail_blend_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "detail_uv_layer", PROPERTY_HINT_ENUM, "UV1,UV2"), "set_detail_uv", "get_detail_uv");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "detail_albedo", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_DETAIL_ALBEDO);
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "detail_normal", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_DETAIL_NORMAL);

	ADD_GROUP("UV1", "uv1_");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "uv1_scale", PROPERTY_HINT_LINK), "set_uv1_scale", "get_uv1_scale");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "uv1_offset"), "set_uv1_offset", "get_uv1_offset");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "uv1_triplanar"), "set_flag", "get_flag", FLAG_UV1_USE_TRIPLANAR);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "uv1_triplanar_sharpness", PROPERTY_HINT_EXP_EASING), "set_uv1_triplanar_blend_sharpness", "get_uv1_triplanar_blend_sharpness");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "uv1_world_triplanar"), "set_flag", "get_flag", FLAG_UV1_USE_WORLD_TRIPLANAR);

	ADD_GROUP("UV2", "uv2_");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "uv2_scale", PROPERTY_HINT_LINK), "set_uv2_scale", "get_uv2_scale");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "uv2_offset"), "set_uv2_offset", "get_uv2_offset");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "uv2_triplanar"), "set_flag", "get_flag", FLAG_UV2_USE_TRIPLANAR);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "uv2_triplanar_sharpness", PROPERTY_HINT_EXP_EASING), "set_uv2_triplanar_blend_sharpness", "get_uv2_triplanar_blend_sharpness");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "uv2_world_triplanar"), "set_flag", "get_flag", FLAG_UV2_USE_WORLD_TRIPLANAR);

	ADD_GROUP("Sampling", "texture_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "texture_filter", PROPERTY_HINT_ENUM, "Nearest,Linear,Nearest Mipmap,Linear Mipmap,Nearest Mipmap Anisotropic,Linear Mipmap Anisotropic"), "set_texture_filter", "get_texture_filter");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "texture_repeat"), "set_flag", "get_flag", FLAG_USE_TEXTURE_REPEAT);

	ADD_GROUP("Shadows", "");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "disable_receive_shadows"), "set_flag", "get_flag", FLAG_DONT_RECEIVE_SHADOWS);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "shadow_to_opacity"), "set_flag", "get_flag", FLAG_USE_SHADOW_TO_OPACITY);

	ADD_GROUP("Billboard", "billboard_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "billboard_mode", PROPERTY_HINT_ENUM, "Disabled,Enabled,Y-Billboard,Particle Billboard"), "set_billboard_mode", "get_billboard_mode");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "billboard_keep_scale"), "set_flag", "get_flag", FLAG_BILLBOARD_KEEP_SCALE);

	ADD_GROUP("Particles Anim", "particles_anim_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "particles_anim_h_frames", PROPERTY_HINT_RANGE, "1,128,1"), "set_particles_anim_h_frames", "get_particles_anim_h_frames");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "particles_anim_v_frames", PROPERTY_HINT_RANGE, "1,128,1"), "set_particles_anim_v_frames", "get_particles_anim_v_frames");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "particles_anim_loop"), "set_particles_anim_loop", "get_particles_anim_loop");

	ADD_GROUP("Grow", "grow_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "grow", PROPERTY_HINT_GROUP_ENABLE), "set_grow_enabled", "is_grow_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "grow_amount", PROPERTY_HINT_RANGE, "-16,16,0.001,suffix:m"), "set_grow", "get_grow");

	ADD_GROUP("Transform", "");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "fixed_size"), "set_flag", "get_flag", FLAG_FIXED_SIZE);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "use_point_size"), "set_flag", "get_flag", FLAG_USE_POINT_SIZE);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "point_size", PROPERTY_HINT_RANGE, "0.1,128,0.1,suffix:px"), "set_point_size", "get_point_size");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "use_particle_trails"), "set_flag", "get_flag", FLAG_PARTICLE_TRAILS_MODE);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "use_z_clip_scale"), "set_flag", "get_flag", FLAG_USE_Z_CLIP_SCALE);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "z_clip_scale", PROPERTY_HINT_RANGE, "0.01,1.0,0.01"), "set_z_clip_scale", "get_z_clip_scale");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "use_fov_override"), "set_flag", "get_flag", FLAG_USE_FOV_OVERRIDE);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fov_override", PROPERTY_HINT_RANGE, "1,179,0.1,degrees"), "set_fov_override", "get_fov_override");

	ADD_GROUP("Proximity Fade", "proximity_fade_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "proximity_fade_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_proximity_fade_enabled", "is_proximity_fade_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "proximity_fade_distance", PROPERTY_HINT_RANGE, "0.01,4096,0.01,suffix:m"), "set_proximity_fade_distance", "get_proximity_fade_distance");

	ADD_GROUP("MSDF", "msdf_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "msdf_pixel_range", PROPERTY_HINT_RANGE, "1,100,1"), "set_msdf_pixel_range", "get_msdf_pixel_range");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "msdf_outline_size", PROPERTY_HINT_RANGE, "0,250,1"), "set_msdf_outline_size", "get_msdf_outline_size");

	ADD_GROUP("Distance Fade", "distance_fade_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "distance_fade_mode", PROPERTY_HINT_ENUM, "Disabled,PixelAlpha,PixelDither,ObjectDither"), "set_distance_fade", "get_distance_fade");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "distance_fade_min_distance", PROPERTY_HINT_RANGE, "0,4096,0.01,suffix:m"), "set_distance_fade_min_distance", "get_distance_fade_min_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "distance_fade_max_distance", PROPERTY_HINT_RANGE, "0,4096,0.01,suffix:m"), "set_distance_fade_max_distance", "get_distance_fade_max_distance");

	ADD_GROUP("Stencil", "stencil_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "stencil_mode", PROPERTY_HINT_ENUM, "Disabled,Outline,X-Ray,Custom"), "set_stencil_mode", "get_stencil_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "stencil_flags", PROPERTY_HINT_FLAGS, "Read,Write,Write Depth Fail"), "set_stencil_flags", "get_stencil_flags");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "stencil_compare", PROPERTY_HINT_ENUM, "Always,Less,Equal,Less Or Equal,Greater,Not Equal,Greater Or Equal"), "set_stencil_compare", "get_stencil_compare");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "stencil_reference", PROPERTY_HINT_RANGE, "0,255,1"), "set_stencil_reference", "get_stencil_reference");

	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "stencil_color", PROPERTY_HINT_NONE), "set_stencil_effect_color", "get_stencil_effect_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stencil_outline_thickness", PROPERTY_HINT_RANGE, "0,1,0.001,or_greater,suffix:m"), "set_stencil_effect_outline_thickness", "get_stencil_effect_outline_thickness");

	BIND_ENUM_CONSTANT(TEXTURE_ALBEDO);
	BIND_ENUM_CONSTANT(TEXTURE_METALLIC);
	BIND_ENUM_CONSTANT(TEXTURE_ROUGHNESS);
	BIND_ENUM_CONSTANT(TEXTURE_EMISSION);
	BIND_ENUM_CONSTANT(TEXTURE_NORMAL);
	BIND_ENUM_CONSTANT(TEXTURE_BENT_NORMAL);
	BIND_ENUM_CONSTANT(TEXTURE_TRANSMISSION);
	BIND_ENUM_CONSTANT(TEXTURE_HAIR_MASK);
	BIND_ENUM_CONSTANT(TEXTURE_RIM);
	BIND_ENUM_CONSTANT(TEXTURE_CLEARCOAT);
	BIND_ENUM_CONSTANT(TEXTURE_FLOWMAP);
	BIND_ENUM_CONSTANT(TEXTURE_AMBIENT_OCCLUSION);
	BIND_ENUM_CONSTANT(TEXTURE_HEIGHTMAP);
	BIND_ENUM_CONSTANT(TEXTURE_SUBSURFACE_SCATTERING);
	BIND_ENUM_CONSTANT(TEXTURE_SUBSURFACE_TRANSMITTANCE);
	BIND_ENUM_CONSTANT(TEXTURE_BACKLIGHT);
	BIND_ENUM_CONSTANT(TEXTURE_REFRACTION);
	BIND_ENUM_CONSTANT(TEXTURE_DETAIL_MASK);
	BIND_ENUM_CONSTANT(TEXTURE_DETAIL_ALBEDO);
	BIND_ENUM_CONSTANT(TEXTURE_DETAIL_NORMAL);
	BIND_ENUM_CONSTANT(TEXTURE_ORM);
	BIND_ENUM_CONSTANT(TEXTURE_MAX);

	BIND_ENUM_CONSTANT(TEXTURE_FILTER_NEAREST);
	BIND_ENUM_CONSTANT(TEXTURE_FILTER_LINEAR);
	BIND_ENUM_CONSTANT(TEXTURE_FILTER_NEAREST_WITH_MIPMAPS);
	BIND_ENUM_CONSTANT(TEXTURE_FILTER_LINEAR_WITH_MIPMAPS);
	BIND_ENUM_CONSTANT(TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC);
	BIND_ENUM_CONSTANT(TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC);
	BIND_ENUM_CONSTANT(TEXTURE_FILTER_MAX);

	BIND_ENUM_CONSTANT(DETAIL_UV_1);
	BIND_ENUM_CONSTANT(DETAIL_UV_2);

	BIND_ENUM_CONSTANT(TRANSPARENCY_DISABLED);
	BIND_ENUM_CONSTANT(TRANSPARENCY_ALPHA);
	BIND_ENUM_CONSTANT(TRANSPARENCY_ALPHA_SCISSOR);
	BIND_ENUM_CONSTANT(TRANSPARENCY_ALPHA_HASH);
	BIND_ENUM_CONSTANT(TRANSPARENCY_ALPHA_DEPTH_PRE_PASS);
	BIND_ENUM_CONSTANT(TRANSPARENCY_MAX);

	BIND_ENUM_CONSTANT(SHADING_MODE_UNSHADED);
	BIND_ENUM_CONSTANT(SHADING_MODE_PER_PIXEL);
	BIND_ENUM_CONSTANT(SHADING_MODE_PER_VERTEX);
	BIND_ENUM_CONSTANT(SHADING_MODE_MAX);

	BIND_ENUM_CONSTANT(FEATURE_EMISSION);
	BIND_ENUM_CONSTANT(FEATURE_NORMAL_MAPPING);
	BIND_ENUM_CONSTANT(FEATURE_RIM);
	BIND_ENUM_CONSTANT(FEATURE_CLEARCOAT);
	BIND_ENUM_CONSTANT(FEATURE_ANISOTROPY);
	BIND_ENUM_CONSTANT(FEATURE_AMBIENT_OCCLUSION);
	BIND_ENUM_CONSTANT(FEATURE_HEIGHT_MAPPING);
	BIND_ENUM_CONSTANT(FEATURE_SUBSURFACE_SCATTERING);
	BIND_ENUM_CONSTANT(FEATURE_SUBSURFACE_TRANSMITTANCE);
	BIND_ENUM_CONSTANT(FEATURE_BACKLIGHT);
	BIND_ENUM_CONSTANT(FEATURE_REFRACTION);
	BIND_ENUM_CONSTANT(FEATURE_DETAIL);
	BIND_ENUM_CONSTANT(FEATURE_BENT_NORMAL_MAPPING);
	BIND_ENUM_CONSTANT(FEATURE_TRANSMISSION);
	BIND_ENUM_CONSTANT(FEATURE_HAIR);
	BIND_ENUM_CONSTANT(FEATURE_WETNESS);
	BIND_ENUM_CONSTANT(FEATURE_SNOW);
	BIND_ENUM_CONSTANT(FEATURE_GROUND_DEFORM);
	BIND_ENUM_CONSTANT(FEATURE_MAX);

	BIND_ENUM_CONSTANT(BLEND_MODE_MIX);
	BIND_ENUM_CONSTANT(BLEND_MODE_ADD);
	BIND_ENUM_CONSTANT(BLEND_MODE_SUB);
	BIND_ENUM_CONSTANT(BLEND_MODE_MUL);
	BIND_ENUM_CONSTANT(BLEND_MODE_PREMULT_ALPHA);
	BIND_ENUM_CONSTANT(BLEND_MODE_OIT);

	BIND_ENUM_CONSTANT(ALPHA_ANTIALIASING_OFF);
	BIND_ENUM_CONSTANT(ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE);
	BIND_ENUM_CONSTANT(ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE_AND_TO_ONE);

	BIND_ENUM_CONSTANT(DEPTH_DRAW_OPAQUE_ONLY);
	BIND_ENUM_CONSTANT(DEPTH_DRAW_ALWAYS);
	BIND_ENUM_CONSTANT(DEPTH_DRAW_DISABLED);

	BIND_ENUM_CONSTANT(DEPTH_TEST_DEFAULT);
	BIND_ENUM_CONSTANT(DEPTH_TEST_INVERTED);

	BIND_ENUM_CONSTANT(CULL_BACK);
	BIND_ENUM_CONSTANT(CULL_FRONT);
	BIND_ENUM_CONSTANT(CULL_DISABLED);

	BIND_ENUM_CONSTANT(FLAG_DISABLE_DEPTH_TEST);
	BIND_ENUM_CONSTANT(FLAG_ALBEDO_FROM_VERTEX_COLOR);
	BIND_ENUM_CONSTANT(FLAG_SRGB_VERTEX_COLOR);
	BIND_ENUM_CONSTANT(FLAG_USE_POINT_SIZE);
	BIND_ENUM_CONSTANT(FLAG_FIXED_SIZE);
	BIND_ENUM_CONSTANT(FLAG_BILLBOARD_KEEP_SCALE);
	BIND_ENUM_CONSTANT(FLAG_UV1_USE_TRIPLANAR);
	BIND_ENUM_CONSTANT(FLAG_UV2_USE_TRIPLANAR);
	BIND_ENUM_CONSTANT(FLAG_UV1_USE_WORLD_TRIPLANAR);
	BIND_ENUM_CONSTANT(FLAG_UV2_USE_WORLD_TRIPLANAR);
	BIND_ENUM_CONSTANT(FLAG_AO_ON_UV2);
	BIND_ENUM_CONSTANT(FLAG_EMISSION_ON_UV2);
	BIND_ENUM_CONSTANT(FLAG_ALBEDO_TEXTURE_FORCE_SRGB);
	BIND_ENUM_CONSTANT(FLAG_DONT_RECEIVE_SHADOWS);
	BIND_ENUM_CONSTANT(FLAG_DISABLE_AMBIENT_LIGHT);
	BIND_ENUM_CONSTANT(FLAG_USE_SHADOW_TO_OPACITY);
	BIND_ENUM_CONSTANT(FLAG_USE_TEXTURE_REPEAT);
	BIND_ENUM_CONSTANT(FLAG_INVERT_HEIGHTMAP);
	BIND_ENUM_CONSTANT(FLAG_SUBSURFACE_MODE_SKIN);
	BIND_ENUM_CONSTANT(FLAG_PARTICLE_TRAILS_MODE);
	BIND_ENUM_CONSTANT(FLAG_ALBEDO_TEXTURE_MSDF);
	BIND_ENUM_CONSTANT(FLAG_DISABLE_FOG);
	BIND_ENUM_CONSTANT(FLAG_DISABLE_SPECULAR_OCCLUSION);
	BIND_ENUM_CONSTANT(FLAG_USE_Z_CLIP_SCALE);
	BIND_ENUM_CONSTANT(FLAG_USE_FOV_OVERRIDE);
	BIND_ENUM_CONSTANT(FLAG_MAX);

	BIND_ENUM_CONSTANT(DIFFUSE_BURLEY);
	BIND_ENUM_CONSTANT(DIFFUSE_LAMBERT);
	BIND_ENUM_CONSTANT(DIFFUSE_LAMBERT_WRAP);
	BIND_ENUM_CONSTANT(DIFFUSE_TOON);

	BIND_ENUM_CONSTANT(SPECULAR_SCHLICK_GGX);
	BIND_ENUM_CONSTANT(SPECULAR_TOON);
	BIND_ENUM_CONSTANT(SPECULAR_DISABLED);

	BIND_ENUM_CONSTANT(BILLBOARD_DISABLED);
	BIND_ENUM_CONSTANT(BILLBOARD_ENABLED);
	BIND_ENUM_CONSTANT(BILLBOARD_FIXED_Y);
	BIND_ENUM_CONSTANT(BILLBOARD_PARTICLES);

	BIND_ENUM_CONSTANT(TEXTURE_CHANNEL_RED);
	BIND_ENUM_CONSTANT(TEXTURE_CHANNEL_GREEN);
	BIND_ENUM_CONSTANT(TEXTURE_CHANNEL_BLUE);
	BIND_ENUM_CONSTANT(TEXTURE_CHANNEL_ALPHA);
	BIND_ENUM_CONSTANT(TEXTURE_CHANNEL_GRAYSCALE);

	BIND_ENUM_CONSTANT(HAIR_COLOR_ALBEDO);
	BIND_ENUM_CONSTANT(HAIR_COLOR_MELANIN);
	BIND_ENUM_CONSTANT(HAIR_COLOR_MAX);

	BIND_ENUM_CONSTANT(HAIR_DIRECTION_CROWN);
	BIND_ENUM_CONSTANT(HAIR_DIRECTION_TANGENT);
	BIND_ENUM_CONSTANT(HAIR_DIRECTION_BITANGENT);
	BIND_ENUM_CONSTANT(HAIR_DIRECTION_STRANDS);
	BIND_ENUM_CONSTANT(HAIR_DIRECTION_MAX);

	BIND_ENUM_CONSTANT(EMISSION_OP_ADD);
	BIND_ENUM_CONSTANT(EMISSION_OP_MULTIPLY);

	BIND_ENUM_CONSTANT(DISTANCE_FADE_DISABLED);
	BIND_ENUM_CONSTANT(DISTANCE_FADE_PIXEL_ALPHA);
	BIND_ENUM_CONSTANT(DISTANCE_FADE_PIXEL_DITHER);
	BIND_ENUM_CONSTANT(DISTANCE_FADE_OBJECT_DITHER);

	BIND_ENUM_CONSTANT(STENCIL_MODE_DISABLED);
	BIND_ENUM_CONSTANT(STENCIL_MODE_OUTLINE);
	BIND_ENUM_CONSTANT(STENCIL_MODE_XRAY);
	BIND_ENUM_CONSTANT(STENCIL_MODE_CUSTOM);

	BIND_ENUM_CONSTANT(STENCIL_FLAG_READ);
	BIND_ENUM_CONSTANT(STENCIL_FLAG_WRITE);
	BIND_ENUM_CONSTANT(STENCIL_FLAG_WRITE_DEPTH_FAIL);

	BIND_ENUM_CONSTANT(STENCIL_COMPARE_ALWAYS);
	BIND_ENUM_CONSTANT(STENCIL_COMPARE_LESS);
	BIND_ENUM_CONSTANT(STENCIL_COMPARE_EQUAL);
	BIND_ENUM_CONSTANT(STENCIL_COMPARE_LESS_OR_EQUAL);
	BIND_ENUM_CONSTANT(STENCIL_COMPARE_GREATER);
	BIND_ENUM_CONSTANT(STENCIL_COMPARE_NOT_EQUAL);
	BIND_ENUM_CONSTANT(STENCIL_COMPARE_GREATER_OR_EQUAL);
}

BaseMaterial3D::BaseMaterial3D(bool p_orm) :
		element(this) {
	orm = p_orm;
	// Initialize to the same values as the shader
	set_albedo(Color(1.0, 1.0, 1.0, 1.0));
	set_specular(0.5);
	set_roughness(1.0);
	set_metallic(0.0);
	set_emission(Color(0, 0, 0));
	set_emission_energy_multiplier(1.0);
	set_emission_indirect_energy(1.0);
	set_normal_scale(1);
	set_rim(1.0);
	set_rim_tint(0.5);
	set_clearcoat(1);
	set_clearcoat_roughness(0.5);
	set_anisotropy(0);
	set_heightmap_scale(5.0);
	set_subsurface_scattering_strength(0);
	set_backlight(Color(0, 0, 0));
	set_transmittance_color(Color(1, 1, 1, 1));
	set_transmittance_depth(0.1);
	set_transmittance_boost(0.0);
	set_refraction(0.05);
	set_transmission(1.0);
	set_transmission_ior(1.5);
	set_transmission_attenuation_color(Color(1, 1, 1));
	set_transmission_attenuation_distance(0.0);
	set_transmission_thickness(0.0);
	set_transmission_scatter_strength(0.0);
	set_transmission_scatter_color(Color(1, 1, 1));
	set_transmission_scatter_anisotropy(0.0);
	set_hair_melanin(0.5);
	set_hair_melanin_redness(0.0);
	set_hair_roughness(0.3);
	set_hair_radial_roughness(0.3);
	set_hair_cuticle_tilt(2.0);
	set_hair_crown(Vector3(0, 1.7, 0));
	set_hair_mask_texture_channel(TEXTURE_CHANNEL_RED);
	set_wetness_porosity(0.5);
	set_wetness_amount(1.0);
	set_snow_amount(1.0);
	set_snow_max_slope(60.0);
	set_point_size(1);
	set_uv1_offset(Vector3(0, 0, 0));
	set_uv1_scale(Vector3(1, 1, 1));
	set_uv1_triplanar_blend_sharpness(1);
	set_uv2_offset(Vector3(0, 0, 0));
	set_uv2_scale(Vector3(1, 1, 1));
	set_uv2_triplanar_blend_sharpness(1);
	set_billboard_mode(BILLBOARD_DISABLED);
	set_particles_anim_h_frames(1);
	set_particles_anim_v_frames(1);
	set_particles_anim_loop(false);

	set_transparency(TRANSPARENCY_DISABLED);
	set_alpha_antialiasing(ALPHA_ANTIALIASING_OFF);
	// Alpha scissor threshold of 0.5 matches the glTF specification and Label3D default.
	// <https://www.khronos.org/registry/glTF/specs/2.0/glTF-2.0.html#_material_alphacutoff>
	set_alpha_scissor_threshold(0.5);
	set_alpha_hash_scale(1.0);
	set_alpha_antialiasing_edge(0.3);

	set_proximity_fade_distance(1);
	set_distance_fade_min_distance(0);
	set_distance_fade_max_distance(10);

	set_ao_light_affect(0.0);

	set_metallic_texture_channel(TEXTURE_CHANNEL_RED);
	set_roughness_texture_channel(TEXTURE_CHANNEL_RED);
	set_ao_texture_channel(TEXTURE_CHANNEL_RED);
	set_refraction_texture_channel(TEXTURE_CHANNEL_RED);

	set_grow(0.0);

	set_msdf_pixel_range(4.0);
	set_msdf_outline_size(0.0);

	set_heightmap_deep_parallax_min_layers(8);
	set_heightmap_deep_parallax_max_layers(32);
	set_heightmap_deep_parallax_flip_tangent(false); //also sets binormal

	set_z_clip_scale(1.0);
	set_fov_override(75.0);

	set_stencil_mode(STENCIL_MODE_DISABLED);

	flags[FLAG_ALBEDO_TEXTURE_MSDF] = false;
	flags[FLAG_USE_TEXTURE_REPEAT] = true;

	current_key.invalid_key = 1;
}

BaseMaterial3D::~BaseMaterial3D() {
	ERR_FAIL_NULL(RS::get_singleton());

	{
		MutexLock lock(shader_map_mutex);
		if (shader_map.has(current_key)) {
			shader_map[current_key].users--;
			if (shader_map[current_key].users == 0) {
				// Deallocate shader which is no longer in use.
				RS::get_singleton()->free_rid(shader_map[current_key].shader);
				shader_map.erase(current_key);
			}
		}
	}

	if (_get_material().is_valid()) {
		RS::get_singleton()->material_set_shader(_get_material(), RID());
	}
}

//////////////////////

#ifndef DISABLE_DEPRECATED
// Kept for compatibility from 3.x to 4.0.
bool StandardMaterial3D::_set(const StringName &p_name, const Variant &p_value) {
	if (p_name == "flags_transparent") {
		bool transparent = p_value;
		if (transparent) {
			set_transparency(TRANSPARENCY_ALPHA);
		}
		return true;
	} else if (p_name == "flags_unshaded") {
		bool unshaded = p_value;
		if (unshaded) {
			set_shading_mode(SHADING_MODE_UNSHADED);
		}
		return true;
	} else if (p_name == "flags_vertex_lighting") {
		bool vertex_lit = p_value;
		if (vertex_lit && get_shading_mode() != SHADING_MODE_UNSHADED) {
			set_shading_mode(SHADING_MODE_PER_VERTEX);
		}
		return true;
	} else if (p_name == "params_use_alpha_scissor") {
		bool use_scissor = p_value;
		if (use_scissor) {
			set_transparency(TRANSPARENCY_ALPHA_SCISSOR);
		}
		return true;
	} else if (p_name == "params_use_alpha_hash") {
		bool use_hash = p_value;
		if (use_hash) {
			set_transparency(TRANSPARENCY_ALPHA_HASH);
		}
		return true;
	} else if (p_name == "params_depth_draw_mode") {
		int mode = p_value;
		if (mode == 3) {
			set_transparency(TRANSPARENCY_ALPHA_DEPTH_PRE_PASS);
		}
		return true;
	} else if (p_name == "depth_enabled") {
		bool enabled = p_value;
		if (enabled) {
			set_feature(FEATURE_HEIGHT_MAPPING, true);
			set_flag(FLAG_INVERT_HEIGHTMAP, true);
		}
		return true;
	} else {
		static const Pair<const char *, const char *> remaps[] = {
			{ "flags_use_shadow_to_opacity", "shadow_to_opacity" },
			{ "flags_use_shadow_to_opacity", "shadow_to_opacity" },
			{ "flags_no_depth_test", "no_depth_test" },
			{ "flags_use_point_size", "use_point_size" },
			{ "flags_fixed_size", "fixed_size" },
			{ "flags_albedo_tex_force_srgb", "albedo_texture_force_srgb" },
			{ "flags_do_not_receive_shadows", "disable_receive_shadows" },
			{ "flags_disable_ambient_light", "disable_ambient_light" },
			{ "params_diffuse_mode", "diffuse_mode" },
			{ "params_specular_mode", "specular_mode" },
			{ "params_blend_mode", "blend_mode" },
			{ "params_cull_mode", "cull_mode" },
			{ "params_depth_draw_mode", "params_depth_draw_mode" },
			{ "params_point_size", "point_size" },
			{ "params_billboard_mode", "billboard_mode" },
			{ "params_billboard_keep_scale", "billboard_keep_scale" },
			{ "params_grow", "grow" },
			{ "params_grow_amount", "grow_amount" },
			{ "params_alpha_scissor_threshold", "alpha_scissor_threshold" },
			{ "params_alpha_hash_scale", "alpha_hash_scale" },
			{ "params_alpha_antialiasing_edge", "alpha_antialiasing_edge" },

			{ "depth_scale", "heightmap_scale" },
			{ "depth_deep_parallax", "heightmap_deep_parallax" },
			{ "depth_min_layers", "heightmap_min_layers" },
			{ "depth_max_layers", "heightmap_max_layers" },
			{ "depth_flip_tangent", "heightmap_flip_tangent" },
			{ "depth_flip_binormal", "heightmap_flip_binormal" },
			{ "depth_texture", "heightmap_texture" },

			{ "emission_energy", "emission_energy_multiplier" },

			{ nullptr, nullptr },
		};

		int idx = 0;
		while (remaps[idx].first) {
			if (p_name == remaps[idx].first) {
				set(remaps[idx].second, p_value);
				return true;
			}
			idx++;
		}

		WARN_PRINT("Godot 3.x SpatialMaterial remapped parameter not found: " + String(p_name));
		return true;
	}
}

#endif // DISABLE_DEPRECATED

///////////////////////
