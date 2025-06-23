#include "jit_runtime_manager.h"

JitRuntimeManager *JitRuntimeManager::singleton = nullptr;

JitRuntimeManager::JitRuntimeManager() {}

JitRuntimeManager::~JitRuntimeManager() {
	destroy_singleton();
}

JitRuntimeManager *JitRuntimeManager::get_singleton() {
	if (!singleton) {
		create_singleton();
	}
	return singleton;
}

void JitRuntimeManager::create_singleton() {
	if (!singleton) {
		singleton = memnew(JitRuntimeManager);
		singleton->code.init(singleton->runtime.environment(), singleton->runtime.cpuFeatures());
	}
}

void JitRuntimeManager::destroy_singleton() {
	if (singleton) {
		memdelete(singleton);
		singleton = nullptr;
	}
}

void JitRuntimeManager::release_function(void *func_ptr) {
	if (func_ptr) {
		runtime.release(func_ptr);
	}
}