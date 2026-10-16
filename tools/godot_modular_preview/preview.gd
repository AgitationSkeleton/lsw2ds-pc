extends Node

const VIEW_SIZE := Vector2i(1280, 720)


func option(name: String, fallback: String = "") -> String:
	var prefix: String = "--%s=" % name
	for argument: String in OS.get_cmdline_user_args():
		if argument.begins_with(prefix):
			return argument.substr(prefix.length())
	return fallback


func load_glb(path: String) -> Node3D:
	var document := GLTFDocument.new()
	var state := GLTFState.new()
	var error: Error = document.append_from_file(path, state)
	if error != OK:
		push_error("GLB load failed (%d): %s" % [error, path])
		return null
	return document.generate_scene(state)


func find_first(root: Node, wanted_type: StringName) -> Node:
	var pending: Array[Node] = [root]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		if node.is_class(wanted_type):
			return node
		for child: Node in node.get_children():
			pending.push_back(child)
	return null


func attach_scene(
	skeleton: Skeleton3D,
	bone_name: String,
	scene: Node3D,
	local_transform: Transform3D = Transform3D.IDENTITY,
) -> bool:
	if skeleton.find_bone(bone_name) < 0:
		push_error("bone not found: %s" % bone_name)
		return false
	var attachment := BoneAttachment3D.new()
	attachment.name = "%s_Attachment" % bone_name
	skeleton.add_child(attachment)
	attachment.bone_name = bone_name
	attachment.add_child(scene)
	scene.transform = local_transform
	return true


func hide_surface(mesh_instance: MeshInstance3D, surface: int) -> void:
	if mesh_instance.mesh == null or surface < 0 or surface >= mesh_instance.mesh.get_surface_count():
		return
	var hidden := StandardMaterial3D.new()
	hidden.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	hidden.albedo_color = Color(1.0, 1.0, 1.0, 0.0)
	hidden.no_depth_test = true
	mesh_instance.set_surface_override_material(surface, hidden)


func add_light(parent: Node, rotation_value: Vector3, energy: float, color: Color) -> void:
	var light := DirectionalLight3D.new()
	light.rotation_degrees = rotation_value
	light.light_energy = energy
	light.light_color = color
	light.shadow_enabled = true
	parent.add_child(light)


func _ready() -> void:
	var body_path := ProjectSettings.globalize_path(option("body"))
	var head_path := ProjectSettings.globalize_path(option("head"))
	var weapon_path := ProjectSettings.globalize_path(option("weapon"))
	var output_path := ProjectSettings.globalize_path(option("output"))
	var animation_name := option("animation", "BlstrIdle")
	if body_path.is_empty() or head_path.is_empty() or output_path.is_empty():
		push_error("usage: -- --body=<body.glb> --head=<head.glb> [--weapon=<weapon.glb>] --output=<render.png>")
		get_tree().quit(2)
		return

	var body := load_glb(body_path)
	var head := load_glb(head_path)
	var weapon: Node3D = null
	if not weapon_path.is_empty():
		weapon = load_glb(weapon_path)
	if body == null or head == null or (not weapon_path.is_empty() and weapon == null):
		get_tree().quit(3)
		return
	var skeleton := find_first(body, &"Skeleton3D") as Skeleton3D
	var body_mesh := find_first(body, &"MeshInstance3D") as MeshInstance3D
	var player := find_first(body, &"AnimationPlayer") as AnimationPlayer
	if skeleton == null or body_mesh == null or player == null:
		push_error("body GLB lacks Skeleton3D, MeshInstance3D, or AnimationPlayer")
		get_tree().quit(4)
		return
	hide_surface(body_mesh, 0)
	if not attach_scene(skeleton, "Lego_Head", head):
		get_tree().quit(5)
		return
	if weapon != null:
		# Human_Model's RWeapon object is omitted by glTF conversion because it
		# does not skin geometry. This is its exact NSBMD rest transform relative
		# to Lego_Wrist_R, recovered by extract_nsbmd_object_trs.py.
		var weapon_basis := Basis(
			Vector3(0.0146484375, 0.030029296875, -0.99951171875),
			Vector3(-0.139892578125, 0.98974609375, 0.02783203125),
			Vector3(0.989990234375, 0.139404296875, 0.0185546875),
		)
		var weapon_transform := Transform3D(
			weapon_basis,
			Vector3(0.474609375, 0.1171875, 0.00439453125),
		)
		if not attach_scene(skeleton, "Lego_Wrist_R", weapon, weapon_transform):
			get_tree().quit(7)
			return

	var viewport := SubViewport.new()
	viewport.size = VIEW_SIZE
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.msaa_3d = Viewport.MSAA_4X
	viewport.own_world_3d = true
	add_child(viewport)
	viewport.add_child(body)
	if not player.has_animation(animation_name):
		push_error("animation not found: %s" % animation_name)
		get_tree().quit(6)
		return
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
	var error := image.save_png(output_path)
	if error != OK:
		push_error("PNG save failed (%d): %s" % [error, output_path])
		get_tree().quit(error)
		return
	print("Rendered modular character %s + %s -> %s" % [body_path, head_path, output_path])
	get_tree().quit()
