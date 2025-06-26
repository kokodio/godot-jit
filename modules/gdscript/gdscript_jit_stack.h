#pragma once
#include "core/templates/hash_map.h"
#include "gdscript_jit_base.h"

static uint32_t next_type_id() {
	static uint32_t counter = 0;
	return ++counter;
}

template <typename T>
static uint32_t type_id() {
	static uint32_t id = next_type_id();
	return id;
}

static inline std::size_t alignUp(std::size_t sz, std::size_t granularity) {
	return (sz + granularity - 1) & ~(granularity - 1);
}

class StackManager {
public:
	StackManager(Compiler *compiler) {
		this->cc = compiler;
	}

	template <typename T>
	constexpr Mem alloc() {
		auto ti = type_id<T>();
		if (memMap.has(ti)) {
			return memMap[ti];
		}

		constexpr std::size_t alignT = alignof(T);
		constexpr std::size_t sizeT = sizeof(T);

		std::size_t offset = alignUp(stackSize, alignT);
		stackSize = offset + sizeT;

		if (!isStackInitialized) {
			stackMem = cc->newStack(stackSize, 16);
			isStackInitialized = true;
		} else {
			cc->setStackSize(stackMem, stackSize, 16);
		}

		Mem mem = stackMem.cloneAdjusted(offset);

		memMap[ti] = mem;
		return mem;
	}

private:
	Compiler *cc;
	Mem stackMem;
	std::size_t stackSize = 0;
	bool isStackInitialized = false;
	HashMap<uint32_t, Mem> memMap;

public:
	Mem allocArg(std::size_t argCount) {
		argSize = argSize + argCount * PTR_SIZE;

		if (!isArgInitialized) {
			argMem = cc->newStack(argSize, 16);
			isArgInitialized = true;
		} else {
			cc->setStackSize(argMem, argSize, 16);
		}

		return argMem;
	}

private:
	Mem argMem;
	std::size_t argSize = 0;
	bool isArgInitialized = false;
};
