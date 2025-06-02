/**************************************************************************/
/*  jit_compiler.cpp                                                      */
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

#include "jit_compiler.h"

#include "core/config/engine.h"
#include "core/string/print_string.h"
#include "core/variant/variant.h"

JitCompiler *JitCompiler::singleton = nullptr;

JitCompiler::JitCompiler() {
	singleton = this;
}

JitCompiler::~JitCompiler() {
	singleton = nullptr;
}

JitCompiler *JitCompiler::get_singleton() {
	return singleton;
}

void *JitCompiler::compile_function(const GDScriptFunction *gdscript) {
	print_line("Compiling GDScript function...");
	int ip = 0;
	while (ip < gdscript->code.size())  // Iterate through the bytecode
	{
		int incr = 0;
		GDScriptFunction::Opcode opcode = GDScriptFunction::Opcode(gdscript->_code_ptr[ip]);
		switch(opcode){
			case GDScriptFunction::OPCODE_LINE: 
				print_line(ip, "LINE: ", gdscript->_code_ptr[ip + 1]);
				incr += 2; 
				break;
			
			// Operators (comparison, arithmetic)
			case GDScriptFunction::OPCODE_OPERATOR: {
				constexpr int _pointer_size = sizeof(Variant::ValidatedOperatorEvaluator) / sizeof(*gdscript->_code_ptr);
				int operation = gdscript->_code_ptr[ip + 4];
				print_line(ip, "OPERATOR: ", Variant::get_operator_name(Variant::Operator(operation)));
				incr += 7 + _pointer_size;
			} break;
			case GDScriptFunction::OPCODE_OPERATOR_VALIDATED: {
				int operation = gdscript->_code_ptr[ip + 4];
				print_line(ip, "OPERATOR_VALIDATED: ", operation, gdscript->_code_ptr[ip + 1], gdscript->_code_ptr[ip + 2], gdscript->_code_ptr[ip + 3]);
				incr += 5;
			} break;
			
			// Assignments
			case GDScriptFunction::OPCODE_ASSIGN:
				print_line(ip, "ASSIGN");
				incr += 3;
				break;
			case GDScriptFunction::OPCODE_ASSIGN_NULL:
				print_line(ip, "ASSIGN_NULL");
				incr += 2;
				break;
			case GDScriptFunction::OPCODE_ASSIGN_TRUE:
				print_line(ip, "ASSIGN_TRUE");
				incr += 2;
				break;
			case GDScriptFunction::OPCODE_ASSIGN_FALSE:
				print_line(ip, "ASSIGN_FALSE");
				incr += 2;
				break;
			case GDScriptFunction::OPCODE_ASSIGN_TYPED_BUILTIN:
				print_line(ip, "ASSIGN_TYPED_BUILTIN: ", Variant::get_type_name((Variant::Type)gdscript->_code_ptr[ip + 3]));
				incr += 4;
				break;
			
			// Jumps
			case GDScriptFunction::OPCODE_JUMP:
				print_line(ip, "JUMP to: ", gdscript->_code_ptr[ip + 1]);
				incr += 2;
				break;
			case GDScriptFunction::OPCODE_JUMP_IF:
				print_line(ip, "JUMP_IF to: ", gdscript->_code_ptr[ip + 2]);
				incr += 3;
				break;
			case GDScriptFunction::OPCODE_JUMP_IF_NOT:
				print_line(ip, "JUMP_IF_NOT to: ", gdscript->_code_ptr[ip + 2]);
				incr += 3;
				break;
			
			// Returns
			case GDScriptFunction::OPCODE_RETURN:
				print_line(ip, "RETURN");
				incr += 2;
				break;
			case GDScriptFunction::OPCODE_RETURN_TYPED_BUILTIN:
				print_line(ip, "RETURN_TYPED_BUILTIN: ", Variant::get_type_name((Variant::Type)gdscript->_code_ptr[ip + 2]));
				incr += 3;
				break;
			
			case GDScriptFunction::OPCODE_ITERATE_BEGIN_ARRAY:
				print_line(ip, "ITERATE_BEGIN, jump to: ", gdscript->_code_ptr[ip + 4]);
				incr += 5;
				break;

			case GDScriptFunction::OPCODE_ITERATE_ARRAY:
				print_line(ip, "ITERATE_BEGIN, jump to: ", gdscript->_code_ptr[ip + 4]);
				incr += 5;
				break;
		
			case GDScriptFunction::OPCODE_CALL_GDSCRIPT_UTILITY: {
				print_line(ip, "CALL_GDSCRIPT_UTILITY");
				int instr_var_args = gdscript->_code_ptr[++ip];
				int argc = gdscript->_code_ptr[ip + 1 + instr_var_args];
				incr = 4 + argc;
			} break;
			// Constants and local variables
			case GDScriptFunction::OPCODE_GET_NAMED:
				print_line(ip, "GET_NAMED");
				incr += 4;
				break;
			case GDScriptFunction::OPCODE_SET_NAMED:
				print_line(ip, "SET_NAMED");
				incr += 4;
				break;

			case GDScriptFunction::OPCODE_END:
				print_line(ip, "END");
				incr += 1;
				break;
			
			default:
				print_line(ip, "Unknown opcode: ", opcode);
				incr += 1;
				break;
		}
		ip += incr;
	}

	asmjit::CodeHolder code;
	code.init(runtime.environment(), runtime.cpuFeatures());

	asmjit::x86::Compiler cc(&code);

	asmjit::FuncSignature sig;
	sig.setRet(asmjit::TypeId::kInt32);
	sig.addArg(asmjit::TypeId::kInt32);

	asmjit::FuncNode* funcNode = cc.addFunc(sig);

	asmjit::x86::Gp n    = cc.newUInt32("n");      // Input: limit
    asmjit::x86::Gp a    = cc.newUInt32("a");      // a = 0
  	asmjit::x86::Gp b    = cc.newUInt32("b");      // b = 1
  	asmjit::x86::Gp i    = cc.newUInt32("i");      // loop counter
  	asmjit::x86::Gp tmp  = cc.newUInt32("tmp");    // temporary for sum

	funcNode->setArg(0, n);

	asmjit::Label L_Loop = cc.newLabel();
	asmjit::Label L_Exit = cc.newLabel();
	asmjit::Label L_ReturnOne = cc.newLabel();

	cc.cmp(n, 0);
	cc.jz(L_Exit);       // If n == 0, return 0

	cc.cmp(n, 1);
	cc.jz(L_ReturnOne);

	// Initialize a = 0, b = 1, i = 2
	cc.mov(a, 0);
	cc.mov(b, 1);
	cc.mov(i, 2);

	cc.bind(L_Loop);
	cc.cmp(i, n);
	cc.ja(L_Exit);  // Exit if i > n

	cc.mov(tmp, a);     // tmp = a
	cc.add(tmp, b);     // tmp += b
	cc.mov(a, b);       // a = b
	cc.mov(b, tmp);     // b = tmp
	cc.inc(i);          // i++
	cc.jmp(L_Loop);

	cc.bind(L_ReturnOne);
	cc.mov(a, 1);
	cc.jmp(L_Exit);

	cc.bind(L_Exit);
	cc.ret(b);

	cc.endFunc();
	cc.finalize();

	void* func_ptr = nullptr;
	asmjit::Error err = runtime.add(&func_ptr, &code);
	if (err) {
		print_error(asmjit::DebugUtils::errorAsString(err));
		return nullptr;
	}

	return func_ptr;
}

void JitCompiler::release_function(void *func_ptr) {
	if (!func_ptr) {
		return;
	}

	runtime.release(func_ptr);
}
