#pragma once

#include "jit_runtime_manager.h"

enum class IRValueKind : uint8_t {
	GP64,
	F64,
};

struct IROptimizationResult {
	Vector<IRBlock> raw_blocks;
	Vector<IRBlock> optimized_blocks;
	HashMap<uint32_t, IRValueKind> value_kind_map;
};

IROptimizationResult optimize_ir(const IRBuilder &p_ir);
