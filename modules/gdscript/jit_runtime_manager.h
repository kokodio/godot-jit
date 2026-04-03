#pragma once

#include "gdscript_codegen.h"
#include "gdscript_function.h"

#include <asmjit/core.h>
#include <asmjit/ujit.h>

#define IROP_LIST(X) \
	X(LoadParam)     \
	X(LoadF64)       \
	X(LoadRealMemberF64) \
	X(StoreRealMemberF64) \
	X(LoadPtr)       \
	X(ZeroI64)       \
	X(AddI64)        \
	X(AddF64)        \
	X(MulI64)        \
	X(MulF64)        \
	X(SubI64)        \
	X(SubF64)        \
	X(DivI64)        \
	X(EqI64)         \
	X(EqF64)         \
	X(NeI64)         \
	X(NeF64)         \
	X(LtI64)         \
	X(LtF64)         \
	X(LeI64)         \
	X(LeF64)         \
	X(GtI64)         \
	X(GtF64)         \
	X(GeI64)         \
	X(GeF64)         \
	X(Assign)        \
	X(AssignNull)    \
	X(AssignTrue)    \
	X(AssignFalse)   \
	X(AssignTypedBuiltin) \
	X(AssignTypedNative) \
	X(AssignTypedScript) \
	X(AssignTypedArray) \
	X(AssignTypedDictionary) \
	X(Construct)     \
	X(ConstructValidated) \
	X(ConstructArray) \
	X(ConstructDictionary) \
	X(ConstructTypedDictionary) \
	X(GetNamed)      \
	X(SetNamed)      \
	X(GetNamedValidated) \
	X(SetNamedValidated) \
	X(GetMember)     \
	X(SetMember)     \
	X(GetKeyed)      \
	X(SetKeyed)      \
	X(GetIndexedValidated) \
	X(SetIndexedValidated) \
	X(GetKeyedValidated) \
	X(SetKeyedValidated) \
	X(ConstructTypedArray) \
	X(Call) \
	X(CallSelf) \
	X(CallUtility) \
	X(CallBuiltinValidated) \
	X(CallMethodBindValidated) \
	X(CallUtilityValidated) \
	X(CallBinOp)     \
	X(CallOperator)  \
	X(IterateBegin)  \
	X(Iterate)       \
	X(Booleanize)    \
	X(Jump)          \
	X(JumpCc)        \
	X(Ret)           \
	X(RetTypedArray) \
	X(RetTypedDictionary) \
	X(StoreI64)      \
	X(StoreF64)      \
	X(StoreType)

enum class IROp : uint8_t {
#define X(name) name,
	IROP_LIST(X)
#undef X
};

constexpr const char* IROpNames[] = {
#define X(name) #name,
	IROP_LIST(X)
#undef X
};

struct ValueId { uint32_t id; };
struct LabelId { uint32_t id; };

enum class IRCond : uint8_t {
	EQ,
	NE,
	LT,
	LE,
	GT,
	GE,
};

struct IRInst {
	IROp op{};
	ValueId dst{};
	Vector<ValueId> args;
	GDScriptCodeGenerator::Address mem_loc;
	uint64_t imm = 0;
	uint64_t aux = 0;
};

struct IRBlock {
	uint32_t id = 0;
	bool has_label = false;
	LabelId label{};
	Vector<IRInst> code;
};

struct IRBuilder {
	uint32_t next_value_id{0};
	uint32_t next_label_id{0};
	uint32_t next_block_id{0};
	Vector<IRBlock> blocks;
	HashMap<uint32_t, int> label_to_block;
	int current_block = -1;

	IRBuilder() {
		_ensure_current_block();
	}

	ValueId new_value() {
		const ValueId v{ next_value_id++ };
		return v;
	}

	LabelId new_label() {
		const LabelId label{ next_label_id++ };
		return label;
	}

	int _create_block() {
		IRBlock block;
		block.id = next_block_id++;
		blocks.push_back(block);
		return blocks.size() - 1;
	}

