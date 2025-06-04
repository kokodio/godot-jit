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

void JitCompiler::print_address_info(const GDScriptFunction *gdscript, int encoded_address) {
	int address_type, address_index;
	decode_address(encoded_address, address_type, address_index);

	String type_name = get_address_type_name(address_type);
	print_line("    Address: ", encoded_address, " -> ", type_name, "[", address_index, "]");

	if (address_type == GDScriptFunction::ADDR_TYPE_CONSTANT && address_index < gdscript->constants.size()) {
		Variant constant_value = gdscript->constants[address_index];
		print_line("      Constant value: ", constant_value);
	}
}

asmjit::x86::Mem JitCompiler::get_stack_slot(asmjit::x86::Gp &stack_ptr, int slot_index) {
	int offset = slot_index * STACK_SLOT_SIZE;
	return asmjit::x86::ptr(stack_ptr, offset);
}

//template<typename T> ? rn int
void JitCompiler::set_stack_slot(asmjit::x86::Compiler &cc, asmjit::x86::Gp &stack_ptr, int slot_index, int value) {
	asmjit::x86::Mem slot = get_stack_slot(stack_ptr, slot_index);
	cc.mov(slot, value);
}

void *JitCompiler::compile_function(const GDScriptFunction *gdscript) {
	print_function_info(gdscript);

	asmjit::CodeHolder code;
	asmjit::StringLogger stringLogger;

	code.init(runtime.environment(), runtime.cpuFeatures());
	code.setLogger(&stringLogger);

	asmjit::x86::Compiler cc(&code);
	HashMap<int, asmjit::Label> jump_labels = analyze_jump_targets(gdscript, cc);

	asmjit::FuncSignature sig;
	sig.setRet(asmjit::TypeId::kVoid);
	sig.addArg(asmjit::TypeId::kIntPtr);
	sig.addArg(asmjit::TypeId::kIntPtr);
	sig.addArg(asmjit::TypeId::kIntPtr);

	asmjit::FuncNode *funcNode = cc.addFunc(sig);

	asmjit::x86::Gp result_ptr = cc.newIntPtr("result_ptr");
	asmjit::x86::Gp args_ptr = cc.newIntPtr("args_ptr");
	asmjit::x86::Gp members_ptr = cc.newIntPtr("members_ptr");

	funcNode->setArg(0, result_ptr);
	funcNode->setArg(1, args_ptr);
	funcNode->setArg(2, members_ptr);

	int stack_size = gdscript->get_max_stack_size();
	int total_stack_bytes = stack_size * STACK_SLOT_SIZE;

	print_line("Allocating stack: ", stack_size, " variants (", total_stack_bytes, " bytes)");

	asmjit::x86::Mem stack = cc.newStack(total_stack_bytes, 16);
	asmjit::x86::Gp stack_ptr = cc.newIntPtr("stack");
	cc.lea(stack_ptr, stack);

	extract_arguments(gdscript, cc, args_ptr, stack_ptr);

	print_line("\n=== Bytecode Analysis ===");
	int ip = 0;
	while (ip < gdscript->code.size()) {
		if (jump_labels.has(ip)) {
			cc.bind(jump_labels[ip]);
			print_line(">>> Label bound at position: ", ip);
		}

		int incr = 0;
		GDScriptFunction::Opcode opcode = GDScriptFunction::Opcode(gdscript->_code_ptr[ip]);
		switch (opcode) {
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

				asmjit::x86::Gp left_val = cc.newInt64();
				asmjit::x86::Gp right_val = cc.newInt64();
				asmjit::x86::Gp dummy = cc.newInt64();

				load_int(cc, left_val, stack_ptr, members_ptr, gdscript, left_addr);
				load_int(cc, right_val, stack_ptr, members_ptr, gdscript, right_addr);
				asmjit::x86::Mem result_mem = get_stack_slot(stack_ptr, result_index);
				cc.xor_(dummy, dummy);

				switch (Variant::Operator(operation)) {
					case Variant::OP_DIVIDE: {
						cc.idiv(dummy, left_val, right_val);
						cc.mov(result_mem, left_val);
					}; break;
					case Variant::OP_MODULE: {
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

				if (operation_name != "UNKNOWN_OPERATION") {
					int result_type, result_index;
					decode_address(result_addr, result_type, result_index);

					asmjit::x86::Gp left_val = cc.newInt64();
					asmjit::x86::Gp right_val = cc.newInt64();

					load_int(cc, left_val, stack_ptr, members_ptr, gdscript, left_addr);
					load_int(cc, right_val, stack_ptr, members_ptr, gdscript, right_addr);
					asmjit::x86::Mem result_mem = get_stack_slot(stack_ptr, result_index);

					handle_operation(operation_name, cc, left_val, right_val, result_mem);
				}
				// else {
				// 	int result_type, result_index;
				// 	decode_address(result_addr, result_type, result_index);

				// 	asmjit::x86::Gp left_ptr = cc.newIntPtr();
				// 	asmjit::x86::Gp right_ptr = cc.newIntPtr();
				// 	asmjit::x86::Gp result_ptr = cc.newIntPtr();

				// 	load_variant_ptr(cc, left_ptr, stack_ptr, gdscript, left_addr);
				// 	load_variant_ptr(cc, right_ptr, stack_ptr, gdscript, right_addr);
				// 	load_variant_ptr(cc, result_ptr, stack_ptr, gdscript, result_addr);

				// 	asmjit::InvokeNode* op_invoke;
				// 	cc.invoke(&op_invoke, op_func, asmjit::FuncSignature::build<void, const Variant*, const Variant*, Variant*>());
				// 	op_invoke->setArg(0, left_ptr);
				// 	op_invoke->setArg(1, right_ptr);
				// 	op_invoke->setArg(2, result_ptr);

				// 	cc.mov(result_mem, result_val);
				// }

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

				asmjit::x86::Gp value = cc.newInt64();

				load_int(cc, value, stack_ptr, members_ptr, gdscript, src_addr);
				save_int(cc, value, stack_ptr, members_ptr, gdscript, dst_addr);

				print_line(ip, "ASSIGN");
				print_line("    Source:");
				print_address_info(gdscript, src_addr);
				print_line("    Destination:");
				print_address_info(gdscript, dst_addr);

				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_ASSIGN_NULL: {
				int dst_addr = gdscript->_code_ptr[ip + 1];

				int dst_type, dst_index;
				decode_address(dst_addr, dst_type, dst_index);

				set_stack_slot(cc, stack_ptr, dst_index, 0); // must be save_int

				print_line(ip, "ASSIGN_NULL");
				print_line("    Destination:");
				print_address_info(gdscript, dst_addr);

				incr = 2;
			} break;
			case GDScriptFunction::OPCODE_ASSIGN_TRUE: {
				int dst_addr = gdscript->_code_ptr[ip + 1];

				int dst_type, dst_index;
				decode_address(dst_addr, dst_type, dst_index);

				set_stack_slot(cc, stack_ptr, dst_index, 1); // must be save_int

				print_line(ip, " ASSIGN_TRUE");
				incr = 2;
			} break;
			case GDScriptFunction::OPCODE_ASSIGN_FALSE: {
				int dst_addr = gdscript->_code_ptr[ip + 1];

				int dst_type, dst_index;
				decode_address(dst_addr, dst_type, dst_index);

				set_stack_slot(cc, stack_ptr, dst_index, 0); // must be save_int

				print_line(ip, " ASSIGN_FALSE");
				incr = 2;
			} break;
			case GDScriptFunction::OPCODE_ASSIGN_TYPED_BUILTIN:
				print_line(ip, "ASSIGN_TYPED_BUILTIN: ", Variant::get_type_name((Variant::Type)gdscript->_code_ptr[ip + 3]));
				incr += 4;
				break;

			case GDScriptFunction::OPCODE_JUMP: {
				int target = gdscript->_code_ptr[ip + 1];

				cc.jmp(jump_labels[target]);

				print_line(ip, "JUMP to: ", gdscript->_code_ptr[ip + 1]);
				incr += 2;
			} break;
			case GDScriptFunction::OPCODE_JUMP_IF: {
				int condition_addr = gdscript->_code_ptr[ip + 1];
				int target = gdscript->_code_ptr[ip + 2];

				asmjit::x86::Gp condition = cc.newInt64();
				load_int(cc, condition, stack_ptr, members_ptr, gdscript, condition_addr);

				cc.test(condition, condition);
				cc.jnz(jump_labels[target]);

				print_line(ip, "JUMP_IF to: ", target);
				print_line("    Condition:");
				print_address_info(gdscript, condition_addr);

				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_JUMP_IF_NOT: {
				int condition_addr = gdscript->_code_ptr[ip + 1];
				int target = gdscript->_code_ptr[ip + 2];

				asmjit::x86::Gp condition = cc.newInt64();
				load_int(cc, condition, stack_ptr, members_ptr, gdscript, condition_addr);

				cc.test(condition, condition);
				cc.jz(jump_labels[target]);

				print_line(ip, "JUMP_IF_NOT to: ", target);
				print_line("    Condition:");
				print_address_info(gdscript, condition_addr);

				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_RETURN_TYPED_BUILTIN: {
				int return_addr = gdscript->_code_ptr[ip + 1];

				if (gdscript->return_type.builtin_type == Variant::INT) {
					asmjit::x86::Gp return_value = cc.newInt64("return_value");
					load_int(cc, return_value, stack_ptr, members_ptr, gdscript, return_addr);

					cc.mov(asmjit::x86::dword_ptr(result_ptr, 0), VARIANT_TYPE_INT);
					cc.mov(asmjit::x86::qword_ptr(result_ptr, OFFSET_INT), return_value);
				}

				print_line(ip, "RETURN BUILTIN: ", Variant::get_type_name(gdscript->return_type.builtin_type));
				print_line("    Return value:");
				print_address_info(gdscript, return_addr);
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_RETURN: {
				int return_addr = gdscript->_code_ptr[ip + 1];

				if (gdscript->return_type.builtin_type == Variant::INT) {
					asmjit::x86::Gp return_value = cc.newInt64("return_value");
					load_int(cc, return_value, stack_ptr, members_ptr, gdscript, return_addr);

					cc.mov(asmjit::x86::dword_ptr(result_ptr, 0), VARIANT_TYPE_INT);
					cc.mov(asmjit::x86::qword_ptr(result_ptr, OFFSET_INT), return_value);
				}

				cc.ret();

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
				int return_addr = gdscript->_code_ptr[ip + 1 + argc];
				int utility_idx = gdscript->_code_ptr[ip + 3 + instr_var_args];

				print_line(ip, "CALL_GDSCRIPT_UTILITY: utility_index=", utility_idx, ", argc=", argc);

				for (int i = 0; i < argc; i++) {
					int arg_addr = gdscript->_code_ptr[ip + 1 + i];
					print_line("    Arg[", i, "]:");
					print_address_info(gdscript, arg_addr);
				}

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

	cc.endFunc();
	cc.finalize();

	print_line("--- AsmJit Generated Assembly ---");
	print_line(stringLogger.data());
	print_line("--- End of Assembly ---");

	void *func_ptr = nullptr;
	asmjit::Error err = runtime.add(&func_ptr, &code);
	if (err) {
		print_error(asmjit::DebugUtils::errorAsString(err));
		return nullptr;
	}

	return func_ptr;
}

void JitCompiler::print_function_info(const GDScriptFunction *gdscript) {
	print_line("=== Compiling GDScript function ===");
	print_line("Function name: ", gdscript->get_name());
	print_line("Function return type: ", gdscript->return_type.builtin_type != Variant::NIL ? Variant::get_type_name(gdscript->return_type.builtin_type) : "void");

	print_line("Code size: ", gdscript->code.size());
	print_line("Stack size: ", gdscript->get_max_stack_size());
	print_line("Constants count: ", gdscript->constants.size());
	print_line("Arguments count: ", gdscript->get_argument_count());

	print_line("\n=== Constants ===");
	for (int i = 0; i < gdscript->constants.size(); i++) {
		print_line("Constant[", i, "]: ", gdscript->constants[i]);
	}
}

void JitCompiler::extract_arguments(const GDScriptFunction *gdscript, asmjit::x86::Compiler &cc, asmjit::x86::Gp &args_ptr, asmjit::x86::Gp &stack_ptr) {
	for (int i = 0; i < gdscript->get_argument_count(); i++) {
		if (gdscript->argument_types[i].builtin_type == Variant::INT) {
			asmjit::x86::Gp variant_ptr = cc.newIntPtr();
			cc.mov(variant_ptr, asmjit::x86::ptr(args_ptr, i * sizeof(void *)));

			asmjit::x86::Gp arg_value = cc.newInt64();
			cc.mov(arg_value, asmjit::x86::ptr(variant_ptr, (int)OFFSET_INT));
			cc.mov(get_stack_slot(stack_ptr, i + 3), arg_value);
		}
	}
}

void JitCompiler::handle_operation(String &operation_name, asmjit::x86::Compiler &cc, asmjit::x86::Gp &left_val, asmjit::x86::Gp &right_val, asmjit::x86::Mem &result_mem) {
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
		asmjit::x86::Gp result_reg = cc.newInt64();
		cc.sete(result_reg.r8());
		cc.movzx(result_reg, result_reg.r8());
		cc.mov(result_mem, result_reg);
	} else if (operation_name == "LESS_INT_INT") {
		cc.cmp(left_val, right_val);
		asmjit::x86::Gp result_reg = cc.newInt64();
		cc.setl(result_reg.r8());
		cc.movzx(result_reg, result_reg.r8());
		cc.mov(result_mem, result_reg);
	} else if (operation_name == "GREATER_INT_INT") {
		cc.cmp(left_val, right_val);
		asmjit::x86::Gp result_reg = cc.newInt64();
		cc.setg(result_reg.r8());
		cc.movzx(result_reg, result_reg.r8());
		cc.mov(result_mem, result_reg);
	} else if (operation_name == "LESS_EQUAL_INT_INT") {
		cc.cmp(left_val, right_val);
		asmjit::x86::Gp result_reg = cc.newInt64();
		cc.setle(result_reg.r8());
		cc.movzx(result_reg, result_reg.r8());
		cc.mov(result_mem, result_reg);
	} else if (operation_name == "GREATER_EQUAL_INT_INT") {
		cc.cmp(left_val, right_val);
		asmjit::x86::Gp result_reg = cc.newInt64();
		cc.setge(result_reg.r8());
		cc.movzx(result_reg, result_reg.r8());
		cc.mov(result_mem, result_reg);
	} else if (operation_name == "NOT_EQUAL_INT_INT") {
		cc.cmp(left_val, right_val);
		asmjit::x86::Gp result_reg = cc.newInt64();
		cc.setne(result_reg.r8());
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

void JitCompiler::load_int(asmjit::x86::Compiler &cc, asmjit::x86::Gp &reg, asmjit::x86::Gp &stack_ptr, asmjit::x86::Gp &members_ptr, const GDScriptFunction *gdscript, int address) {
	int address_type, address_index;
	decode_address(address, address_type, address_index);

	if (address_type == GDScriptFunction::ADDR_TYPE_CONSTANT) {
		cc.mov(reg, (int)gdscript->constants[address_index]);
	} else if (address_type == GDScriptFunction::ADDR_TYPE_STACK) {
		cc.mov(reg, get_stack_slot(stack_ptr, address_index));
	} else if (address_type == GDScriptFunction::ADDR_TYPE_MEMBER) {
		asmjit::x86::Gp variant_ptr = cc.newIntPtr();
		cc.mov(variant_ptr, members_ptr);
		cc.add(variant_ptr, (int)(address_index * sizeof(Variant)));
		cc.mov(reg, asmjit::x86::ptr(variant_ptr, (int)OFFSET_INT));
	}
}

void JitCompiler::save_int(asmjit::x86::Compiler &cc, asmjit::x86::Gp &reg, asmjit::x86::Gp &stack_ptr, asmjit::x86::Gp &members_ptr, const GDScriptFunction *gdscript, int address) {
	int address_type, address_index;
	decode_address(address, address_type, address_index);

	if (address_type == GDScriptFunction::ADDR_TYPE_STACK) {
		asmjit::x86::Mem variant_mem = get_stack_slot(stack_ptr, address_index);
		cc.mov(variant_mem, reg);
	} else if (address_type == GDScriptFunction::ADDR_TYPE_MEMBER) {
		int offset = address_index * sizeof(Variant);
		asmjit::x86::Gp variant_ptr = cc.newIntPtr();

		cc.lea(variant_ptr, asmjit::x86::ptr(members_ptr, offset));
		cc.mov(asmjit::x86::dword_ptr(variant_ptr, 0), VARIANT_TYPE_INT);
		cc.mov(asmjit::x86::qword_ptr(variant_ptr, OFFSET_INT), reg);
	}
}

HashMap<int, asmjit::Label> JitCompiler::analyze_jump_targets(const GDScriptFunction *gdscript, asmjit::x86::Compiler &cc) {
	HashMap<int, asmjit::Label> jump_labels;

	print_line("\n=== Analyzing Jump Targets ===");

	int ip = 0;
	while (ip < gdscript->code.size()) {
		int incr = 0;
		GDScriptFunction::Opcode opcode = GDScriptFunction::Opcode(gdscript->_code_ptr[ip]);

		switch (opcode) {
			case GDScriptFunction::OPCODE_LINE:
				incr = 2;
				break;

			case GDScriptFunction::OPCODE_OPERATOR: {
				constexpr int _pointer_size = sizeof(Variant::ValidatedOperatorEvaluator) / sizeof(*gdscript->_code_ptr);
				incr = 7 + _pointer_size;
			} break;

			case GDScriptFunction::OPCODE_OPERATOR_VALIDATED: {
				incr = 5;
			} break;

			case GDScriptFunction::OPCODE_ASSIGN: {
				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_ASSIGN_NULL:
			case GDScriptFunction::OPCODE_ASSIGN_TRUE:
			case GDScriptFunction::OPCODE_ASSIGN_FALSE:
				incr = 2;
				break;

			case GDScriptFunction::OPCODE_ASSIGN_TYPED_BUILTIN:
				incr = 4;
				break;

			case GDScriptFunction::OPCODE_JUMP: {
				int target = gdscript->_code_ptr[ip + 1];
				if (!jump_labels.has(target)) {
					jump_labels[target] = cc.newLabel();
					print_line("Created label for JUMP target: ", target);
				}
				incr = 2;
			} break;

			case GDScriptFunction::OPCODE_JUMP_IF: {
				int target = gdscript->_code_ptr[ip + 2];
				if (!jump_labels.has(target)) {
					jump_labels[target] = cc.newLabel();
					print_line("Created label for JUMP_IF target: ", target);
				}
				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_JUMP_IF_NOT: {
				int target = gdscript->_code_ptr[ip + 2];
				if (!jump_labels.has(target)) {
					jump_labels[target] = cc.newLabel();
					print_line("Created label for JUMP_IF_NOT target: ", target);
				}
				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_RETURN_TYPED_BUILTIN:
				incr = 3;
				break;

			case GDScriptFunction::OPCODE_RETURN:
				incr = 2;
				break;

			case GDScriptFunction::OPCODE_ITERATE_BEGIN_ARRAY:
			case GDScriptFunction::OPCODE_ITERATE_ARRAY: {
				int jump_target = gdscript->_code_ptr[ip + 4];
				if (!jump_labels.has(jump_target)) {
					jump_labels[jump_target] = cc.newLabel();
					print_line("Created label for ITERATE target: ", jump_target);
				}
				incr = 5;
			} break;

			case GDScriptFunction::OPCODE_CALL_GDSCRIPT_UTILITY: {
				int instr_var_args = gdscript->_code_ptr[++ip];
				int argc = gdscript->_code_ptr[ip + 1 + instr_var_args];
				incr = 4 + argc;
			} break;

			case GDScriptFunction::OPCODE_END:
				incr = 1;
				break;

			default:
				incr = 1;
				break;
		}
		ip += incr;
	}

	return jump_labels;
}