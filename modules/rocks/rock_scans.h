/**************************************************************************/
/*  rock_scans.h                                                          */
/**************************************************************************/

#pragma once

// Scanned texture detail for the rocks module's own surfaces (stones, caves): the sections of the art folder's
// scans.cfg marked system="rocks" (TerrainMaterial3D.load_scan_set leaves them to this loader). Each scan is detail
// only: the shaders divide it by its mean (linear), so the stone or wall keeps its own colour
// (docs/ideas/scan-textures.md).

#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "scene/resources/image_texture.h"
#include "servers/rendering/scan_texture_limit.h"

struct RockScan {
	Ref<Texture2D> albedo;
	Ref<Texture2D> normal;
	Vector3 mean = Vector3(1, 1, 1); // the albedo's linear mean
	float tile = 2.0f; // metres one repeat covers (the scan's real size)
	float normal_strength = 1.0f;
	bool is_valid() const { return albedo.is_valid(); }
};

inline Ref<Texture2D> rock_scan_texture(const String &p_path, bool p_normal, Vector3 *r_mean) {
	if (!FileAccess::exists(p_path)) {
		return Ref<Texture2D>();
	}
	Ref<Image> img = Image::load_from_file(p_path);
	if (img.is_null() || img->is_empty()) {
		return Ref<Texture2D>();
	}
	scan_texture_limit(img);
	if (r_mean) {
		Ref<Image> small = img->duplicate();
		small->convert(Image::FORMAT_RGBA8);
		small->resize(32, 32, Image::INTERPOLATE_BILINEAR);
		Color sum(0, 0, 0, 0);
		for (int y = 0; y < 32; y++) {
			for (int x = 0; x < 32; x++) {
				sum += small->get_pixel(x, y).srgb_to_linear();
			}
		}
		*r_mean = Vector3(sum.r, sum.g, sum.b) / 1024.0f;
		*r_mean = r_mean->max(Vector3(0.001, 0.001, 0.001));
	}
	img->generate_mipmaps(p_normal);
	return ImageTexture::create_from_image(img);
}

// One section of <dir>/scans.cfg (scan = folder and file prefix, tile = metres, normal = strength).
inline bool rock_scan_load(const String &p_dir, const String &p_section, RockScan &r_scan) {
	r_scan = RockScan();
	Ref<ConfigFile> cfg;
	cfg.instantiate();
	if (!DirAccess::dir_exists_absolute(p_dir) || cfg->load(p_dir.path_join("scans.cfg")) != OK || !cfg->has_section(p_section)) {
		return false;
	}
	const String scan = cfg->get_value(p_section, "scan", "");
	const String base = p_dir.path_join(scan).path_join(scan);
	r_scan.albedo = rock_scan_texture(base + "_albedo.jpg", false, &r_scan.mean);
	r_scan.normal = rock_scan_texture(base + "_normal.jpg", true, nullptr);
	r_scan.tile = MAX(float(cfg->get_value(p_section, "tile", 2.0f)), 0.01f);
	r_scan.normal_strength = cfg->get_value(p_section, "normal", 1.0f);
	return r_scan.is_valid();
}
