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

#if defined(__x86_64) || defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
#include <asmjit/x86.h>
namespace Arch {
using namespace ::asmjit::x86;
}
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(ARM64_ENABLED)
#include <asmjit/a64.h>
namespace Arch {
using namespace ::asmjit::a64;
}
#endif

using Gp = Arch::Gp;
using Vec = Arch::Vec;
using Mem = Arch::Mem;
using Compiler = Arch::Compiler;
using CondCode = Arch::CondCode;

struct OpInfo {
	Variant::Operator op;
	Variant::Type left_type;
	Variant::Type right_type;
};

struct JitContext {
	const GDScriptFunction *gdscript;
	Gp args_ptr;
	Gp result_ptr;
	Gp stack_ptr;
	Gp constants_ptr;
	Gp members_ptr;
	Gp call_error_ptr;
	Gp bool_ptr;
	Gp operator_ptr;
	Compiler *cc;
};

struct FunctionAnalysis {
	bool uses_bool = false;
	bool uses_error = false;
	bool uses_operator = false;
	HashMap<int, asmjit::Label> jump_labels;
};

class JitCompiler : public Object {
	GDCLASS(JitCompiler, Object);

private:
	static HashMap<intptr_t, OpInfo> op_map;
	static JitCompiler *singleton;
	asmjit::JitRuntime runtime;

	FunctionAnalysis analyze_function(JitContext &ctx);
	void initialize_context(JitContext &ctx, const FunctionAnalysis &analysis);

	void print_address_info(const GDScriptFunction *gdscript, int encoded_address);
	void decode_address(int encoded_address, int &address_type, int &address_index);
	String get_address_type_name(int address_type);
	void handle_int_operation(const OpInfo operation, JitContext &ctx, int left_addr, int right_addr, int result_addr);
	void handle_float_operation(const OpInfo operation, JitContext &ctx, int left_addr, int right_addr, int result_addr);
	void handle_vector2_operation(const OpInfo operation, JitContext &ctx, int left_addr, int right_addr, int result_addr);
	void copy_variant(JitContext &ctx, Gp &dst_ptr, Gp &src_ptr);
	Gp extract_int_from_variant(JitContext &ctx, int address);
	void extract_float_from_variant(JitContext &ctx, Vec &result_reg, int address);
	void extract_type_from_variant(JitContext &ctx, Gp &result_reg, int address);
	void store_int_to_variant(JitContext &ctx, int value, int address);
	void store_float_to_variant(JitContext &ctx, Vec &value, int address);
	void store_vector2_to_variant(JitContext &ctx, Vec &x_reg, Vec &y_reg, int address);

	void cast_and_store(JitContext &ctx, Gp &src_ptr, Gp &dst_ptr, Variant::Type expected_type, int return_addr);

	Gp get_call_error_ptr(JitContext &ctx, bool reset = true);
	Gp get_bool_ptr(JitContext &ctx, bool value);
	Gp prepare_args_array(JitContext &ctx, int argc, int ip_base);
	Gp get_variant_ptr(JitContext &ctx, int address);
	Mem get_variant_mem(const JitContext &ctx, int address, int offset_field);
	Mem get_variant_type_mem(const JitContext &ctx, int address);
	Mem get_int_mem_ptr(JitContext &ctx, int address);

	void register_op(Variant::Operator op, Variant::Type left_type, Variant::Type right_type);
	OpInfo get_operator_info(intptr_t op_func);

	static inline Mem mem_qword_ptr(const Gp &base, int disp = 0);
	static inline Mem mem_dword_ptr(const Gp &base, int disp = 0);
	static inline Mem mem_byte_ptr(const Gp &base, int disp = 0);
	void gen_compare_int(JitContext &ctx, Gp &lhs, Mem &rhs, int result_addr, CondCode cc);
	void gen_compare_float(JitContext &ctx, Vec &lhs, Vec &rhs, int result_addr, CondCode cc);

public:
	static constexpr int STACK_SLOT_SIZE = sizeof(Variant);

	static constexpr int OFFSET_DATA = offsetof(Variant, _data);
	static constexpr int OFFSET_INT_IN_DATA = offsetof(decltype(Variant::_data), _int);
	static constexpr int OFFSET_INT = OFFSET_DATA + OFFSET_INT_IN_DATA;

	static constexpr int OFFSET_FLOAT_IN_DATA = offsetof(decltype(Variant::_data), _float);
	static constexpr int OFFSET_FLOAT = OFFSET_DATA + OFFSET_FLOAT_IN_DATA;

	static constexpr int OFFSET_BOOL_IN_DATA = offsetof(decltype(Variant::_data), _bool);
	static constexpr int OFFSET_BOOL = OFFSET_DATA + OFFSET_BOOL_IN_DATA;

	static constexpr int OFFSET_MEM_IN_DATA = offsetof(decltype(Variant::_data), _mem);
	static constexpr int OFFSET_MEM = OFFSET_DATA + OFFSET_MEM_IN_DATA;

	static constexpr int OFFSET_VECTOR2_X = OFFSET_MEM + offsetof(Vector2, x);
	static constexpr int OFFSET_VECTOR2_Y = OFFSET_MEM + offsetof(Vector2, y);

	static constexpr int PTR_SIZE = sizeof(void *);

	static JitCompiler *get_singleton();
	asmjit::JitRuntime *get_runtime() { return &runtime; }

	void *compile_function(const GDScriptFunction *gdscript);
	void print_function_info(const GDScriptFunction *gdscript);
	void release_function(void *func_ptr);

	JitCompiler();
	~JitCompiler();
};