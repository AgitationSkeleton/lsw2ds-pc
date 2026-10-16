extends "res://preview.gd"


func _ready() -> void:
	var path := ProjectSettings.globalize_path(option("model"))
	if path.is_empty():
		get_tree().quit(2)
		return
	var root := load_glb(path)
	if root == null:
		get_tree().quit(3)
		return
	for node in descendants(root):
		if node is not MeshInstance3D:
			continue
		var mesh_instance := node as MeshInstance3D
		print("mesh=", mesh_instance.name, " surfaces=", mesh_instance.mesh.get_surface_count())
		for surface in range(mesh_instance.mesh.get_surface_count()):
			var material := mesh_instance.mesh.surface_get_material(surface)
			if material is BaseMaterial3D:
				var base := material as BaseMaterial3D
				var texture_name := ""
				if base.albedo_texture != null:
					texture_name = base.albedo_texture.resource_path
				print(
					"surface=", surface,
					" material=", material.resource_name,
					" transparency=", base.transparency,
					" texture=", texture_name,
				)
			else:
				print("surface=", surface, " material=", material)
	get_tree().quit(0)
