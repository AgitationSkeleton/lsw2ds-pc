extends "res://preview.gd"
const HeadCap = preload("res://head_cap.gd")

func _ready() -> void:
	var body := load_glb(ProjectSettings.globalize_path(option("body")))
	disable_backface_culling(body)
	var skel: Skeleton3D
	for n in body.find_children("*", "Skeleton3D", true, false):
		skel = n
		break
	var cap := HeadCap.apply(body, skel)
	print("CAP node=", cap, " tris=", (cap as MeshInstance3D).mesh.get_faces().size() / 3 if cap else -1)
	var vp := SubViewport.new()
	vp.size = Vector2i(480, 480)
	vp.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	vp.own_world_3d = true
	add_child(vp)
	vp.add_child(body)
	var we := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.05, 0.05, 0.08)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(1, 1, 1)
	env.ambient_light_energy = 1.0
	we.environment = env
	vp.add_child(we)
	add_light(vp, Vector3(-55, -35, 0), 1.3, Color(1, 1, 1))
	var cam := Camera3D.new()
	cam.projection = Camera3D.PROJECTION_ORTHOGONAL
	cam.size = 3.4
	cam.position = Vector3(1.4, 9.9, 2.4)
	vp.add_child(cam)
	cam.look_at(Vector3(0, 7.9, 0))
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	vp.get_texture().get_image().save_png(ProjectSettings.globalize_path(option("output")))
	get_tree().quit(0)
