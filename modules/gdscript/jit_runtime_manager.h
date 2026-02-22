#pragma once

#include "core/object/object.h"
#include "core/variant/variant.h"
#include "gdscript_codegen.h"
#include "gdscript_function.h"

#include <asmjit/core.h>
#include <asmjit/ujit.h>

enum class IROp : uint8_t {
	LoadParam,
	AddI64,
	Ret,
	StoreI64
};

struct ValueId { uint32_t id; };

struct IRInst {
	IROp op;
	ValueId dst;
	Vector<ValueId> args;
	GDScriptCodeGenerator::Address mem_loc;
	uint64_t imm = 0;
};

struct IRBuilder {
	uint32_t nextValueId = 0;
	Vector<IRInst> code;

	ValueId newValue() {
		return ValueId{nextValueId++};
	}

	ValueId emitLoad(GDScriptCodeGenerator::Address m) {
		const ValueId v = newValue();
		code.push_back({IROp::LoadParam, v, {}, m});
		return v;
	}

	ValueId emitAdd(ValueId a, ValueId b) {
		const ValueId v = newValue();
		code.push_back({IROp::AddI64, v,{a, b}});
		return v;
	}

	void emitStore(GDScriptCodeGenerator::Address m, ValueId v) {
		code.push_back({IROp::StoreI64, {}, {v}, m});
	}
};

class JitRuntimeManager {
public:
	struct AddressInfo {
		int encoded = 0;
		int type = 0;
		int index = 0;
		Variant::Type variant_type = Variant::NIL;

		bool is_stack() const {
			return type == GDScriptFunction::ADDR_TYPE_STACK;
		}

		bool is_constant() const {
			return type == GDScriptFunction::ADDR_TYPE_CONSTANT;
		}

		bool is_member() const {
			return type == GDScriptFunction::ADDR_TYPE_MEMBER;
		}
	};

private:
	static JitRuntimeManager *singleton;

	asmjit::JitRuntime runtime;
	asmjit::CodeHolder code;
	asmjit::ujit::BackendCompiler cc;
	asmjit::ujit::UniCompiler pc;
	asmjit::FileLogger logger;

	GDScriptFunction *function = nullptr;
	int code_size = 0;
	int ip = 0;

	asmjit::ujit::Gp result_ptr;
	asmjit::ujit::Gp stack_ptr;
	asmjit::ujit::Gp members_ptr;
	asmjit::ujit::Gp constants_ptr;

	JitRuntimeManager();

public:
	static JitRuntimeManager *get_singleton();
	static void create_singleton();
	static void destroy_singleton();
	void compile(IRBuilder ir, GDScriptFunction *func, int max_locals);
	void reset();
};