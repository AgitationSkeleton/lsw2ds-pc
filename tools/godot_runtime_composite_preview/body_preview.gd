extends "res://preview.gd"


func material_matches_slot(material_name: String, slot: String) -> bool:
	var lowered := material_name.to_lower()
	if slot == "head":
		return "face" in lowered or "head" in lowered
	if slot == "legs":
		return "leg" in lowered
	if slot == "torso":
		return "torso" in lowered
	return false


func override_body_slot(body: Node, slot: String, path: String) -> void:
	var texture := load_texture(path)
	if texture == null:
		return
	for node in descendants(body):
		if node is MeshInstance3D:
			var mesh_instance := node as MeshInstance3D
			if mesh_instance.mesh == null:
				continue
			for surface in range(mesh_instance.mesh.get_surface_count()):
				var original := mesh_instance.mesh.surface_get_material(surface)
				if original == null or not material_matches_slot(original.resource_name, slot):
					continue
				var material: BaseMaterial3D
				if original is BaseMaterial3D:
					material = original.duplicate() as BaseMaterial3D
				else:
					material = StandardMaterial3D.new()
				material.albedo_texture = texture
				material.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST_WITH_MIPMAPS
				material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
				mesh_instance.set_surface_override_material(surface, material)


func combined_bounds(root: Node3D) -> AABB:
	var result := AABB()
	var found := false
	for node in descendants(root):
		if node is MeshInstance3D and node.mesh != null:
			var local_bounds: AABB = node.mesh.get_aabb()
			var relative: Transform3D = root.global_transform.affine_inverse() * node.global_transform
			var transformed: AABB = relative * local_bounds
			result = transformed if not found else result.merge(transformed)
			found = true
	return result if found else AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2))


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
	for slot in ["head", "legs", "torso"]:
		override_body_slot(body, slot, ProjectSettings.globalize_path(option("body-%s-texture" % slot)))

	var skeleton := first_of_type(body, &"Skeleton3D") as Skeleton3D
	if skeleton != null:
		if not option("head").is_empty():
			hide_generic_head(body)
		for specification in [["head", "head-texture", "Lego_Head"], ["gear", "gear-texture", "Lego_Head"]]:
			var path := ProjectSettings.globalize_path(option(specification[0]))
			if path.is_empty():
				continue
			var scene := load_glb(path)
			if scene == null:
				get_tree().quit(4)
				return
			override_texture(scene, ProjectSettings.globalize_path(option(specification[1])))
			if not attach(skeleton, specification[2], scene):
				get_tree().quit(5)
				return
	var back_path := ProjectSettings.globalize_path(option("back-gear"))
	if not back_path.is_empty():
		var back := load_glb(back_path)
		if back == null:
			get_tree().quit(6)
			return
		override_texture(back, ProjectSettings.globalize_path(option("back-gear-texture")))
		body.add_child(back)
		play_first_animation(back)

	var viewport := SubViewport.new()
	viewport.size = VIEW_SIZE
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.msaa_3d = Viewport.MSAA_4X
	viewport.own_world_3d = true
	add_child(viewport)
	viewport.add_child(body)
	var player := first_of_type(body, &"AnimationPlayer") as AnimationPlayer
	var animation := option("animation")
	if player != null and not animation.is_empty() and player.has_animation(animation):
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

	var bounds := combined_bounds(body)
	var center := bounds.get_center()
	var radius := maxf(bounds.size.length() * 0.5, 0.1)
	var camera := Camera3D.new()
	camera.fov = 32.0
	camera.near = maxf(radius / 500.0, 0.001)
	camera.far = maxf(radius * 20.0, 100.0)
	var camera_direction := Vector3(1.25, 0.75, 1.55)
	var camera_values := option("camera-direction").split(",")
	if camera_values.size() == 3:
		camera_direction = Vector3(camera_values[0].to_float(), camera_values[1].to_float(), camera_values[2].to_float())
	camera.position = center + camera_direction.normalized() * radius * 2.25
	viewport.add_child(camera)
	camera.look_at(center, Vector3.UP)
	camera.current = true
	await RenderingServer.frame_post_draw
	await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var image := viewport.get_texture().get_image()
	DirAccess.make_dir_recursive_absolute(output_path.get_base_dir())
	var error := image.save_png(output_path)
	get_tree().quit(error)
