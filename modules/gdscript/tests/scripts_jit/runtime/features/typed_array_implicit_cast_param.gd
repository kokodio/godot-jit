# GH-93990

@jit
func test_param(array: Array[String]) -> void:
	print(array.get_typed_builtin() == TYPE_STRING)

@jit
func test() -> void:
	test_param(PackedStringArray())
