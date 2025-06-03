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
HashMap<Variant::ValidatedOperatorEvaluator, String> JitCompiler::op_map;

JitCompiler::JitCompiler() {
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_ADD, Variant::INT, Variant::INT)] = "ADD_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_SUBTRACT, Variant::INT, Variant::INT)] = "SUBTRACT_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_MULTIPLY, Variant::INT, Variant::INT)] = "MULTIPLY_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_DIVIDE, Variant::INT, Variant::INT)] = "DIVIDE_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_MODULE, Variant::INT, Variant::INT)] = "MODULO_INT_INT";
	
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_EQUAL, Variant::INT, Variant::INT)] = "EQUAL_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_NOT_EQUAL, Variant::INT, Variant::INT)] = "NOT_EQUAL_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_LESS, Variant::INT, Variant::INT)] = "LESS_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_LESS_EQUAL, Variant::INT, Variant::INT)] = "LESS_EQUAL_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_GREATER, Variant::INT, Variant::INT)] = "GREATER_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_GREATER_EQUAL, Variant::INT, Variant::INT)] = "GREATER_EQUAL_INT_INT";
	
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_AND, Variant::BOOL, Variant::BOOL)] = "AND_BOOL_BOOL";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_OR, Variant::BOOL, Variant::BOOL)] = "OR_BOOL_BOOL";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_NOT, Variant::BOOL, Variant::NIL)] = "NOT_BOOL";
	
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_BIT_AND, Variant::INT, Variant::INT)] = "BIT_AND_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_BIT_OR, Variant::INT, Variant::INT)] = "BIT_OR_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_BIT_XOR, Variant::INT, Variant::INT)] = "BIT_XOR_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_BIT_NEGATE, Variant::INT, Variant::NIL)] = "BIT_NEGATE_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_SHIFT_LEFT, Variant::INT, Variant::INT)] = "SHIFT_LEFT_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_SHIFT_RIGHT, Variant::INT, Variant::INT)] = "SHIFT_RIGHT_INT_INT";
	singleton = this;
}

JitCompiler::~JitCompiler() {
	singleton = nullptr;
}

JitCompiler *JitCompiler::get_singleton() {
	return singleton;
}

