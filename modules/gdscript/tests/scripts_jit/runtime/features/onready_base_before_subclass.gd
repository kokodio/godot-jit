#GH-63329
class A extends Node:
	@onready var a := get_value("a")

	@jit
	func get_value(var_name: String) -> String:
		print(var_name)
		return var_name

class B extends A:
	@onready var b := get_value("b")

	@jit
	func _ready():
		pass

@jit
func test():
	var node := B.new()
	node._ready()
	node.free()
