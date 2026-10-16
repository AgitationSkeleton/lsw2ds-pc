extends "res://preview.gd"

const HeadCap = preload("res://head_cap.gd")


func _ready() -> void:
	var body_path := ProjectSettings.globalize_path(option("body"))
	var output_path := ProjectSettings.globalize_path(option("output"))
	if body_path.is_empty() or output_path.is_empty():
		get_tree().quit(2)
		return
	var body := load_glb(body_path)
	if body == null:
		get_tree().quit(3)
		return
	# Base body only. Attachments keep normal culling: capes and helmets are
	# wound correctly, and drawing their interior faces makes them render as
	# blank white sheets.
	disable_backface_culling(body)
	var skeleton := first_of_type(body, &"Skeleton3D") as Skeleton3D
	var player := first_of_type(body, &"AnimationPlayer") as AnimationPlayer
	if skeleton == null or player == null:
		get_tree().quit(4)
		return
	# This must happen before adding an attachment below the skeleton.  Once
	# attached, a first-mesh traversal can select and hide the new head rather
	# than Human_Model's generic Face surface.
	if not option("head").is_empty():
		hide_generic_head(body)
	for specification in [["head", "head-texture", "Lego_Head"], ["gear", "gear-texture", "Lego_Head"]]:
		var path := ProjectSettings.globalize_path(option(specification[0]))
		if path.is_empty():
			continue
		var scene := load_glb(path)
		if scene == null:
			get_tree().quit(5)
			return
		override_texture(scene, ProjectSettings.globalize_path(option(specification[1])))
		if not attach(skeleton, specification[2], scene):
			get_tree().quit(6)
			return
	var back_path := ProjectSettings.globalize_path(option("back-gear"))
	if not back_path.is_empty():
		var back := load_glb(back_path)
		if back == null:
			get_tree().quit(7)
			return
		override_texture(back, ProjectSettings.globalize_path(option("back-gear-texture")))
		# Back models come in two flavours and the difference is measurable.
		# Capes are authored in complete-character space: Cape_Model's own
		# joint sits at y=6.22 and its geometry spans 0.68..6.29, so adding it
		# to the body at identity is already correct and a chest attachment
		# would apply torso height twice. Jetpacks are authored around their
		# own origin: BobaFettPack spans -2.25..-0.94, so at identity it lands
		# under the character's feet and it needs the chest bone.
		# Which space this model is authored in is decided when the manifest is
		# built, by measuring the GLB, and passed in. Capes carry their own
		# torso height and must sit at identity; packs are built around their
		# own origin and need the chest bone, otherwise they land at the feet.
		if option("back-gear-space", "character") == "character":
			body.add_child(back)
		elif not attach(skeleton, "Lego_Chest", back):
			get_tree().quit(7)
			return

	# Only characters using the built-in head need the crown closed. "gear"
	# attaches to Lego_Head as well and is where helmets arrive - Boba Fett and
	# Biggs among them - so a cap there would push a stud through the helmet.
	if option("head").is_empty() and option("gear").is_empty():
		HeadCap.apply(body, skeleton)

	var viewport := SubViewport.new()
	viewport.size = VIEW_SIZE
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.msaa_3d = Viewport.MSAA_4X
	viewport.own_world_3d = true
	add_child(viewport)
	viewport.add_child(body)
	var animation := option("animation", "BlstrIdle")
	if player.has_animation(animation):
		player.play(animation)
		player.seek(0.0, true)
		player.advance(0.0)

	var world_environment := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.035, 0.045, 0.065, 1)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.72, 0.76, 0.86, 1)
	environment.ambient_light_energy = 0.82
	environment.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	world_environment.environment = environment
	viewport.add_child(world_environment)
	add_light(viewport, Vector3(-48, -32, 0), 1.15, Color(1, 0.91, 0.78))
	add_light(viewport, Vector3(-30, 145, 0), 0.55, Color(0.58, 0.72, 1))
	var camera := Camera3D.new()
	camera.fov = 30.0
	camera.near = 0.05
	camera.far = 300.0
	camera.position = Vector3(12.2, 9.7, 17.5)
	viewport.add_child(camera)
	camera.look_at(Vector3(0, 5.0, 0), Vector3.UP)
	camera.current = true
	await RenderingServer.frame_post_draw
	await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var image := viewport.get_texture().get_image()
	DirAccess.make_dir_recursive_absolute(output_path.get_base_dir())
	var error := image.save_png(output_path)
	get_tree().quit(error)