void JitCompiler::decode_address(int encoded_address, int &address_type, int &address_index) {
	address_type = (encoded_address & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS;
	address_index = encoded_address & GDScriptFunction::ADDR_MASK;
}

// Helper function to get address type name for debugging
String JitCompiler::get_address_type_name(int address_type) {
	switch (address_type) {
		case GDScriptFunction::ADDR_TYPE_STACK:
			return "STACK";
		case GDScriptFunction::ADDR_TYPE_CONSTANT:
			return "CONSTANT";
		case GDScriptFunction::ADDR_TYPE_MEMBER:
			return "MEMBER";
		default:
			return "UNKNOWN";
	}
}

// Helper function to print address information
void JitCompiler::print_address_info(const GDScriptFunction *gdscript, int encoded_address) {
	int address_type, address_index;
	decode_address(encoded_address, address_type, address_index);
	
	String type_name = get_address_type_name(address_type);
	print_line("    Address: ", encoded_address, " -> ", type_name, "[", address_index, "]");
	
	// Print constant value if it's a constant
	if (address_type == GDScriptFunction::ADDR_TYPE_CONSTANT && address_index < gdscript->constants.size()) {
		Variant constant_value = gdscript->constants[address_index];
		print_line("      Constant value: ", constant_value);
	}
}

void *JitCompiler::compile_function(const GDScriptFunction *gdscript) {
	print_line("=== Compiling GDScript function ===");
	print_line("Function name: ", gdscript->get_name());
	print_line("Function return type: ", gdscript->return_type.builtin_type != Variant::NIL ? Variant::get_type_name(gdscript->return_type.builtin_type) : "void");
	
	print_line("Code size: ", gdscript->code.size());
	print_line("Stack size: ", gdscript->get_max_stack_size());
	print_line("Constants count: ", gdscript->constants.size());
	print_line("Arguments count: ", gdscript->get_argument_count());
	
	// Print constants for reference
	print_line("\n=== Constants ===");
	for (int i = 0; i < gdscript->constants.size(); i++) {
		print_line("Constant[", i, "]: ", gdscript->constants[i]);
	}
	
	print_line("\n=== Bytecode Analysis ===");

	asmjit::CodeHolder code;
	code.init(runtime.environment(), runtime.cpuFeatures());

	asmjit::x86::Compiler cc(&code);

	asmjit::FuncSignature sig;
	if (gdscript->return_type.builtin_type == Variant::INT){
		sig.setRet(asmjit::TypeId::kInt32);
	} else if (gdscript->return_type.builtin_type == Variant::FLOAT) {
		sig.setRet(asmjit::TypeId::kFloat32);
	} else {
		sig.setRet(asmjit::TypeId::kVoid);
	}

	for(int i = 0; i < gdscript->get_argument_count(); i++) {
		if (gdscript->argument_types[i].builtin_type == Variant::INT) {
			sig.addArg(asmjit::TypeId::kInt32);
		} else if (gdscript->argument_types[i].builtin_type == Variant::FLOAT) {
			sig.addArg(asmjit::TypeId::kFloat32);
		} else {
			sig.addArg(asmjit::TypeId::kVoid);
		}
	}
	
	asmjit::FuncNode* funcNode = cc.addFunc(sig);
	asmjit::x86::Gp n = cc.newInt32("n");
	funcNode->setArg(0, n);

	int stack_size = gdscript->get_max_stack_size();
    int variant_size = sizeof(int);
    int total_stack_bytes = stack_size * variant_size;
    
    print_line("Allocating stack: ", stack_size, " variants (", total_stack_bytes, " bytes)");
    
    asmjit::x86::Mem stack = cc.newStack(total_stack_bytes, 16);
    asmjit::x86::Gp stack_ptr = cc.newIntPtr("stack");
    cc.lea(stack_ptr, stack);

	// Copy n to stack offset * 3
	cc.mov(asmjit::x86::ptr(stack_ptr, 3 * variant_size), n);

	int ip = 0;
	while (ip < gdscript->code.size())
	{
		int incr = 0;
		GDScriptFunction::Opcode opcode = GDScriptFunction::Opcode(gdscript->_code_ptr[ip]);
		switch(opcode){
			case GDScriptFunction::OPCODE_LINE: 
				print_line(ip, "LINE: ", gdscript->_code_ptr[ip + 1]);
				incr += 2; 
				break;
			
			case GDScriptFunction::OPCODE_OPERATOR: {
				constexpr int _pointer_size = sizeof(Variant::ValidatedOperatorEvaluator) / sizeof(*gdscript->_code_ptr);
				int left_addr = gdscript->_code_ptr[ip + 1];
				int right_addr = gdscript->_code_ptr[ip + 2];
				int result_addr = gdscript->_code_ptr[ip + 3];
				int operation = gdscript->_code_ptr[ip + 4];

				int result_type, result_index;
				decode_address(result_addr, result_type, result_index);

				asmjit::x86::Gp left_val = cc.newInt32();
				asmjit::x86::Gp right_val = cc.newInt32();
				asmjit::x86::Gp dummy = cc.newInt32();

				load_int(cc, left_val, stack_ptr, gdscript, left_addr);
				load_int(cc, right_val, stack_ptr, gdscript, right_addr);
				asmjit::x86::Mem result_mem = asmjit::x86::ptr(stack_ptr, result_index * sizeof(int));
				cc.xor_(dummy, dummy);

				switch (Variant::Operator(operation))
				{
					case Variant::OP_DIVIDE:{
						cc.idiv(dummy, left_val, right_val);
						cc.mov(result_mem, left_val);
					}; break;
					case Variant::OP_MODULE:{
						cc.idiv(dummy, left_val, right_val);
						cc.mov(result_mem, dummy);
					}; break;
				}

				print_line(ip, "OPERATOR: ", Variant::get_operator_name(Variant::Operator(operation)));
				print_line("    Left operand:");
				print_address_info(gdscript, left_addr);
				print_line("    Right operand:");
				print_address_info(gdscript, right_addr);
				print_line("    Result:");
				print_address_info(gdscript, result_addr);
				
				incr = 7 + _pointer_size;
			} break;
			case GDScriptFunction::OPCODE_OPERATOR_VALIDATED: {
				int left_addr = gdscript->_code_ptr[ip + 1];
				int right_addr = gdscript->_code_ptr[ip + 2];
				int result_addr = gdscript->_code_ptr[ip + 3];
				int operation_idx = gdscript->_code_ptr[ip + 4];
				
				Variant::ValidatedOperatorEvaluator op_func = gdscript->operator_funcs[operation_idx];
				
				String operation_name = get_operator_name_from_function(op_func);

				int result_type, result_index;
				decode_address(result_addr, result_type, result_index);

				asmjit::x86::Gp left_val = cc.newInt32();
				asmjit::x86::Gp right_val = cc.newInt32();

				load_int(cc, left_val, stack_ptr, gdscript, left_addr);
				load_int(cc, right_val, stack_ptr, gdscript, right_addr);
				asmjit::x86::Mem result_mem = asmjit::x86::ptr(stack_ptr, result_index * sizeof(int));

				handle_operation(operation_name, cc, left_val, right_val, result_mem);

				print_line(ip, "OPERATOR_VALIDATED: ", operation_name);
				print_line("    Function index: ", operation_idx);
				
				print_line("    Left operand:");
				print_address_info(gdscript, left_addr);
				print_line("    Right operand:");
				print_address_info(gdscript, right_addr);
				print_line("    Result:");
				print_address_info(gdscript, result_addr);
				
				incr = 5;
			} break;

			case GDScriptFunction::OPCODE_ASSIGN: {
				int dst_addr = gdscript->_code_ptr[ip + 1];
				int src_addr = gdscript->_code_ptr[ip + 2];

				int dst_type, dst_index;
				decode_address(dst_addr, dst_type, dst_index);

				asmjit::x86::Gp value = cc.newIntPtr();
				asmjit::x86::Mem variant_mem = asmjit::x86::ptr(stack_ptr, dst_index * sizeof(int));

				load_int(cc, value, stack_ptr, gdscript, src_addr);
				cc.mov(variant_mem, value);

				print_line(ip, "ASSIGN");
				print_line("    Source:");
				print_address_info(gdscript, src_addr);
				print_line("    Destination:");
				print_address_info(gdscript, dst_addr);
				
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_ASSIGN_NULL: {
				int dst_addr = gdscript->_code_ptr[ip + 1];
				
				print_line(ip, "ASSIGN_NULL");
				print_line("    Destination:");
				print_address_info(gdscript, dst_addr);
				
				incr = 2;
			} break;
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
			
			case GDScriptFunction::OPCODE_JUMP:
				print_line(ip, "JUMP to: ", gdscript->_code_ptr[ip + 1]);
				incr += 2;
				break;
			case GDScriptFunction::OPCODE_JUMP_IF: {
				int condition_addr = gdscript->_code_ptr[ip + 1];
				int target = gdscript->_code_ptr[ip + 2];
				
				print_line(ip, "JUMP_IF to: ", target);
				print_line("    Condition:");
				print_address_info(gdscript, condition_addr);
				
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_JUMP_IF_NOT: {
				int condition_addr = gdscript->_code_ptr[ip + 1];
				int target = gdscript->_code_ptr[ip + 2];
				
				print_line(ip, "JUMP_IF_NOT to: ", target);
				print_line("    Condition:");
				print_address_info(gdscript, condition_addr);
				
				incr = 3;
			} break;
			
			case GDScriptFunction::OPCODE_RETURN_TYPED_BUILTIN: {
				int return_addr = gdscript->_code_ptr[ip + 1];

				int address_type, address_index;
				decode_address(return_addr, address_type, address_index);
				Variant return_value;

				if (address_type == GDScriptFunction::ADDR_TYPE_CONSTANT) {
					return_value = gdscript->constants[address_index];
				} else if (address_type == GDScriptFunction::ADDR_TYPE_STACK) {
					int offset = address_index * sizeof(int);
					asmjit::x86::Mem variant_mem = asmjit::x86::ptr(stack_ptr, offset);
					cc.mov(asmjit::x86::eax, variant_mem);
					cc.ret(asmjit::x86::eax);
					goto end_builtin_return;
				} else {
					return_value = Variant();
				}

				if (gdscript->return_type.builtin_type == Variant::INT){
					cc.mov(asmjit::x86::eax, (int)return_value);
					cc.ret(asmjit::x86::eax);
				} else {
					cc.ret();
				}

				end_builtin_return:
				print_line(ip, "RETURN BUILTIN: ", Variant::get_type_name(gdscript->return_type.builtin_type));
				print_line("    Return value:");
				print_address_info(gdscript, return_addr);
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_RETURN: {
				int return_addr = gdscript->_code_ptr[ip + 1];

				int address_type, address_index;
				decode_address(return_addr, address_type, address_index);
				Variant return_value;

				if (address_type == GDScriptFunction::ADDR_TYPE_CONSTANT) {
					return_value = gdscript->constants[address_index];
				} else if (address_type == GDScriptFunction::ADDR_TYPE_STACK) {
					int offset = address_index * sizeof(int);
					asmjit::x86::Mem variant_mem = asmjit::x86::ptr(stack_ptr, offset);
					cc.mov(asmjit::x86::eax, variant_mem);
					cc.ret(asmjit::x86::eax);
					goto end_return;
				} else {
					return_value = Variant();
				}

				if (gdscript->return_type.builtin_type == Variant::INT){
					cc.mov(asmjit::x86::eax, (int)return_value);
					cc.ret(asmjit::x86::eax);
				} else {
					cc.ret();
				}
				end_return:
				print_line(ip, "RETURN");
				print_line("    Return value:");
				print_address_info(gdscript, return_addr);
				incr = 2;
			} break;
			
			case GDScriptFunction::OPCODE_ITERATE_BEGIN_ARRAY: {
				int counter_addr = gdscript->_code_ptr[ip + 1];
				int container_addr = gdscript->_code_ptr[ip + 2];
				int iterator_addr = gdscript->_code_ptr[ip + 3];
				int jump_target = gdscript->_code_ptr[ip + 4];
				
				print_line(ip, "ITERATE_BEGIN, jump to: ", jump_target);
				print_line("    Counter:");
				print_address_info(gdscript, counter_addr);
				print_line("    Container:");
				print_address_info(gdscript, container_addr);
				print_line("    Iterator:");
				print_address_info(gdscript, iterator_addr);
				
				incr = 5;
			} break;

			case GDScriptFunction::OPCODE_ITERATE_ARRAY: {
				int counter_addr = gdscript->_code_ptr[ip + 1];
				int container_addr = gdscript->_code_ptr[ip + 2];
				int iterator_addr = gdscript->_code_ptr[ip + 3];
				int jump_target = gdscript->_code_ptr[ip + 4];
				
				print_line(ip, "ITERATE, jump to: ", jump_target);
				print_line("    Counter:");
				print_address_info(gdscript, counter_addr);
				print_line("    Container:");
				print_address_info(gdscript, container_addr);
				print_line("    Iterator:");
				print_address_info(gdscript, iterator_addr);
				
				incr = 5;
			} break;
		
			case GDScriptFunction::OPCODE_CALL_GDSCRIPT_UTILITY: {
				int instr_var_args = gdscript->_code_ptr[++ip];
				int argc = gdscript->_code_ptr[ip + 1 + instr_var_args];
				int utility_idx = gdscript->_code_ptr[ip + 3 + instr_var_args];
				
				print_line(ip, "CALL_GDSCRIPT_UTILITY: utility_index=", utility_idx, ", argc=", argc);
				
				for (int i = 0; i < argc; i++) {
					int arg_addr = gdscript->_code_ptr[ip + 1 + i];
					print_line("    Arg[", i, "]:");
					print_address_info(gdscript, arg_addr);
				}
				
				int return_addr = gdscript->_code_ptr[1 + argc];
				print_line("    Return:");
				print_address_info(gdscript, return_addr);
				
				incr = 4 + argc;
			} break;

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

	// asmjit::x86::Gp n    = cc.newUInt32("n");
    // asmjit::x86::Gp a    = cc.newUInt32("a");
  	// asmjit::x86::Gp b    = cc.newUInt32("b");
  	// asmjit::x86::Gp i    = cc.newUInt32("i");
  	// asmjit::x86::Gp tmp  = cc.newUInt32("tmp");

	// funcNode->setArg(0, n);

	// asmjit::Label L_Loop = cc.newLabel();
	// asmjit::Label L_Exit = cc.newLabel();
	// asmjit::Label L_ReturnOne = cc.newLabel();

	// cc.cmp(n, 0);
	// cc.jz(L_Exit);

	// cc.cmp(n, 1);
	// cc.jz(L_ReturnOne);

	// cc.mov(a, 0);
	// cc.mov(b, 1);
	// cc.mov(i, 2);

	// cc.bind(L_Loop);
	// cc.cmp(i, n);
	// cc.ja(L_Exit);

	// cc.mov(tmp, a);
	// cc.add(tmp, b);
	// cc.mov(a, b);
	// cc.mov(b, tmp);
	// cc.inc(i);
	// cc.jmp(L_Loop);

	// cc.bind(L_ReturnOne);
	// cc.mov(a, 1);
	// cc.jmp(L_Exit);

	// cc.bind(L_Exit);
	// cc.ret(b);

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

void JitCompiler::handle_operation(String &operation_name, asmjit::v1_16::x86::Compiler &cc, asmjit::v1_16::x86::Gp &left_val, asmjit::v1_16::x86::Gp &right_val, asmjit::v1_16::x86::Mem &result_mem) {
	if (operation_name == "SUBTRACT_INT_INT") {
		cc.sub(left_val, right_val);
		cc.mov(result_mem, left_val);
	} else if (operation_name == "ADD_INT_INT") {
		cc.add(left_val, right_val);
		cc.mov(result_mem, left_val);
	} else if (operation_name == "MULTIPLY_INT_INT") {
		cc.imul(left_val, right_val);
		cc.mov(result_mem, left_val);
	} else if (operation_name == "EQUAL_INT_INT") {
		cc.cmp(left_val, right_val);
		asmjit::x86::Gp result_reg = cc.newInt32();
		cc.sete(result_reg.r8());
		cc.movzx(result_reg, result_reg.r8());
		cc.mov(result_mem, result_reg);
	}
}

void JitCompiler::release_function(void *func_ptr) {
	if (!func_ptr) {
		return;
	}

	runtime.release(func_ptr);
}


String JitCompiler::get_operator_name_from_function(Variant::ValidatedOperatorEvaluator op_func) {    
    if (op_map.has(op_func)) {
        return op_map[op_func];
    }
    
    return "UNKNOWN_OPERATION";
}

void JitCompiler::load_int(asmjit::x86::Compiler& cc, asmjit::x86::Gp& reg, asmjit::x86::Gp& stack_ptr, const GDScriptFunction *gdscript, int address) {
	int address_type, address_index;
	decode_address(address, address_type, address_index);

   	if (address_type == GDScriptFunction::ADDR_TYPE_CONSTANT) {
		cc.mov(reg, (int)gdscript->constants[address_index]);
	} else if (address_type == GDScriptFunction::ADDR_TYPE_STACK) {
		int offset = address_index * sizeof(int);
		asmjit::x86::Mem variant_mem = asmjit::x86::ptr(stack_ptr, offset);
		cc.mov(reg, variant_mem);
	}
}