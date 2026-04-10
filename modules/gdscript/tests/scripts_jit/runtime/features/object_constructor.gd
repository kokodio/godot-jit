# GH-73213

@jit
func test():
	var object := Object.new() # Not `Object()`.
	print(object.get_class())
	object.free()