	void _ensure_current_block() {
		if (current_block < 0) {
			current_block = _create_block();
		}
	}

	void _push_inst(const IRInst &inst) {
		_ensure_current_block();
		blocks.write[current_block].code.push_back(inst);
	}

	int _get_or_create_label_block(LabelId label) {
		if (const auto it = label_to_block.find(label.id); it != label_to_block.end()) {
			return it->value;
		}

		_ensure_current_block();
		int block_index = current_block;
		IRBlock &block = blocks.write[block_index];

		if (block.has_label || !block.code.is_empty()) {
			block_index = _create_block();
		}

		blocks.write[block_index].has_label = true;
		blocks.write[block_index].label = label;
		label_to_block[label.id] = block_index;
		return block_index;
	}

	ValueId emit_load(const GDScriptCodeGenerator::Address &m) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::LoadParam;
		inst.dst = v;
		inst.mem_loc = m;

		_push_inst(inst);
		return v;
	}

	ValueId emit_load_ptr(const GDScriptCodeGenerator::Address &m) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::LoadPtr;
		inst.dst = v;
		inst.mem_loc = m;

		_push_inst(inst);
		return v;
	}

	ValueId emit_iterate_begin(ValueId container_ptr, ValueId counter_ptr, ValueId iterator_ptr) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::IterateBegin;
		inst.dst = v;
		inst.args = { container_ptr, counter_ptr, iterator_ptr };

		_push_inst(inst);
		return v;
	}

	ValueId emit_iterate(ValueId container_ptr, ValueId counter_ptr, ValueId iterator_ptr) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::Iterate;
		inst.dst = v;
		inst.args = { container_ptr, counter_ptr, iterator_ptr };

		_push_inst(inst);
		return v;
	}

	ValueId emit_loadf64(const GDScriptCodeGenerator::Address &m) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::LoadF64;
		inst.dst = v;
		inst.mem_loc = m;

		_push_inst(inst);
		return v;
	}

	ValueId emit_load_real_member_f64(ValueId base_ptr, uint32_t byte_offset) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::LoadRealMemberF64;
		inst.dst = v;
		inst.args = { base_ptr };
		inst.imm = byte_offset;

		_push_inst(inst);
		return v;
	}

	ValueId emit_load_real_member_f64(const GDScriptCodeGenerator::Address &m, uint32_t byte_offset) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::LoadRealMemberF64;
		inst.dst = v;
		inst.mem_loc = m;
		inst.imm = byte_offset;

		_push_inst(inst);
		return v;
	}

	ValueId emit_add64(ValueId a, ValueId b) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::AddI64;
		inst.dst = v;
		inst.args = { a, b };

		_push_inst(inst);
		return v;
	}

	ValueId emit_zero64() {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::ZeroI64;
		inst.dst = v;

		_push_inst(inst);
		return v;
	}

	ValueId emit_add64(ValueId a, int64_t imm) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::AddI64;
		inst.dst = v;
		inst.args = { a };
		inst.imm = uint64_t(imm);

		_push_inst(inst);
		return v;
	}

	ValueId emit_addf64(ValueId a, ValueId b) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::AddF64;
		inst.dst = v;
		inst.args = { a, b };

		_push_inst(inst);
		return v;
	}

	ValueId emit_mul64(ValueId a, ValueId b) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::MulI64;
		inst.dst = v;
		inst.args = { a, b };

		_push_inst(inst);
		return v;
	}

	ValueId emit_mulf64(ValueId a, ValueId b) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::MulF64;
		inst.dst = v;
		inst.args = { a, b };

		_push_inst(inst);
		return v;
	}

	ValueId emit_sub64(ValueId a, ValueId b) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::SubI64;
		inst.dst = v;
		inst.args = { a, b };

		_push_inst(inst);
		return v;
	}

	ValueId emit_subf64(ValueId a, ValueId b) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::SubF64;
		inst.dst = v;
		inst.args = { a, b };

		_push_inst(inst);
		return v;
	}

	ValueId emit_div64(ValueId a, ValueId b) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::DivI64;
		inst.dst = v;
		inst.args = { a, b };

		_push_inst(inst);
		return v;
	}

	ValueId emit_eq64(ValueId a, ValueId b) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::EqI64;
		inst.dst = v;
		inst.args = { a, b };

		_push_inst(inst);
		return v;
	}

	ValueId emit_eqf64(ValueId a, ValueId b) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::EqF64;
		inst.dst = v;
		inst.args = { a, b };

		_push_inst(inst);
		return v;
	}

	ValueId emit_ne64(ValueId a, ValueId b) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::NeI64;
		inst.dst = v;
		inst.args = { a, b };

		_push_inst(inst);
		return v;
	}

	ValueId emit_nef64(ValueId a, ValueId b) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::NeF64;
		inst.dst = v;
		inst.args = { a, b };

		_push_inst(inst);
		return v;
	}

	ValueId emit_lt64(ValueId a, ValueId b) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::LtI64;
		inst.dst = v;
		inst.args = { a, b };

		_push_inst(inst);
		return v;
	}

	ValueId emit_ltf64(ValueId a, ValueId b) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::LtF64;
		inst.dst = v;
		inst.args = { a, b };

		_push_inst(inst);
		return v;
	}

	ValueId emit_le64(ValueId a, ValueId b) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::LeI64;
		inst.dst = v;
		inst.args = { a, b };

		_push_inst(inst);
		return v;
	}

	ValueId emit_lef64(ValueId a, ValueId b) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::LeF64;
		inst.dst = v;
		inst.args = { a, b };

		_push_inst(inst);
		return v;
	}

	ValueId emit_gt64(ValueId a, ValueId b) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::GtI64;
		inst.dst = v;
		inst.args = { a, b };

		_push_inst(inst);
		return v;
	}

	ValueId emit_gtf64(ValueId a, ValueId b) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::GtF64;
		inst.dst = v;
		inst.args = { a, b };

		_push_inst(inst);
		return v;
	}

	ValueId emit_ge64(ValueId a, ValueId b) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::GeI64;
		inst.dst = v;
		inst.args = { a, b };

		_push_inst(inst);
		return v;
	}

	ValueId emit_gef64(ValueId a, ValueId b) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::GeF64;
		inst.dst = v;
		inst.args = { a, b };

		_push_inst(inst);
		return v;
	}

	void emit_assign(ValueId dst_ptr, ValueId src_ptr, uint32_t type = Variant::NIL) {
		IRInst inst;
		inst.op = IROp::Assign;
		inst.args = { dst_ptr, src_ptr };
		inst.aux = type;

		_push_inst(inst);
	}

	void emit_assign_null(ValueId dst_ptr) {
		IRInst inst;
		inst.op = IROp::AssignNull;
		inst.args = { dst_ptr };

		_push_inst(inst);
	}

	void emit_assign_true(ValueId dst_ptr) {
		IRInst inst;
		inst.op = IROp::AssignTrue;
		inst.args = { dst_ptr };

		_push_inst(inst);
	}

	void emit_assign_false(ValueId dst_ptr) {
		IRInst inst;
		inst.op = IROp::AssignFalse;
		inst.args = { dst_ptr };

		_push_inst(inst);
	}

	void emit_assign_typed_builtin(ValueId dst_ptr, ValueId src_ptr, Variant::Type builtin_type) {
		IRInst inst;
		inst.op = IROp::AssignTypedBuiltin;
		inst.args = { dst_ptr, src_ptr };
		inst.imm = builtin_type;

		_push_inst(inst);
	}

	void emit_assign_typed_native(ValueId dst_ptr, ValueId src_ptr, ValueId type_ptr) {
		IRInst inst;
		inst.op = IROp::AssignTypedNative;
		inst.args = { dst_ptr, src_ptr, type_ptr };

		_push_inst(inst);
	}

	void emit_assign_typed_script(ValueId dst_ptr, ValueId src_ptr, ValueId type_ptr) {
		IRInst inst;
		inst.op = IROp::AssignTypedScript;
		inst.args = { dst_ptr, src_ptr, type_ptr };

		_push_inst(inst);
	}

	void emit_get_named(ValueId src_ptr, ValueId dst_ptr, int name_index) {
		IRInst inst;
		inst.op = IROp::GetNamed;
		inst.args = { src_ptr, dst_ptr };
		inst.imm = name_index;

		_push_inst(inst);
	}

	void emit_set_named(ValueId dst_ptr, ValueId src_ptr, int name_index) {
		IRInst inst;
		inst.op = IROp::SetNamed;
		inst.args = { dst_ptr, src_ptr };
		inst.imm = name_index;

		_push_inst(inst);
	}

	void emit_get_named_validated(ValueId src_ptr, ValueId dst_ptr, Variant::ValidatedGetter getter) {
		IRInst inst;
		inst.op = IROp::GetNamedValidated;
		inst.args = { src_ptr, dst_ptr };
		inst.imm = uint64_t(getter);

		_push_inst(inst);
	}

	void emit_set_named_validated(ValueId dst_ptr, ValueId src_ptr, Variant::ValidatedSetter setter) {
		IRInst inst;
		inst.op = IROp::SetNamedValidated;
		inst.args = { dst_ptr, src_ptr };
		inst.imm = uint64_t(setter);

		_push_inst(inst);
	}

	void emit_get_member(ValueId dst_ptr, int name_index) {
		IRInst inst;
		inst.op = IROp::GetMember;
		inst.args = { dst_ptr };
		inst.imm = name_index;

		_push_inst(inst);
	}

	void emit_set_member(ValueId src_ptr, int name_index) {
		IRInst inst;
		inst.op = IROp::SetMember;
		inst.args = { src_ptr };
		inst.imm = name_index;

		_push_inst(inst);
	}

	void emit_get_keyed(ValueId src_ptr, ValueId key_ptr, ValueId dst_ptr) {
		IRInst inst;
		inst.op = IROp::GetKeyed;
		inst.args = { src_ptr, key_ptr, dst_ptr };

		_push_inst(inst);
	}

	void emit_set_keyed(ValueId dst_ptr, ValueId key_ptr, ValueId src_ptr) {
		IRInst inst;
		inst.op = IROp::SetKeyed;
		inst.args = { dst_ptr, key_ptr, src_ptr };

		_push_inst(inst);
	}

	void emit_get_indexed_validated(ValueId src_ptr, ValueId index_ptr, ValueId dst_ptr, Variant::ValidatedIndexedGetter getter) {
		IRInst inst;
		inst.op = IROp::GetIndexedValidated;
		inst.args = { src_ptr, index_ptr, dst_ptr };
		inst.imm = uint64_t(getter);

		_push_inst(inst);
	}

	void emit_set_indexed_validated(ValueId dst_ptr, ValueId index_ptr, ValueId src_ptr, Variant::ValidatedIndexedSetter setter) {
		IRInst inst;
		inst.op = IROp::SetIndexedValidated;
		inst.args = { dst_ptr, index_ptr, src_ptr };
		inst.imm = uint64_t(setter);

		_push_inst(inst);
	}

	void emit_get_keyed_validated(ValueId src_ptr, ValueId key_ptr, ValueId dst_ptr, Variant::ValidatedKeyedGetter getter) {
		IRInst inst;
		inst.op = IROp::GetKeyedValidated;
		inst.args = { src_ptr, key_ptr, dst_ptr };
		inst.imm = uint64_t(getter);

		_push_inst(inst);
	}

	void emit_set_keyed_validated(ValueId dst_ptr, ValueId key_ptr, ValueId src_ptr, Variant::ValidatedKeyedSetter setter) {
		IRInst inst;
		inst.op = IROp::SetKeyedValidated;
		inst.args = { dst_ptr, key_ptr, src_ptr };
		inst.imm = uint64_t(setter);

		_push_inst(inst);
	}

	void emit_construct_typed_array(const Vector<ValueId> &arg_ptrs, ValueId dst_ptr, ValueId script_type_ptr, Variant::Type builtin_type, int native_type_index) {
		IRInst inst;
		inst.op = IROp::ConstructTypedArray;
		inst.args = arg_ptrs;
		inst.args.push_back(dst_ptr);
		inst.args.push_back(script_type_ptr);
		inst.imm = uint64_t(builtin_type);
		inst.aux = uint64_t(native_type_index);

		_push_inst(inst);
	}

	void emit_assign_typed_array(ValueId dst_ptr, ValueId src_ptr, ValueId script_type_ptr, Variant::Type builtin_type, int native_type_index) {
		IRInst inst;
		inst.op = IROp::AssignTypedArray;
		inst.args = { dst_ptr, src_ptr, script_type_ptr };
		inst.imm = uint64_t(builtin_type);
		inst.aux = uint64_t(native_type_index);

		_push_inst(inst);
	}

	void emit_assign_typed_dictionary(ValueId dst_ptr, ValueId src_ptr, ValueId key_script_type_ptr, ValueId value_script_type_ptr, Variant::Type key_builtin_type, int key_native_type_index, Variant::Type value_builtin_type, int value_native_type_index) {
		IRInst inst;
		inst.op = IROp::AssignTypedDictionary;
		inst.args = { dst_ptr, src_ptr, key_script_type_ptr, value_script_type_ptr };
		inst.imm = uint64_t(key_builtin_type) | (uint64_t(uint32_t(value_builtin_type)) << 32);
		inst.aux = uint64_t(uint32_t(key_native_type_index)) | (uint64_t(uint32_t(value_native_type_index)) << 32);

		_push_inst(inst);
	}

	void emit_call(ValueId base_ptr, const Vector<ValueId> &arg_ptrs, ValueId dst_ptr, int method_name_index) {
		IRInst inst;
		inst.op = IROp::Call;
		inst.args = arg_ptrs;
		inst.args.push_back(base_ptr);
		inst.args.push_back(dst_ptr);
		inst.imm = uint64_t(method_name_index);

		_push_inst(inst);
	}

	void emit_call_self(const Vector<ValueId> &arg_ptrs, ValueId dst_ptr, int method_name_index) {
		IRInst inst;
		inst.op = IROp::CallSelf;
		inst.args = arg_ptrs;
		inst.args.push_back(dst_ptr);
		inst.imm = uint64_t(method_name_index);

		_push_inst(inst);
	}

	void emit_call_utility(const Vector<ValueId> &arg_ptrs, ValueId dst_ptr, int function_name_index) {
		IRInst inst;
		inst.op = IROp::CallUtility;
		inst.args = arg_ptrs;
		inst.args.push_back(dst_ptr);
		inst.imm = uint64_t(function_name_index);

		_push_inst(inst);
	}

	void emit_construct(const Vector<ValueId> &arg_ptrs, ValueId dst_ptr, Variant::Type type) {
		IRInst inst;
		inst.op = IROp::Construct;
		inst.args = arg_ptrs;
		inst.args.push_back(dst_ptr);
		inst.imm = uint64_t(type);

		_push_inst(inst);
	}

	void emit_construct_validated(const Vector<ValueId> &arg_ptrs, ValueId dst_ptr, Variant::ValidatedConstructor constructor) {
		IRInst inst;
		inst.op = IROp::ConstructValidated;
		inst.args = arg_ptrs;
		inst.args.push_back(dst_ptr);
		inst.imm = uint64_t(constructor);

		_push_inst(inst);
	}

	void emit_construct_array(const Vector<ValueId> &arg_ptrs, ValueId dst_ptr) {
		IRInst inst;
		inst.op = IROp::ConstructArray;
		inst.args = arg_ptrs;
		inst.args.push_back(dst_ptr);

		_push_inst(inst);
	}

	void emit_construct_dictionary(const Vector<ValueId> &arg_ptrs, ValueId dst_ptr) {
		IRInst inst;
		inst.op = IROp::ConstructDictionary;
		inst.args = arg_ptrs;
		inst.args.push_back(dst_ptr);

		_push_inst(inst);
	}

	void emit_construct_typed_dictionary(const Vector<ValueId> &arg_ptrs, ValueId dst_ptr, ValueId key_script_type_ptr, ValueId value_script_type_ptr, Variant::Type key_builtin_type, int key_native_type_index, Variant::Type value_builtin_type, int value_native_type_index) {
		IRInst inst;
		inst.op = IROp::ConstructTypedDictionary;
		inst.args = arg_ptrs;
		inst.args.push_back(dst_ptr);
		inst.args.push_back(key_script_type_ptr);
		inst.args.push_back(value_script_type_ptr);
		inst.imm = uint64_t(key_builtin_type) | (uint64_t(uint32_t(value_builtin_type)) << 32);
		inst.aux = uint64_t(uint32_t(key_native_type_index)) | (uint64_t(uint32_t(value_native_type_index)) << 32);

		_push_inst(inst);
	}

	void emit_call_utility_validated(const Vector<ValueId> &arg_ptrs, ValueId dst_ptr, Variant::ValidatedUtilityFunction function) {
		IRInst inst;
		inst.op = IROp::CallUtilityValidated;
		inst.args = arg_ptrs;
		inst.args.push_back(dst_ptr);
		inst.imm = uint64_t(function);

		_push_inst(inst);
	}

	void emit_call_builtin_validated(ValueId base_ptr, const Vector<ValueId> &arg_ptrs, ValueId dst_ptr, Variant::ValidatedBuiltInMethod method) {
		IRInst inst;
		inst.op = IROp::CallBuiltinValidated;
		inst.args = arg_ptrs;
		inst.args.push_back(base_ptr);
		inst.args.push_back(dst_ptr);
		inst.imm = uint64_t(method);

		_push_inst(inst);
	}

	void emit_call_method_bind_validated(ValueId base_ptr, const Vector<ValueId> &arg_ptrs, ValueId dst_ptr, MethodBind *method) {
		IRInst inst;
		inst.op = IROp::CallMethodBindValidated;
		inst.args = arg_ptrs;
		inst.args.push_back(base_ptr);
		inst.args.push_back(dst_ptr);
		inst.imm = uint64_t(method);

		_push_inst(inst);
	}

	template <typename Fn>
	void emit_call_binop(const Vector<ValueId> &args, Fn func) {
		IRInst inst;
		inst.op = IROp::CallBinOp;
		inst.args = args;
		inst.imm = (uint64_t)(func);

		_push_inst(inst);
	}

	void emit_call_operator(ValueId left_ptr, ValueId right_ptr, ValueId dst_ptr, Variant::Operator op) {
		IRInst inst;
		inst.op = IROp::CallOperator;
		inst.args = { left_ptr, right_ptr, dst_ptr };
		inst.imm = uint64_t(op);

		_push_inst(inst);
	}

	ValueId emit_booleanize(ValueId ptr, uint32_t type = Variant::NIL) {
		const ValueId v = new_value();

		IRInst inst;
		inst.op = IROp::Booleanize;
		inst.dst = v;
		inst.args = { ptr };
		inst.imm = type;

		_push_inst(inst);
		return v;
	}

	void bind_label(LabelId label) {
		current_block = _get_or_create_label_block(label);
	}

	void emit_jump(LabelId label) {
		IRInst inst;
		inst.op = IROp::Jump;
		inst.imm = label.id;

		_push_inst(inst);
	}

	void emit_jump_cc(IRCond cond, ValueId value, LabelId label) {
		IRInst inst;
		inst.op = IROp::JumpCc;
		inst.args = { value };
		inst.imm = label.id;
		inst.aux = uint64_t(cond);

		_push_inst(inst);
	}

	void emit_jump_cc(IRCond cond, ValueId a, ValueId b, LabelId label) {
		IRInst inst;
		inst.op = IROp::JumpCc;
		inst.args = { a, b };
		inst.imm = label.id;
		inst.aux = uint64_t(cond);

		_push_inst(inst);
	}

	void emit_jump_if_zero(ValueId value, LabelId label) {
		emit_jump_cc(IRCond::EQ, value, label);
	}

	void emit_jump_if_ge_zero(ValueId value, LabelId label) {
		emit_jump_cc(IRCond::GE, value, label);
	}

	void emit_store(const GDScriptCodeGenerator::Address &m, ValueId v) {
		IRInst inst;
		inst.op = IROp::StoreI64;
		inst.args = { v };
		inst.mem_loc = m;

		_push_inst(inst);
	}

	void emit_storef64(const GDScriptCodeGenerator::Address &m, ValueId v) {
		IRInst inst;
		inst.op = IROp::StoreF64;
		inst.args = { v };
		inst.mem_loc = m;

		_push_inst(inst);
	}

	void emit_store_real_member_f64(ValueId base_ptr, uint32_t byte_offset, ValueId value) {
		IRInst inst;
		inst.op = IROp::StoreRealMemberF64;
		inst.args = { base_ptr, value };
		inst.imm = byte_offset;

		_push_inst(inst);
	}

	void emit_store_real_member_f64(const GDScriptCodeGenerator::Address &m, uint32_t byte_offset, ValueId value) {
		IRInst inst;
		inst.op = IROp::StoreRealMemberF64;
		inst.args = { value };
		inst.mem_loc = m;
		inst.imm = byte_offset;

		_push_inst(inst);
	}

	void emit_store_type(const GDScriptCodeGenerator::Address &m, uint32_t type) {
		IRInst inst;
		inst.op = IROp::StoreType;
		inst.mem_loc = m;
		inst.imm = type;

		_push_inst(inst);
	}

	void emit_return(ValueId v) {
		IRInst inst;
		inst.op = IROp::Ret;
		inst.args = { v };

		_push_inst(inst);
	}

	void emit_return_typed_array(ValueId src_ptr, ValueId script_type_ptr, Variant::Type builtin_type, int native_type_index) {
		IRInst inst;
		inst.op = IROp::RetTypedArray;
		inst.args = { src_ptr, script_type_ptr };
		inst.imm = uint64_t(builtin_type);
		inst.aux = uint64_t(native_type_index);

		_push_inst(inst);
	}

	void emit_return_typed_dictionary(ValueId src_ptr, ValueId key_script_type_ptr, ValueId value_script_type_ptr, Variant::Type key_builtin_type, int key_native_type_index, Variant::Type value_builtin_type, int value_native_type_index) {
		IRInst inst;
		inst.op = IROp::RetTypedDictionary;
		inst.args = { src_ptr, key_script_type_ptr, value_script_type_ptr };
		inst.imm = uint64_t(key_builtin_type) | (uint64_t(uint32_t(value_builtin_type)) << 32);
		inst.aux = uint64_t(uint32_t(key_native_type_index)) | (uint64_t(uint32_t(value_native_type_index)) << 32);

		_push_inst(inst);
	}
};

class JitRuntimeManager {
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
	struct AddressInfo {
		int address_type; // ADDR_TYPE_STACK, ADDR_TYPE_CONSTANT, ADDR_TYPE_MEMBER
		int address_index;
	};

	static JitRuntimeManager *get_singleton();
	static void create_singleton();
	static void destroy_singleton();
	void compile(IRBuilder ir, GDScriptFunction *func, int max_locals);
	void reset();

	static AddressInfo decode_address_index(int encoded_address);
	static int encode_address_index(int address_type, int address_index);
	static void print_address_info(const GDScriptCodeGenerator::Address &p_address);

	template <typename Fn>
	asmjit::InvokeNode *emit_invoke(Fn op_func, asmjit::FuncSignature const &sig);
};
