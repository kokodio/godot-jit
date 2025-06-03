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
#include "modules/gdscript/gdscript_function.h"

#include <asmjit/core.h>
#include <asmjit/x86.h>

struct RangeInfo {
	asmjit::x86::Gp start_reg;
	asmjit::x86::Gp end_reg;
	asmjit::x86::Gp step_reg;
	int return_addr;
};

class JitCompiler : public Object {
	GDCLASS(JitCompiler, Object);

private:
	static HashMap<Variant::ValidatedOperatorEvaluator, String> op_map;
	static JitCompiler *singleton;
	asmjit::JitRuntime runtime;

	void print_address_info(const GDScriptFunction *gdscript, int encoded_address);
	asmjit::x86::Mem get_stack_slot(asmjit::x86::Gp &stack_ptr, int slot_index);
	void set_stack_slot(asmjit::x86::Compiler &cc, asmjit::x86::Gp &stack_ptr, int slot_index, int value);
	void decode_address(int encoded_address, int &address_type, int &address_index);
	String get_address_type_name(int address_type);
	String get_operator_name_from_function(Variant::ValidatedOperatorEvaluator op_func);
	void load_int(asmjit::x86::Compiler &cc, asmjit::x86::Gp &reg, asmjit::x86::Gp &stack_ptr, const GDScriptFunction *gdscript, int address);
	void handle_operation(String &operation_name, asmjit::x86::Compiler &cc, asmjit::x86::Gp &left_val, asmjit::x86::Gp &right_val, asmjit::x86::Mem &result_mem);
	HashMap<int, asmjit::Label> analyze_jump_targets(const GDScriptFunction *gdscript, asmjit::x86::Compiler &cc);
	RangeInfo handle_range_call(asmjit::x86::Compiler &cc, asmjit::x86::Gp &stack_ptr, const GDScriptFunction *gdscript, int argc, int ip);

public:
	static constexpr size_t STACK_SLOT_SIZE = sizeof(int);

	static JitCompiler *get_singleton();
	asmjit::JitRuntime *get_runtime() { return &runtime; }

	void *compile_function(const GDScriptFunction *gdscript);
	void print_function_info(const GDScriptFunction *gdscript);
	void extract_arguments(const GDScriptFunction *gdscript, asmjit::x86::Compiler &cc, asmjit::x86::Gp &args_ptr, asmjit::x86::Gp &stack_ptr);
	void release_function(void *func_ptr);

	JitCompiler();
	~JitCompiler();
};

#endif // JIT_COMPILER_H
