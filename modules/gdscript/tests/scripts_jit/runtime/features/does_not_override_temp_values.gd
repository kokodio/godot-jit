# https://github.com/godotengine/godot/issues/71177

@jit
func test():
	builtin_method()
	builtin_method_static()
	print("done")

@jit
func builtin_method():
	var pba := PackedByteArray()
	@warning_ignore("return_value_discarded")
	pba.resize(1) # Built-in validated.

@jit
func builtin_method_static():
	var _pba := PackedByteArray()
	@warning_ignore("return_value_discarded")
	Vector2.from_angle(PI) # Static built-in validated.
