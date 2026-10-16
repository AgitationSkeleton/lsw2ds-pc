extends Node

const VIEW_SIZE := Vector2i(1280, 720)


func option(name: String, fallback: String = "") -> String:
	var prefix: String = "--%s=" % name
	for argument: String in OS.get_cmdline_user_args():
		if argument.begins_with(prefix):
			return argument.substr(prefix.length())
	return fallback


func combined_bounds(root: Node3D) -> AABB:
	var result := AABB()
	var found := false
	var pending: Array[Node] = [root]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		if node is MeshInstance3D and node.mesh != null:
			var local_bounds: AABB = node.mesh.get_aabb()
			var relative_transform: Transform3D = (
				root.global_transform.affine_inverse() * node.global_transform
			)
			var transformed_bounds: AABB = relative_transform * local_bounds
			result = transformed_bounds if not found else result.merge(transformed_bounds)
			found = true
		for child: Node in node.get_children():
			pending.push_back(child)
	return result if found else AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2))


func add_light(parent: Node, rotation_degrees_value: Vector3, energy: float, color: Color) -> void:
	var light := DirectionalLight3D.new()
	light.rotation_degrees = rotation_degrees_value
	light.light_energy = energy
	light.light_color = color
	light.shadow_enabled = true
	parent.add_child(light)


func _ready() -> void:
	var input_path: String = option("input")
	var output_path: String = option("output")
	if input_path.is_empty() or output_path.is_empty():
		push_error("usage: -- --input=<model.glb> --output=<render.png>")
		get_tree().quit(2)
		return

	input_path = ProjectSettings.globalize_path(input_path)
	output_path = ProjectSettings.globalize_path(output_path)
	var document := GLTFDocument.new()
	var state := GLTFState.new()
	var error: Error = document.append_from_file(input_path, state)
	if error != OK:
		push_error("GLB load failed (%d): %s" % [error, input_path])
		get_tree().quit(error)
		return
	var model: Node3D = document.generate_scene(state)
	if model == null:
		push_error("GLB scene generation failed: %s" % input_path)
		get_tree().quit(3)
		return

	var viewport := SubViewport.new()
	viewport.size = VIEW_SIZE
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.msaa_3d = Viewport.MSAA_4X
	viewport.own_world_3d = true
	add_child(viewport)
	viewport.add_child(model)

	var environment := WorldEnvironment.new()
	var settings := Environment.new()
	settings.background_mode = Environment.BG_COLOR
	settings.background_color = Color(0.035, 0.045, 0.065, 1.0)
	settings.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	settings.ambient_light_color = Color(0.72, 0.76, 0.86, 1.0)
	settings.ambient_light_energy = 0.75
	settings.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	environment.environment = settings
	viewport.add_child(environment)

	add_light(viewport, Vector3(-48, -32, 0), 1.15, Color(1.0, 0.91, 0.78))
	add_light(viewport, Vector3(-30, 145, 0), 0.55, Color(0.58, 0.72, 1.0))
	var bounds: AABB = combined_bounds(model)
	var center: Vector3 = bounds.get_center()
	var radius: float = maxf(bounds.size.length() * 0.5, 0.1)
	var camera := Camera3D.new()
	camera.fov = 32.0
	camera.near = maxf(radius / 500.0, 0.001)
	camera.far = maxf(radius * 20.0, 100.0)
	var direction := Vector3(1.35, 1.05, 1.35).normalized()
	camera.position = center + direction * radius * 2.25
	viewport.add_child(camera)
	camera.look_at(center, Vector3.UP)
	camera.current = true

	await RenderingServer.frame_post_draw
	await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var image: Image = viewport.get_texture().get_image()
	var parent_dir: String = output_path.get_base_dir()
	DirAccess.make_dir_recursive_absolute(parent_dir)
	error = image.save_png(output_path)
	if error != OK:
		push_error("PNG save failed (%d): %s" % [error, output_path])
		get_tree().quit(error)
		return
	print("Rendered %s -> %s; bounds=%s" % [input_path, output_path, bounds])
	get_tree().quit()
