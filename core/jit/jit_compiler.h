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

#pragma once

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
	asmjit::x86::Gp result_ptr;
	asmjit::x86::Gp shared_call_error_ptr;
	asmjit::x86::Compiler *cc;
};

class JitCompiler : public Object {
	GDCLASS(JitCompiler, Object);

private:
	static HashMap<Variant::ValidatedOperatorEvaluator, String> op_map;
	static JitCompiler *singleton;
	asmjit::JitRuntime runtime;

	HashMap<int, asmjit::Label> analyze_jump_targets(JitContext &context);

	void print_address_info(const GDScriptFunction *gdscript, int encoded_address);
	void decode_address(int encoded_address, int &address_type, int &address_index);
	String get_address_type_name(int address_type);
	String get_operator_name_from_function(Variant::ValidatedOperatorEvaluator op_func);
	void handle_int_operation(String &operation_name, JitContext &context, asmjit::x86::Gp &left_val, asmjit::x86::Gp &right_val, asmjit::x86::Gp &result_mem);
	void handle_float_operation(String &operation_name, JitContext &ctx, int left_addr, int right_addr, int result_addr);
	void copy_variant(JitContext &context, asmjit::x86::Gp &dst_ptr, asmjit::x86::Gp &src_ptr);
	asmjit::x86::Gp extract_int_from_variant(JitContext &context, int address);
	void extract_float_from_variant(JitContext &context, asmjit::x86::Xmm &result_reg, int address);
	void extract_type_from_variant(JitContext &context, asmjit::x86::Gp &result_reg, int address);
	void store_reg_to_variant(JitContext &context, asmjit::x86::Gp &value, int address);
	void store_int_to_variant(JitContext &context, int value, int address);
	void store_float_to_variant(JitContext &context, asmjit::x86::Xmm &value, int address);
	void convert_int_to_float(JitContext &context, asmjit::x86::Gp &int_reg, asmjit::x86::Xmm &float_reg);

	void cast_and_store(JitContext &context, asmjit::x86::Gp &src_ptr, asmjit::x86::Gp &dst_ptr, Variant::Type expected_type, int return_addr);

	asmjit::x86::Gp create_call_error(JitContext &context);
	asmjit::x86::Gp get_call_error_ptr(JitContext &context, bool reset = true);
	asmjit::x86::Gp prepare_args_array(JitContext &context, int argc, int ip_base);
	asmjit::x86::Gp get_variant_ptr(JitContext &context, int address);

public:
	static constexpr int STACK_SLOT_SIZE = sizeof(Variant);

	static constexpr int OFFSET_DATA = offsetof(Variant, _data);
	static constexpr int OFFSET_INT_IN_DATA = offsetof(decltype(Variant::_data), _int);
	static constexpr int OFFSET_INT = OFFSET_DATA + OFFSET_INT_IN_DATA;

	static constexpr int OFFSET_FLOAT_IN_DATA = offsetof(decltype(Variant::_data), _float);
	static constexpr int OFFSET_FLOAT = OFFSET_DATA + OFFSET_FLOAT_IN_DATA;

	static constexpr int OFFSET_BOOL_IN_DATA = offsetof(decltype(Variant::_data), _bool);
	static constexpr int OFFSET_BOOL = OFFSET_DATA + OFFSET_BOOL_IN_DATA;

	static constexpr int PTR_SIZE = sizeof(void *);

	static JitCompiler *get_singleton();
	asmjit::JitRuntime *get_runtime() { return &runtime; }

	void *compile_function(const GDScriptFunction *gdscript);
	void print_function_info(const GDScriptFunction *gdscript);
	void release_function(void *func_ptr);

	JitCompiler();
	~JitCompiler();
};