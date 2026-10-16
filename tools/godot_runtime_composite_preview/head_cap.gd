extends RefCounted

# Closes the top of the shared LEGO head and restores its stud.
# Kept in step with godot/runtime/head_cap.gd.
#
# WHY THIS EXISTS. Human_Model's head arrives from the Nitro conversion as an
# open cylinder. Measured on the exported body: exactly seven vertices sit at
# y = 8.046 forming the crown ring, and there is not a single triangle above
# y = 7.7. The ring is there, the cap that closes it is not. Nitro's three
# shapes all survive the conversion, so nothing was dropped at that level - the
# cap is a triangle fan over a heptagon and apicula does not bring it across.
#
# Characters with a real head model are unaffected, which is why the Tusken
# Raider shows a proper studded head: asset 5104 is its own model and converts
# whole. Only characters falling back to the built-in head are bald and open,
# Mace Windu among them.
#
# THIS GEOMETRY IS RECONSTRUCTED, NOT DECODED. The ring positions are ROM data;
# the fan closing them and the stud above are authored here to LEGO
# proportions. The DS model tops out at the ring, so nothing in the export
# says how tall the real stud is.

# Measured from the crown ring of the exported Human_Model.
const RING_Y := 8.046
const RING_RADIUS := 1.19
const RING_SIDES := 7
const HEAD_BONE := "Lego_Head"
# LEGO stud proportions relative to the head.
const STUD_RADIUS := 0.62
const STUD_HEIGHT := 0.34
const CAP_NAME := "Lego_Head_Cap"


static func _skin_colour(body: Node3D) -> Color:
	# Take the crown colour from the head texture itself so the cap matches the
	# character. The top-centre texel of a LEGO face texture is scalp.
	for node in body.find_children("*", "MeshInstance3D", true, false):
		var instance := node as MeshInstance3D
		if instance.mesh == null:
			continue
		for surface in instance.mesh.get_surface_count():
			var material := instance.get_surface_override_material(surface)
			if material == null:
				material = instance.mesh.surface_get_material(surface)
			if not material is BaseMaterial3D:
				continue
			var texture := (material as BaseMaterial3D).albedo_texture
			if texture == null:
				continue
			var image := texture.get_image()
			if image == null or image.is_empty():
				continue
			return image.get_pixel(int(image.get_width() * 0.5), 1)
	return Color(0.86, 0.60, 0.32)


static func _build_mesh(colour: Color) -> ArrayMesh:
	var surface := SurfaceTool.new()
	surface.begin(Mesh.PRIMITIVE_TRIANGLES)
	surface.set_color(colour)

	# Fan closing the crown ring.
	for side in RING_SIDES:
		var a := TAU * float(side) / float(RING_SIDES)
		var b := TAU * float(side + 1) / float(RING_SIDES)
		var first := Vector3(cos(a) * RING_RADIUS, 0.0, sin(a) * RING_RADIUS)
		var second := Vector3(cos(b) * RING_RADIUS, 0.0, sin(b) * RING_RADIUS)
		# Wind the same way as the stud cap below. With CULL_DISABLED Godot
		# flips the normal on back-facing fragments, so a reversed fan shades
		# as if it pointed at the floor and reads as a dark ring.
		surface.set_normal(Vector3.UP)
		surface.add_vertex(Vector3.ZERO)
		surface.add_vertex(first)
		surface.add_vertex(second)

	# Stud: a short cylinder with its own cap.
	for side in RING_SIDES * 2:
		var a := TAU * float(side) / float(RING_SIDES * 2)
		var b := TAU * float(side + 1) / float(RING_SIDES * 2)
		var lower_a := Vector3(cos(a) * STUD_RADIUS, 0.0, sin(a) * STUD_RADIUS)
		var lower_b := Vector3(cos(b) * STUD_RADIUS, 0.0, sin(b) * STUD_RADIUS)
		var upper_a := lower_a + Vector3.UP * STUD_HEIGHT
		var upper_b := lower_b + Vector3.UP * STUD_HEIGHT
		surface.set_normal((lower_a + lower_b).normalized())
		surface.add_vertex(lower_a)
		surface.add_vertex(upper_b)
		surface.add_vertex(upper_a)
		surface.add_vertex(lower_a)
		surface.add_vertex(lower_b)
		surface.add_vertex(upper_b)
		surface.set_normal(Vector3.UP)
		surface.add_vertex(Vector3.UP * STUD_HEIGHT)
		surface.add_vertex(upper_a)
		surface.add_vertex(upper_b)

	var material := StandardMaterial3D.new()
	material.albedo_color = colour
	material.vertex_color_use_as_albedo = false
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	surface.set_material(material)
	return surface.commit()


static func apply(body: Node3D, skeleton: Skeleton3D) -> Node3D:
	if body == null or skeleton == null:
		return null
	if body.has_node(CAP_NAME):
		return body.get_node(CAP_NAME) as Node3D
	var bone := skeleton.find_bone(HEAD_BONE)
	if bone < 0:
		return null
	var instance := MeshInstance3D.new()
	instance.name = CAP_NAME
	instance.mesh = _build_mesh(_skin_colour(body))
	var attachment := BoneAttachment3D.new()
	skeleton.add_child(attachment)
	attachment.bone_name = HEAD_BONE
	attachment.add_child(instance)
	# The ring position is model-space, but the head bone's rest basis is
	# rotated, so offsetting along the attachment's local +Y pushes the cap out
	# in front of the face instead of up. Transform the model-space target
	# through the bone's inverse rest so it lands on the crown and still rides
	# the bone during animation.
	var rest := skeleton.get_bone_global_rest(bone)
	instance.transform = rest.affine_inverse() * Transform3D(
		Basis(), Vector3(0.0, RING_Y, 0.0)
	)
	return instance
