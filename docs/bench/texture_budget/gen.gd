# Writes the bench's textures (docs/bench/texture_budget/main.gd): each one twice, imported as a plain
# Texture2D and as a Texture2D Streamed, so a frame drawn with each can be compared.
#
# godot --headless --path docs/bench/texture_budget --script gen.gd ; godot --headless --path docs/bench/texture_budget --import
extends SceneTree

const SIZE := 4096
const NAMES := ["floor", "wall", "poster", "far", "leaves"]


func _init() -> void:
	DirAccess.make_dir_recursive_absolute("res://tex")
	for i in NAMES.size():
		var img := _detail(NAMES[i], i)
		for streamed in [false, true]:
			var path := "res://tex/%s%s.png" % [NAMES[i], "_s" if streamed else ""]
			img.save_png(path)
			var f := FileAccess.open(path + ".import", FileAccess.WRITE)
			if streamed:
				f.store_string("[remap]\n\nimporter=\"streamed_texture_2d\"\ntype=\"StreamedTexture2D\"\n\n[params]\n\n")
			else:
				f.store_string("[remap]\n\nimporter=\"texture\"\ntype=\"CompressedTexture2D\"\n\n[params]\n\ncompress/mode=2\nmipmaps/generate=true\ndetect_3d/compress_to=0\n")
			f.close()
	print("texture_budget: wrote %d textures" % (NAMES.size() * 2))
	quit()


# Fine detail at every scale (noise octaves plus a one-texel grid), so a missing mip shows in a 100% crop.
func _detail(name: String, seed_value: int) -> Image:
	var noise := FastNoiseLite.new()
	noise.seed = seed_value * 31 + 5
	noise.frequency = 0.004
	noise.fractal_octaves = 8
	var img := noise.get_seamless_image(SIZE, SIZE, false, false, 0.1)
	img.convert(Image.FORMAT_RGBA8)
	var tint := Color.from_hsv(float(seed_value) / NAMES.size(), 0.5, 1.0)
	var data := img.get_data()
	for y in range(0, SIZE, 1):
		var row := y * SIZE * 4
		var line := (y % 64) == 0
		for x in range(0, SIZE, 16):
			var o := row + x * 4
			if line or (x % 64) == 0:
				data[o] = 255
				data[o + 1] = 255
				data[o + 2] = 255
	img.set_data(SIZE, SIZE, false, Image.FORMAT_RGBA8, data)
	img.adjust_bcs(1.0, 1.0, 1.0)
	var out := Image.create(SIZE, SIZE, false, Image.FORMAT_RGBA8)
	out.blit_rect(img, Rect2i(0, 0, SIZE, SIZE), Vector2i.ZERO)
	if name == "leaves":
		# Alpha-tested cards: alpha from the noise, cut at 0.5.
		var a := out.get_data()
		for i in range(0, a.size(), 4):
			a[i + 3] = 255 if a[i] > 128 else 0
		out.set_data(SIZE, SIZE, false, Image.FORMAT_RGBA8, a)
	out.fill_rect(Rect2i(0, 0, SIZE, 8), tint)
	return out
