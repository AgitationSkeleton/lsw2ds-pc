extends Node

const VIEW_SIZE := Vector2i(960, 720)


func option(name: String, fallback: String = "") -> String:
	var prefix := "--%s=" % name
	for argument: String in OS.get_cmdline_user_args():
		if argument.begins_with(prefix):
			return argument.substr(prefix.length())
	return fallback


func load_glb(path: String) -> Node3D:
	var document := GLTFDocument.new()
	var state := GLTFState.new()
	var error := document.append_from_file(path, state)
	if error != OK:
		push_error("GLB load failed (%d): %s" % [error, path])
		return null
	return document.generate_scene(state)


func descendants(root: Node) -> Array[Node]:
	var result: Array[Node] = []
	var pending: Array[Node] = [root]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		result.append(node)
		for child: Node in node.get_children():
			pending.push_back(child)
	return result


func first_of_type(root: Node, wanted: StringName) -> Node:
	for node in descendants(root):
		if node.is_class(wanted):
			return node
	return null


func play_first_animation(root: Node) -> void:
	var player := first_of_type(root, &"AnimationPlayer") as AnimationPlayer
	if player == null:
		return
	for animation: StringName in player.get_animation_list():
		if animation == &"RESET":
			continue
		player.play(animation)
		player.seek(0.0, true)
		player.advance(0.0)
		return


func attach(skeleton: Skeleton3D, bone: String, scene: Node3D) -> bool:
	if skeleton.find_bone(bone) < 0:
		push_error("bone not found: %s" % bone)
		return false
	var attachment := BoneAttachment3D.new()
	attachment.name = "%s_RuntimeAttachment" % bone
	skeleton.add_child(attachment)
	attachment.bone_name = bone
	attachment.add_child(scene)
	return true


func load_texture(path: String) -> Texture2D:
	if path.is_empty():
		return null
	var image := Image.load_from_file(path)
	if image == null or image.is_empty():
		push_error("texture load failed: %s" % path)
		return null
	return ImageTexture.create_from_image(image)


func override_texture(root: Node, path: String) -> void:
	var texture := load_texture(path)
	if texture == null:
		return
	for node in descendants(root):
		if node is MeshInstance3D:
			var mesh_instance := node as MeshInstance3D
			if mesh_instance.mesh == null:
				continue
			for surface in range(mesh_instance.mesh.get_surface_count()):
				var original := mesh_instance.mesh.surface_get_material(surface)
				var material: BaseMaterial3D
				if original is BaseMaterial3D:
					material = original.duplicate() as BaseMaterial3D
				else:
					material = StandardMaterial3D.new()
				material.albedo_texture = texture
				material.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST_WITH_MIPMAPS
				material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
				material.alpha_scissor_threshold = 0.5
				mesh_instance.set_surface_override_material(surface, material)


func disable_backface_culling(root: Node) -> void:
	# Nitro models carry a per-polygon cull mode that apicula does not bring
	# across to glTF, so some faces arrive with inverted winding and vanish
	# under glTF's default back-face culling. The clearest symptom was every
	# bare LEGO head rendering with its crown sliced off; helmets and hair hid
	# it on most of the roster. Disabling culling can only reveal geometry that
	# should already be there.
	for node in root.find_children("*", "MeshInstance3D", true, false):
		var instance := node as MeshInstance3D
		if instance.mesh == null:
			continue
		for surface in instance.mesh.get_surface_count():
			var original := instance.mesh.surface_get_material(surface)
			var material := (
				original.duplicate() if original is BaseMaterial3D
				else StandardMaterial3D.new()
			) as BaseMaterial3D
			material.cull_mode = BaseMaterial3D.CULL_DISABLED
			instance.set_surface_override_material(surface, material)


func hide_generic_head(root: Node) -> void:
	var mesh_instance := first_of_type(root, &"MeshInstance3D") as MeshInstance3D
	if mesh_instance == null or mesh_instance.mesh == null or mesh_instance.mesh.get_surface_count() == 0:
		return
	var hidden := StandardMaterial3D.new()
	hidden.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	hidden.albedo_color = Color(1, 1, 1, 0)
	hidden.no_depth_test = true
	mesh_instance.set_surface_override_material(0, hidden)


func add_light(parent: Node, degrees: Vector3, energy: float, color: Color) -> void:
	var light := DirectionalLight3D.new()
	light.rotation_degrees = degrees
	light.light_energy = energy
	light.light_color = color
	light.shadow_enabled = true
	parent.add_child(light)


func _ready() -> void:
	var body_path := ProjectSettings.globalize_path(option("body"))
	var output_path := ProjectSettings.globalize_path(option("output"))
	if body_path.is_empty() or output_path.is_empty():
		push_error("usage: -- --body=<body.glb> --output=<render.png> [attachments]")
		get_tree().quit(2)
		return
	var body := load_glb(body_path)
	if body == null:
		get_tree().quit(3)
		return
	var skeleton := first_of_type(body, &"Skeleton3D") as Skeleton3D
	var player := first_of_type(body, &"AnimationPlayer") as AnimationPlayer
	if skeleton == null or player == null:
		push_error("body GLB lacks Skeleton3D or AnimationPlayer")
		get_tree().quit(4)
		return

	for specification in [
		["head", "head-texture", "Lego_Head"],
		["gear", "gear-texture", "Lego_Head"],
		["back-gear", "back-gear-texture", "Lego_Chest"],
	]:
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
		if specification[0] == "head":
			hide_generic_head(body)

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
	if error != OK:
		push_error("PNG save failed (%d): %s" % [error, output_path])
		get_tree().quit(error)
		return
	print("Rendered runtime composite -> %s" % output_path)
	get_tree().quit()
