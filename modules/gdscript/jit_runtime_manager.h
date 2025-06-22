#pragma once

#include "core/object/object.h"
#include "core/templates/hash_map.h"
#include <asmjit/core.h>

class JitRuntimeManager {
private:
	static JitRuntimeManager *singleton;
	asmjit::JitRuntime runtime;

	JitRuntimeManager();

public:
	static JitRuntimeManager *get_singleton();
	static void create_singleton();
	static void destroy_singleton();

	~JitRuntimeManager();

	asmjit::JitRuntime &get_runtime() { return runtime; }

	void release_function(void *func_ptr);
};