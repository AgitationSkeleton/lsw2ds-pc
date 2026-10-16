extends Node

const VIEW_SIZE := Vector2i(1280, 720)


func option(name: String, fallback: String = "") -> String:
	var prefix: String = "--%s=" % name
	for argument: String in OS.get_cmdline_user_args():
		if argument.begins_with(prefix):
			return argument.substr(prefix.length())
	return fallback


func find_animation_player(root: Node) -> AnimationPlayer:
	var pending: Array[Node] = [root]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		if node is AnimationPlayer:
			return node
		for child: Node in node.get_children():
			pending.push_back(child)
	return null


func add_light(parent: Node, rotation_value: Vector3, energy: float, color: Color) -> void:
	var light := DirectionalLight3D.new()
	light.rotation_degrees = rotation_value
	light.light_energy = energy
	light.light_color = color
	light.shadow_enabled = true
	parent.add_child(light)


func _ready() -> void:
	var input_path: String = ProjectSettings.globalize_path(option("input"))
	var output_path: String = ProjectSettings.globalize_path(option("output"))
	var animation_name: String = option("animation")
	if input_path.is_empty() or output_path.is_empty() or animation_name.is_empty():
		push_error("usage: -- --input=<model.glb> --output=<render.png> --animation=<idle>")
		get_tree().quit(2)
		return

	var document := GLTFDocument.new()
	var state := GLTFState.new()
	var error: Error = document.append_from_file(input_path, state)
	if error != OK:
		push_error("GLB load failed (%d): %s" % [error, input_path])
		get_tree().quit(error)
		return
	var model: Node3D = document.generate_scene(state)
	var player: AnimationPlayer = find_animation_player(model)
	if player == null or not player.has_animation(animation_name):
		push_error("animation not found: %s" % animation_name)
		get_tree().quit(3)
		return

	var viewport := SubViewport.new()
	viewport.size = VIEW_SIZE
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.msaa_3d = Viewport.MSAA_4X
	viewport.own_world_3d = true
	add_child(viewport)
	viewport.add_child(model)
	player.play(animation_name)
	player.seek(0.0, true)
	player.advance(0.0)

	var environment := WorldEnvironment.new()
	var settings := Environment.new()
	settings.background_mode = Environment.BG_COLOR
	settings.background_color = Color(0.035, 0.045, 0.065, 1.0)
	settings.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	settings.ambient_light_color = Color(0.72, 0.76, 0.86, 1.0)
	settings.ambient_light_energy = 0.82
	settings.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	environment.environment = settings
	viewport.add_child(environment)
	add_light(viewport, Vector3(-48, -32, 0), 1.15, Color(1.0, 0.91, 0.78))
	add_light(viewport, Vector3(-30, 145, 0), 0.55, Color(0.58, 0.72, 1.0))

	# All playable minifigure rigs use the same DS-world scale and origin. Framing
	# this volume avoids the intentionally exploded modular bind-pose AABB.
	var center := Vector3(0.0, 5.0, 0.0)
	var radius := 9.0
	var camera := Camera3D.new()
	camera.fov = 32.0
	camera.near = 0.05
	camera.far = 300.0
	var direction := Vector3(1.15, 0.45, 1.65).normalized()
	camera.position = center + direction * radius * 2.25
	viewport.add_child(camera)
	camera.look_at(center, Vector3.UP)
	camera.current = true

	await RenderingServer.frame_post_draw
	await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var image: Image = viewport.get_texture().get_image()
	DirAccess.make_dir_recursive_absolute(output_path.get_base_dir())
	error = image.save_png(output_path)
	if error != OK:
		push_error("PNG save failed (%d): %s" % [error, output_path])
		get_tree().quit(error)
		return
	print("Rendered %s [%s] -> %s" % [input_path, animation_name, output_path])
	get_tree().quit()
