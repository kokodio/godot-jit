class Foo extends Node:
	@jit
	func _init():
		name = 'f'
		var string: String = name
		Utils.check(typeof(string) == TYPE_STRING)
		Utils.check(string == 'f')
		print('ok')

@jit
func test():
	var foo := Foo.new()
	foo.free()
