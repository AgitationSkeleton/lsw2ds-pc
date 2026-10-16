class_name RuntimeWeaponHelpers
extends RefCounted


static func remove_inactive_flash_geometry(root: Node) -> void:
	var pending: Array[Node] = [root]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		for child: Node in node.get_children():
			pending.push_back(child)
		if node is not MeshInstance3D:
			continue
		var mesh_instance := node as MeshInstance3D
		if mesh_instance.mesh == null:
			continue
		var rebuilt := ArrayMesh.new()
		var changed := false
		for surface in range(mesh_instance.mesh.get_surface_count()):
			var material := mesh_instance.mesh.surface_get_material(surface)
			var arrays := mesh_instance.mesh.surface_get_arrays(surface)
			if material != null and material.resource_name.to_lower() == "blstrtrans":
				var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
				if indices.size() >= 6:
					# StTrprBlstr's BlstrTrans primitive contains three red muzzle-stud
					# rings followed by one six-index firing quad.  Removing the entire
					# primitive erased Leia/Boushh's red stud; trim only that final quad.
					indices.resize(indices.size() - 6)
					arrays[Mesh.ARRAY_INDEX] = indices
					changed = true
			rebuilt.add_surface_from_arrays(mesh_instance.mesh.surface_get_primitive_type(surface), arrays)
			rebuilt.surface_set_material(rebuilt.get_surface_count() - 1, material)
			rebuilt.surface_set_name(rebuilt.get_surface_count() - 1, mesh_instance.mesh.surface_get_name(surface))
		if changed:
			mesh_instance.mesh = rebuilt


static func attach_weapon(skeleton: Skeleton3D, hand: String, scene: Node3D, roll_degrees: float = 0.0) -> bool:
	var bone_name := "Lego_Wrist_R" if hand == "right" else "Lego_Wrist_L"
	if skeleton.find_bone(bone_name) < 0:
		push_error("weapon bone not found: %s" % bone_name)
		return false
	remove_inactive_flash_geometry(scene)
	var attachment := BoneAttachment3D.new()
	attachment.name = "%s_WeaponAttachment" % hand.capitalize()
	skeleton.add_child(attachment)
	attachment.bone_name = bone_name
	attachment.add_child(scene)
	if hand == "right":
		scene.transform = Transform3D(
			Basis(
				Vector3(0.0146484375, 0.030029296875, -0.99951171875),
				Vector3(-0.139892578125, 0.98974609375, 0.02783203125),
				Vector3(0.989990234375, 0.139404296875, 0.0185546875),
			),
			Vector3(0.474609375, 0.1171875, 0.00439453125),
		)
	else:
		scene.transform = Transform3D(
			Basis(
				Vector3(0.009765625, -0.0712890625, -0.997314453125),
				Vector3(0.1328125, -0.988525390625, 0.072021484375),
				Vector3(-0.9912109375, -0.133056640625, -0.000244140625),
			),
			Vector3(-0.4755859375, -0.112060546875, -0.014892578125),
		)
	if not is_zero_approx(roll_degrees):
		scene.rotate_object_local(Vector3(0, 0, 1), deg_to_rad(roll_degrees))
	return true
