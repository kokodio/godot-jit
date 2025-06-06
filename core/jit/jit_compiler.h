/**************************************************************************/
/*  jit_compiler.h                                                        */
/**************************************************************************/
/*                         This file is part of:                         */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#ifndef JIT_COMPILER_H
#define JIT_COMPILER_H

#include "core/object/object.h"
#include "core/os/memory.h"
#include "modules/gdscript/gdscript.h"
#include "modules/gdscript/gdscript_function.h"

#include <asmjit/core.h>
#include <asmjit/x86.h>

struct JitContext {
	const GDScriptFunction *gdscript;
	asmjit::x86::Gp stack_ptr;
	asmjit::x86::Gp members_ptr;
	asmjit::x86::Gp args_ptr;
	Vector<Variant::Type> stack_types;
	asmjit::x86::Compiler *cc;
	asmjit::x86::Gp result_ptr;
};

struct OperatorTypes {
	Variant::Operator op;
	Variant::Type left_type;
	Variant::Type right_type;
};

class JitCompiler : public Object {
	GDCLASS(JitCompiler, Object);

private:
	static HashMap<Variant::ValidatedOperatorEvaluator, String> op_map;
	static HashMap<Variant::ValidatedOperatorEvaluator, OperatorTypes> evaluator_to_types_map;
	static JitCompiler *singleton;
	asmjit::JitRuntime runtime;

	void print_address_info(const GDScriptFunction *gdscript, int encoded_address);
	void decode_address(int encoded_address, int &address_type, int &address_index);
	String get_address_type_name(int address_type);
	String get_operator_name_from_function(Variant::ValidatedOperatorEvaluator op_func);
	StringName get_utility_function_name(int utility_idx, const GDScriptFunction *gdscript);
	void get_variant_ptr(JitContext &context, asmjit::x86::Gp &variant_ptr, int address);
	void handle_operation(String &operation_name, JitContext &context, asmjit::x86::Gp &left_val, asmjit::x86::Gp &right_val, asmjit::x86::Gp &result_mem);
	HashMap<int, asmjit::Label> analyze_jump_targets(JitContext &context);

	OperatorTypes get_operator_types(Variant::ValidatedOperatorEvaluator op_func);
	Variant::Type get_result_type_for_operator(OperatorTypes types);
	void copy_variant(JitContext &context, asmjit::x86::Gp &dst_ptr, asmjit::x86::Gp &src_ptr);
	void extract_int_from_variant(JitContext &context, asmjit::x86::Gp &result_reg, int address);
	void extract_type_from_variant(JitContext &context, asmjit::x86::Gp &result_reg, int address);
	void store_reg_to_variant(JitContext &context, asmjit::x86::Gp &value, int address);
	void store_int_to_variant(JitContext &context, int value, int address);
	void cast_and_store(JitContext &context, asmjit::x86::Gp &src_ptr, asmjit::x86::Gp &dst_ptr, Variant::Type expected_type, int return_addr);

	asmjit::x86::Gp create_call_error(JitContext &context);

public:
	static constexpr size_t STACK_SLOT_SIZE = sizeof(Variant);
	static constexpr size_t MEMBER_OFFSET = offsetof(GDScriptInstance, members);

	static constexpr size_t OFFSET_DATA = offsetof(Variant, _data);
	static constexpr size_t OFFSET_INT_IN_DATA = offsetof(decltype(Variant::_data), _int);
	static constexpr size_t OFFSET_INT = OFFSET_DATA + OFFSET_INT_IN_DATA;

	static constexpr int VARIANT_TYPE_INT = Variant::INT;

	static JitCompiler *get_singleton();
	asmjit::JitRuntime *get_runtime() { return &runtime; }

	void *compile_function(const GDScriptFunction *gdscript);
	void print_function_info(const GDScriptFunction *gdscript);
	void release_function(void *func_ptr);

	JitCompiler();
	~JitCompiler();
};

#endif // JIT_COMPILER_H
