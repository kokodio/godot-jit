#pragma once

#include "core/object/object.h"
#include "core/templates/hash_map.h"
#include "core/variant/variant.h"
#include "gdscript_function.h"

#include <asmjit/core.h>
#include <asmjit/ujit.h>

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

	HashMap<int, asmjit::Label> jump_labels;

	struct LoopCtx {
		asmjit::ujit::Gp to;
		asmjit::ujit::Gp step;
		asmjit::ujit::Gp count;
	} loop_ctx;

	JitRuntimeManager();

public:
	static JitRuntimeManager *get_singleton();
	static void create_singleton();
	static void destroy_singleton();

	void compile(GDScriptFunction *gd_function);
	void release_function(void *func_ptr);
	void reset();

	int position() const;
	bool has_next() const;
	GDScriptFunction::Opcode opcode() const;
	GDScriptFunction::Opcode peek(int positions_ahead);
	int operand(int offset) const;
	void next();
	int instruction_size();
	int get_instruction_size(int pos) const;

	AddressInfo decode_address(int offset) const;

private:
	void collect_jump_targets();
	void emit_opcode(GDScriptFunction::Opcode opcode);
	void emit_operator_validated();
	void emit_return();

	asmjit::ujit::Gp get_variant_ptr(const AddressInfo &p_address);
	asmjit::ujit::Mem get_variant_mem(const AddressInfo &p_address, int offset = 0) const;
	void copy_variant(const asmjit::ujit::Gp &dst_ptr, const asmjit::ujit::Gp &src_ptr);
	void copy_variant(const AddressInfo &dst, const AddressInfo &src);

	template <typename Fn>
	asmjit::InvokeNode *emit_invoke(Fn op_func, asmjit::FuncSignature const &sig);

	asmjit::ujit::Gp booleanize(const AddressInfo &p_address);
	void assign_type(const AddressInfo &dst, int type_value);
	void load64(const asmjit::ujit::Gp &dst, const AddressInfo &src, int offset);
	Variant::Type get_stack_type(int index) const;

	template <typename...  Args>
	void print_debug(Args...  p_args);
};