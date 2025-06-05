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

extern "C" void call_variant_method(Variant *base, const StringName *method_name, const Variant **args, int argc, Variant *result, Callable::CallError *error) {
	base->callp(*method_name, args, argc, *result, *error);
}

JitCompiler *JitCompiler::singleton = nullptr;
HashMap<Variant::ValidatedOperatorEvaluator, String> JitCompiler::op_map;
HashMap<Variant::ValidatedOperatorEvaluator, OperatorTypes> JitCompiler::evaluator_to_types_map;

JitCompiler::JitCompiler() {
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_ADD, Variant::INT, Variant::INT)] = "ADD_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_SUBTRACT, Variant::INT, Variant::INT)] = "SUBTRACT_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_MULTIPLY, Variant::INT, Variant::INT)] = "MULTIPLY_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_EQUAL, Variant::INT, Variant::INT)] = "EQUAL_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_NOT_EQUAL, Variant::INT, Variant::INT)] = "NOT_EQUAL_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_LESS, Variant::INT, Variant::INT)] = "LESS_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_LESS_EQUAL, Variant::INT, Variant::INT)] = "LESS_EQUAL_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_GREATER, Variant::INT, Variant::INT)] = "GREATER_INT_INT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_GREATER_EQUAL, Variant::INT, Variant::INT)] = "GREATER_EQUAL_INT_INT";

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

void *JitCompiler::compile_function(const GDScriptFunction *gdscript) {
	print_function_info(gdscript);

	asmjit::CodeHolder code;
	asmjit::StringLogger stringLogger;

	code.init(runtime.environment(), runtime.cpuFeatures());
	code.setLogger(&stringLogger);

	asmjit::x86::Compiler cc(&code);

	asmjit::FuncSignature sig;
	sig.setRet(asmjit::TypeId::kVoid);
	sig.addArg(asmjit::TypeId::kIntPtr);
	sig.addArg(asmjit::TypeId::kIntPtr);
	sig.addArg(asmjit::TypeId::kIntPtr);
	sig.addArg(asmjit::TypeId::kIntPtr);
	sig.addArg(asmjit::TypeId::kIntPtr);

	asmjit::FuncNode *funcNode = cc.addFunc(sig);

	asmjit::x86::Gp result_ptr = cc.newIntPtr("result_ptr");
	asmjit::x86::Gp args_ptr = cc.newIntPtr("args_ptr");
	asmjit::x86::Gp members_ptr = cc.newIntPtr("members_ptr");
	asmjit::x86::Gp base_ptr = cc.newIntPtr("base_ptr");
	asmjit::x86::Gp class_ptr = cc.newIntPtr("stack_ptr");

	funcNode->setArg(0, result_ptr);
	funcNode->setArg(1, args_ptr);
	funcNode->setArg(2, members_ptr);
	funcNode->setArg(3, base_ptr);
	funcNode->setArg(4, class_ptr);

	int stack_size = gdscript->get_max_stack_size();
	int total_stack_bytes = stack_size * STACK_SLOT_SIZE;

	Vector<Variant::Type> stack_types;
	for (int i = 0; i < stack_size; i++) {
		stack_types.push_back(Variant::NIL);
	}

	print_line("Allocating stack: ", stack_size, " variants (", total_stack_bytes, " bytes)");

	asmjit::x86::Mem stack = cc.newStack(total_stack_bytes, 16);
	asmjit::x86::Gp stack_ptr = cc.newIntPtr("stack");
	cc.lea(stack_ptr, stack);

	JitContext context;
	context.gdscript = gdscript;
	context.stack_ptr = stack_ptr;
	context.members_ptr = members_ptr;
	context.args_ptr = args_ptr;
	context.stack_types = stack_types;
	context.cc = &cc;
	context.result_ptr = result_ptr;

	HashMap<int, asmjit::Label> jump_labels = analyze_jump_targets(context);
	initialize_stack(context, base_ptr, class_ptr);
	extract_arguments(context);

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
				Variant::Operator operation = (Variant::Operator)gdscript->_code_ptr[ip + 4];

				asmjit::x86::Gp left_ptr = cc.newIntPtr();
				asmjit::x86::Gp right_ptr = cc.newIntPtr();
				asmjit::x86::Gp result_ptr = cc.newIntPtr();

				get_variant_ptr(context, left_ptr, left_addr);
				get_variant_ptr(context, right_ptr, right_addr);
				get_variant_ptr(context, result_ptr, result_addr);

				asmjit::x86::Mem valid_mem = cc.newStack(sizeof(bool), 16);
				asmjit::x86::Gp valid_ptr = cc.newIntPtr();
				cc.lea(valid_ptr, valid_mem);
				cc.mov(asmjit::x86::byte_ptr(valid_ptr), 1);

				asmjit::x86::Mem op_mem = cc.newStack(sizeof(Variant::Operator), 16);
				asmjit::x86::Gp op_ptr = cc.newIntPtr();
				cc.lea(op_ptr, op_mem);
				cc.mov(asmjit::x86::dword_ptr(op_ptr), operation);

				initialize_with_type(context, result_addr, Variant::NIL);

				asmjit::InvokeNode *evaluate_invoke;
				cc.invoke(&evaluate_invoke, static_cast<void (*)(const Variant::Operator &, const Variant &, const Variant &, Variant &, bool &)>(&Variant::evaluate),
						asmjit::FuncSignature::build<void, const Variant::Operator &, const Variant &, const Variant &, Variant &, bool &>());
				evaluate_invoke->setArg(0, op_ptr);
				evaluate_invoke->setArg(1, left_ptr);
				evaluate_invoke->setArg(2, right_ptr);
				evaluate_invoke->setArg(3, result_ptr);
				evaluate_invoke->setArg(4, valid_ptr);

				print_line(ip, "OPERATOR: ", Variant::get_operator_name(operation));
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
					asmjit::x86::Gp left_val = cc.newInt64();
					asmjit::x86::Gp right_val = cc.newInt64();
					asmjit::x86::Gp result_val = cc.newInt64();

					extract_int_from_variant(context, left_val, left_addr);
					extract_int_from_variant(context, right_val, right_addr);

					handle_operation(operation_name, context, left_val, right_val, result_val);

					initialize_with_type(context, result_addr, Variant::INT);
					store_reg_to_variant(context, result_val, result_addr);
				} else {
					auto types = get_operator_types(op_func);
					Variant::Type ret_type = get_result_type_for_operator(types);

					asmjit::x86::Gp left_ptr = cc.newIntPtr();
					asmjit::x86::Gp right_ptr = cc.newIntPtr();
					asmjit::x86::Gp op_ptr = cc.newIntPtr();

					get_variant_ptr(context, left_ptr, left_addr);
					get_variant_ptr(context, right_ptr, right_addr);
					get_variant_ptr(context, op_ptr, result_addr);

					initialize_with_type(context, result_addr, ret_type);

					asmjit::InvokeNode *op_invoke;
					cc.invoke(&op_invoke, op_func, asmjit::FuncSignature::build<void, const Variant *, const Variant *, Variant *>());
					op_invoke->setArg(0, left_ptr);
					op_invoke->setArg(1, right_ptr);
					op_invoke->setArg(2, op_ptr);
				}

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

				asmjit::x86::Gp src_ptr = cc.newIntPtr();
				asmjit::x86::Gp dst_ptr = cc.newIntPtr();

				get_variant_ptr(context, src_ptr, src_addr);
				get_variant_ptr(context, dst_ptr, dst_addr);

				copy_variant(context, dst_ptr, src_ptr);

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

				//set_stack_slot(context, dst_index, 0); // must be save_int

				print_line(ip, "ASSIGN_NULL");
				print_line("    Destination:");
				print_address_info(gdscript, dst_addr);

				incr = 2;
			} break;
			case GDScriptFunction::OPCODE_ASSIGN_TRUE: {
				int dst_addr = gdscript->_code_ptr[ip + 1];

				int dst_type, dst_index;
				decode_address(dst_addr, dst_type, dst_index);

				//set_stack_slot(context, dst_index, 1); // must be save_int

				print_line(ip, " ASSIGN_TRUE");
				incr = 2;
			} break;
			case GDScriptFunction::OPCODE_ASSIGN_FALSE: {
				int dst_addr = gdscript->_code_ptr[ip + 1];

				int dst_type, dst_index;
				decode_address(dst_addr, dst_type, dst_index);

				//set_stack_slot(context, dst_index, 0); // must be save_int

				print_line(ip, " ASSIGN_FALSE");
				incr = 2;
			} break;
			case GDScriptFunction::OPCODE_ASSIGN_TYPED_BUILTIN: {
				int dst_addr = gdscript->_code_ptr[ip + 1];
				int src_addr = gdscript->_code_ptr[ip + 2];
				Variant::Type target_type = (Variant::Type)gdscript->_code_ptr[ip + 3];

				asmjit::x86::Gp src_ptr = cc.newIntPtr();
				asmjit::x86::Gp dst_ptr = cc.newIntPtr();

				get_variant_ptr(context, src_ptr, src_addr);
				get_variant_ptr(context, dst_ptr, dst_addr);

				asmjit::x86::Gp args_array = cc.newIntPtr("args_array");
				asmjit::x86::Gp arg_ptr = cc.newIntPtr();
				cc.lea(args_array, cc.newStack(sizeof(void *), 16));

				get_variant_ptr(context, arg_ptr, src_addr);
				cc.mov(asmjit::x86::ptr(args_array, 0), arg_ptr);

				initialize_with_type(context, dst_addr, target_type);

				asmjit::x86::Mem call_error_mem = cc.newStack(sizeof(Callable::CallError), 16);
				asmjit::x86::Gp call_error_ptr = cc.newIntPtr();
				cc.lea(call_error_ptr, call_error_mem);

				asmjit::InvokeNode *construct_invoke;
				cc.invoke(&construct_invoke, &Variant::construct, asmjit::FuncSignature::build<void, Variant::Type, Variant &, const Variant **, int, Callable::CallError &>());
				construct_invoke->setArg(0, target_type);
				construct_invoke->setArg(1, dst_ptr);
				construct_invoke->setArg(2, args_array);
				construct_invoke->setArg(3, 1);
				construct_invoke->setArg(4, call_error_ptr);

				print_line(ip, "ASSIGN_TYPED_BUILTIN: ", Variant::get_type_name(target_type));
				print_line("    Source:");
				print_address_info(gdscript, src_addr);
				print_line("    Destination:");
				print_address_info(gdscript, dst_addr);
				incr += 4;
			} break;

			case GDScriptFunction::OPCODE_CONSTRUCT: {
				int instr_arg_count = gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				int argc = gdscript->_code_ptr[ip + 1];
				Variant::Type construct_type = (Variant::Type)gdscript->_code_ptr[ip + 2];
				int result_addr = gdscript->_code_ptr[ip];

				print_line(ip - instr_arg_count - 1, "CONSTRUCT: ", Variant::get_type_name(construct_type), ", argc=", argc);

				asmjit::x86::Gp dst_ptr = cc.newIntPtr();
				get_variant_ptr(context, dst_ptr, result_addr);

				asmjit::x86::Gp args_array = cc.newIntPtr("args_array");
				if (argc > 0) {
					int args_array_size = argc * sizeof(void *);
					asmjit::x86::Mem args_stack = cc.newStack(args_array_size, 16);
					cc.lea(args_array, args_stack);

					for (int i = 0; i < argc; i++) {
						int arg_addr = gdscript->_code_ptr[ip - argc + i];

						asmjit::x86::Gp arg_ptr = cc.newIntPtr();
						get_variant_ptr(context, arg_ptr, arg_addr);

						cc.mov(asmjit::x86::ptr(args_array, i * sizeof(void *)), arg_ptr);

						print_line("    Arg[", i, "]:");
						print_address_info(gdscript, arg_addr);
					}
				} else {
					cc.mov(args_array, 0);
					print_line("    No arguments for construct call");
				}

				asmjit::x86::Mem call_error_mem = cc.newStack(sizeof(Callable::CallError), 16);
				asmjit::x86::Gp call_error_ptr = cc.newIntPtr();
				cc.lea(call_error_ptr, call_error_mem);

				initialize_with_type(context, result_addr, Variant::NIL); // ?

				asmjit::InvokeNode *construct_invoke;
				cc.invoke(&construct_invoke, &Variant::construct, asmjit::FuncSignature::build<void, Variant::Type, Variant &, const Variant **, int, Callable::CallError &>());
				construct_invoke->setArg(0, construct_type);
				construct_invoke->setArg(1, dst_ptr);
				construct_invoke->setArg(2, args_array);
				construct_invoke->setArg(3, argc);
				construct_invoke->setArg(4, call_error_ptr);

				print_line("    Result:");
				print_address_info(gdscript, result_addr);

				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_CONSTRUCT_VALIDATED: {
				int instr_arg_count = gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				int argc = gdscript->_code_ptr[ip + 1];
				int constructor_idx = gdscript->_code_ptr[ip + 2];
				int result_addr = gdscript->_code_ptr[ip];

				Variant::ValidatedConstructor constructor = gdscript->_constructors_ptr[constructor_idx];

				print_line(ip - instr_arg_count - 1, "CONSTRUCT_VALIDATED: constructor_idx=", constructor_idx, ", argc=", argc);

				asmjit::x86::Gp args_array = cc.newIntPtr("args_array");
				if (argc > 0) {
					int args_array_size = argc * sizeof(void *);
					asmjit::x86::Mem args_stack = cc.newStack(args_array_size, 16);
					cc.lea(args_array, args_stack);

					for (int i = 0; i < argc; i++) {
						int arg_addr = gdscript->_code_ptr[ip - argc + i];

						asmjit::x86::Gp arg_ptr = cc.newIntPtr();
						get_variant_ptr(context, arg_ptr, arg_addr);

						cc.mov(asmjit::x86::ptr(args_array, i * sizeof(void *)), arg_ptr);

						print_line("    Arg[", i, "]:");
						print_address_info(gdscript, arg_addr);
					}
				} else {
					cc.mov(args_array, 0);
					print_line("    No arguments for utility call");
				}

				asmjit::x86::Gp result_ptr = cc.newIntPtr();
				get_variant_ptr(context, result_ptr, result_addr);

				initialize_with_type(context, result_addr, Variant::NIL);

				asmjit::InvokeNode *construct_invoke;
				cc.invoke(&construct_invoke, constructor, asmjit::FuncSignature::build<void, Variant *, const Variant **>());
				construct_invoke->setArg(0, result_ptr);
				construct_invoke->setArg(1, args_array);

				print_line("    Result:");
				print_address_info(gdscript, result_addr);

				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_CALL_RETURN: {
				int instr_arg_count = gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				int return_addr = gdscript->_code_ptr[ip];
				int argc = gdscript->_code_ptr[ip + 1];
				int function_name_idx = gdscript->_code_ptr[ip + 2];
				int base_addr = gdscript->_code_ptr[ip - 1];

				StringName function_name = gdscript->_global_names_ptr[function_name_idx];
				print_line(ip - instr_arg_count - 1, "CALL_RETURN: ", function_name, ", argc=", argc, ", instr_arg_count=", instr_arg_count);

				asmjit::x86::Gp base_ptr = cc.newIntPtr();
				asmjit::x86::Gp result_ptr = cc.newIntPtr();

				get_variant_ptr(context, base_ptr, base_addr);
				get_variant_ptr(context, result_ptr, return_addr);

				asmjit::x86::Gp args_array = cc.newIntPtr("args_array");

				if (argc > 0) {
					int args_array_size = argc * sizeof(void *);
					asmjit::x86::Mem args_stack = cc.newStack(args_array_size, 16);
					cc.lea(args_array, args_stack);

					for (int i = 0; i < argc; i++) {
						int arg_addr = gdscript->_code_ptr[ip - instr_arg_count + i + 1];

						asmjit::x86::Gp arg_ptr = cc.newIntPtr();
						get_variant_ptr(context, arg_ptr, arg_addr);

						cc.mov(asmjit::x86::ptr(args_array, i * sizeof(void *)), arg_ptr);

						print_line("    Arg[", i, "]:");
						print_address_info(gdscript, arg_addr);
					}
				} else {
					cc.mov(args_array, 0);
					print_line("    No arguments for call");
				}

				initialize_with_type(context, return_addr, Variant::NIL);

				asmjit::x86::Mem call_error_mem = cc.newStack(sizeof(Callable::CallError), 16);
				asmjit::x86::Gp call_error_ptr = cc.newIntPtr("call_error_ptr");
				cc.lea(call_error_ptr, call_error_mem);

				asmjit::x86::Gp function_name_ptr = cc.newIntPtr("function_name_ptr");
				cc.mov(function_name_ptr, &gdscript->_global_names_ptr[function_name_idx]);

				asmjit::InvokeNode *call_invoke;
				cc.invoke(&call_invoke, &call_variant_method,
						asmjit::FuncSignature::build<void, const Variant *, const StringName *, const Variant **, int, Variant *, Callable::CallError *>());

				call_invoke->setArg(0, base_ptr);
				call_invoke->setArg(1, function_name_ptr);
				call_invoke->setArg(2, args_array);
				call_invoke->setArg(3, argc);
				call_invoke->setArg(4, result_ptr);
				call_invoke->setArg(5, call_error_ptr);

				print_line("    Return value:");
				print_address_info(gdscript, return_addr);
				print_line("    Base adress:");
				print_address_info(gdscript, base_addr);

				incr = 3;

			} break;

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
				extract_int_from_variant(context, condition, condition_addr);

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
				extract_int_from_variant(context, condition, condition_addr);

				cc.test(condition, condition);
				cc.jz(jump_labels[target]);

				print_line(ip, "JUMP_IF_NOT to: ", target);
				print_line("    Condition:");
				print_address_info(gdscript, condition_addr);

				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_RETURN_TYPED_BUILTIN: {
				int return_addr = gdscript->_code_ptr[ip + 1];

				int return_type, return_index;
				decode_address(return_addr, return_type, return_index);

				asmjit::x86::Gp src_addr = cc.newIntPtr("src_addr");
				asmjit::x86::Gp dst_addr = cc.newIntPtr("dst_addr");

				get_variant_ptr(context, src_addr, return_addr);
				cc.mov(dst_addr, result_ptr);

				copy_variant(context, dst_addr, src_addr);
				cc.ret();

				print_line(ip, "RETURN BUILTIN: ", Variant::get_type_name(gdscript->return_type.builtin_type));
				print_line("    Return value:");
				print_address_info(gdscript, return_addr);
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_RETURN: {
				int return_addr = gdscript->_code_ptr[ip + 1];

				int return_type, return_index;
				decode_address(return_addr, return_type, return_index);

				asmjit::x86::Gp src_addr = cc.newIntPtr("src_addr");
				asmjit::x86::Gp dst_addr = cc.newIntPtr("dst_addr");

				get_variant_ptr(context, src_addr, return_addr);
				cc.mov(dst_addr, result_ptr);

				copy_variant(context, dst_addr, src_addr);
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

			case GDScriptFunction::OPCODE_CALL_UTILITY: {
				int instr_arg_count = gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				int return_addr = gdscript->_code_ptr[ip];
				int argc = gdscript->_code_ptr[ip + 1];
				int utility_name_idx = gdscript->_code_ptr[ip + 2];
				StringName function_name = gdscript->_global_names_ptr[utility_name_idx];
				print_line(ip - instr_arg_count - 1, "CALL_UTILITY: ", function_name, ", argc=", argc, ", instr_arg_count=", instr_arg_count);

				asmjit::x86::Gp args_array = cc.newIntPtr("args_array");

				if (argc > 0) {
					int args_array_size = argc * sizeof(void *);
					asmjit::x86::Mem args_stack = cc.newStack(args_array_size, 16);
					cc.lea(args_array, args_stack);

					for (int i = 0; i < argc; i++) {
						int arg_addr = gdscript->_code_ptr[ip - argc + i];

						asmjit::x86::Gp arg_ptr = cc.newIntPtr();
						get_variant_ptr(context, arg_ptr, arg_addr);

						cc.mov(asmjit::x86::ptr(args_array, i * sizeof(void *)), arg_ptr);

						print_line("    Arg[", i, "]:");
						print_address_info(gdscript, arg_addr);
					}
				} else {
					cc.mov(args_array, 0);
					print_line("    No arguments for utility call");
				}

				asmjit::x86::Gp result_ptr = cc.newIntPtr("utility_result_ptr");
				get_variant_ptr(context, result_ptr, return_addr);

				initialize_with_type(context, return_addr, Variant::NIL);

				asmjit::x86::Gp function_name_ptr = cc.newIntPtr("function_name_ptr");
				cc.mov(function_name_ptr, &gdscript->_global_names_ptr[utility_name_idx]);

				asmjit::x86::Mem call_error_mem = cc.newStack(sizeof(Callable::CallError), 16);
				asmjit::x86::Gp call_error_ptr = cc.newIntPtr();
				cc.lea(call_error_ptr, call_error_mem);

				asmjit::InvokeNode *utility_invoke;
				cc.invoke(&utility_invoke, &Variant::call_utility_function, asmjit::FuncSignature::build<void, StringName &, Variant *, const Variant **, int, Callable::CallError &>());
				utility_invoke->setArg(0, function_name_ptr);
				utility_invoke->setArg(1, result_ptr);
				utility_invoke->setArg(2, args_array);
				utility_invoke->setArg(3, argc);
				utility_invoke->setArg(4, call_error_ptr);

				print_line("    Return:");
				print_address_info(gdscript, return_addr);

				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_CALL_UTILITY_VALIDATED: {
				int instr_arg_count = gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				int return_addr = gdscript->_code_ptr[ip];
				int argc = gdscript->_code_ptr[ip + 1];
				int utility_idx = gdscript->_code_ptr[ip + 2];
				print_line(ip - instr_arg_count - 1, "CALL_UTILITY_VALIDATED: utility_index=", utility_idx, ", argc=", argc, ", instr_arg_count=", instr_arg_count);

				Variant::ValidatedUtilityFunction utility_func = gdscript->_utilities_ptr[utility_idx];
				asmjit::x86::Gp args_array = cc.newIntPtr("validated_args_array");

				if (argc > 0) {
					int args_array_size = argc * sizeof(void *);
					asmjit::x86::Mem args_stack = cc.newStack(args_array_size, 16);
					cc.lea(args_array, args_stack);

					for (int i = 0; i < argc; i++) {
						int arg_addr = gdscript->_code_ptr[ip - argc + i];

						asmjit::x86::Gp arg_ptr = cc.newIntPtr();
						get_variant_ptr(context, arg_ptr, arg_addr);

						cc.mov(asmjit::x86::ptr(args_array, i * sizeof(void *)), arg_ptr);

						print_line("    Arg[", i, "]:");
						print_address_info(gdscript, arg_addr);
					}
				} else {
					cc.mov(args_array, 0);
					print_line("    No arguments for validated utility call");
				}

				asmjit::x86::Gp result_ptr = cc.newIntPtr("validated_utility_result_ptr");
				get_variant_ptr(context, result_ptr, return_addr);

				initialize_with_type(context, return_addr, Variant::NIL);

				asmjit::InvokeNode *utility_invoke;
				cc.invoke(&utility_invoke, utility_func, asmjit::FuncSignature::build<void, Variant *, const Variant **, int>());
				utility_invoke->setArg(0, result_ptr);
				utility_invoke->setArg(1, args_array);
				utility_invoke->setArg(2, argc);

				print_line("    Return:");
				print_address_info(gdscript, return_addr);

				incr = 3;
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

void JitCompiler::extract_arguments(JitContext &context) {
	const int PTR_SIZE = sizeof(void *);

	asmjit::x86::Gp temp_ptr = context.cc->newIntPtr("temp_ptr");
	asmjit::x86::Gp src_addr = context.cc->newIntPtr("src_addr");
	asmjit::x86::Gp dst_addr = context.cc->newIntPtr("dst_addr");

	for (int i = 0; i < context.gdscript->get_argument_count(); i++) {
		context.cc->lea(temp_ptr, asmjit::x86::ptr(context.args_ptr, i * PTR_SIZE));
		context.cc->mov(src_addr, asmjit::x86::ptr(temp_ptr));
		context.cc->lea(dst_addr, asmjit::x86::ptr(context.stack_ptr, (i + 3) * STACK_SLOT_SIZE));

		copy_variant(context, dst_addr, src_addr);
	}
}

void JitCompiler::initialize_stack(JitContext &context, asmjit::x86::Gp base_ptr, asmjit::x86::Gp class_ptr) {
	const int PTR_SIZE = sizeof(void *);
	asmjit::x86::Gp dst_addr = context.cc->newIntPtr("dst_addr");

	context.cc->lea(dst_addr, asmjit::x86::ptr(context.stack_ptr, 0 * STACK_SLOT_SIZE));
	copy_variant(context, dst_addr, base_ptr);

	context.cc->lea(dst_addr, asmjit::x86::ptr(context.stack_ptr, 1 * STACK_SLOT_SIZE));
	copy_variant(context, dst_addr, class_ptr);

	context.cc->lea(dst_addr, asmjit::x86::ptr(context.stack_ptr, 2 * STACK_SLOT_SIZE));
	copy_variant(context, dst_addr, context.result_ptr);
}

void JitCompiler::get_variant_ptr(JitContext &context, asmjit::x86::Gp &variant_ptr, int address) {
	int address_type, address_index;
	decode_address(address, address_type, address_index);

	if (address_type == GDScriptFunction::ADDR_TYPE_CONSTANT) {
		asmjit::x86::Gp constants_ptr = context.cc->newIntPtr();
		context.cc->mov(constants_ptr, (intptr_t)context.gdscript->_constants_ptr);
		context.cc->lea(variant_ptr, asmjit::x86::ptr(constants_ptr, address_index * sizeof(Variant)));
	} else if (address_type == GDScriptFunction::ADDR_TYPE_STACK) {
		context.cc->lea(variant_ptr, asmjit::x86::ptr(context.stack_ptr, address_index * STACK_SLOT_SIZE));
	} else if (address_type == GDScriptFunction::ADDR_TYPE_MEMBER) {
		context.cc->lea(variant_ptr, asmjit::x86::ptr(context.members_ptr, address_index * sizeof(Variant)));
	}
}

void JitCompiler::handle_operation(String &operation_name, JitContext &ctx, asmjit::x86::Gp &left_val, asmjit::x86::Gp &right_val, asmjit::x86::Gp &result_mem) {
	if (operation_name == "SUBTRACT_INT_INT") {
		ctx.cc->sub(left_val, right_val);
		ctx.cc->mov(result_mem, left_val);
	} else if (operation_name == "ADD_INT_INT") {
		ctx.cc->add(left_val, right_val);
		ctx.cc->mov(result_mem, left_val);
	} else if (operation_name == "MULTIPLY_INT_INT") {
		ctx.cc->imul(left_val, right_val);
		ctx.cc->mov(result_mem, left_val);
	} else if (operation_name == "EQUAL_INT_INT") {
		ctx.cc->cmp(left_val, right_val);
		asmjit::x86::Gp result_reg = ctx.cc->newInt64();
		ctx.cc->sete(result_reg.r8());
		ctx.cc->movzx(result_reg, result_reg.r8());
		ctx.cc->mov(result_mem, result_reg);
	} else if (operation_name == "LESS_INT_INT") {
		ctx.cc->cmp(left_val, right_val);
		asmjit::x86::Gp result_reg = ctx.cc->newInt64();
		ctx.cc->setl(result_reg.r8());
		ctx.cc->movzx(result_reg, result_reg.r8());
		ctx.cc->mov(result_mem, result_reg);
	} else if (operation_name == "GREATER_INT_INT") {
		ctx.cc->cmp(left_val, right_val);
		asmjit::x86::Gp result_reg = ctx.cc->newInt64();
		ctx.cc->setg(result_reg.r8());
		ctx.cc->movzx(result_reg, result_reg.r8());
		ctx.cc->mov(result_mem, result_reg);
	} else if (operation_name == "LESS_EQUAL_INT_INT") {
		ctx.cc->cmp(left_val, right_val);
		asmjit::x86::Gp result_reg = ctx.cc->newInt64();
		ctx.cc->setle(result_reg.r8());
		ctx.cc->movzx(result_reg, result_reg.r8());
		ctx.cc->mov(result_mem, result_reg);
	} else if (operation_name == "GREATER_EQUAL_INT_INT") {
		ctx.cc->cmp(left_val, right_val);
		asmjit::x86::Gp result_reg = ctx.cc->newInt64();
		ctx.cc->setge(result_reg.r8());
		ctx.cc->movzx(result_reg, result_reg.r8());
		ctx.cc->mov(result_mem, result_reg);
	} else if (operation_name == "NOT_EQUAL_INT_INT") {
		ctx.cc->cmp(left_val, right_val);
		asmjit::x86::Gp result_reg = ctx.cc->newInt64();
		ctx.cc->setne(result_reg.r8());
		ctx.cc->movzx(result_reg, result_reg.r8());
		ctx.cc->mov(result_mem, result_reg);
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

StringName JitCompiler::get_utility_function_name(int utility_idx, const GDScriptFunction *gdscript) {
	return "utility_" + String::num(utility_idx);
}

HashMap<int, asmjit::Label> JitCompiler::analyze_jump_targets(JitContext &context) {
	HashMap<int, asmjit::Label> jump_labels;

	print_line("\n=== Analyzing Jump Targets ===");

	int ip = 0;
	while (ip < context.gdscript->code.size()) {
		int incr = 0;
		GDScriptFunction::Opcode opcode = GDScriptFunction::Opcode(context.gdscript->_code_ptr[ip]);

		switch (opcode) {
			case GDScriptFunction::OPCODE_LINE:
				incr = 2;
				break;

			case GDScriptFunction::OPCODE_OPERATOR: {
				constexpr int _pointer_size = sizeof(Variant::ValidatedOperatorEvaluator) / sizeof(*context.gdscript->_code_ptr);
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
			case GDScriptFunction::OPCODE_CONSTRUCT: {
				int instr_arg_count = context.gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_CONSTRUCT_VALIDATED: {
				int instr_arg_count = context.gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_CALL_RETURN: {
				int instr_arg_count = context.gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_CALL_UTILITY: {
				int instr_arg_count = context.gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_CALL_UTILITY_VALIDATED: {
				int instr_arg_count = context.gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_JUMP: {
				int target = context.gdscript->_code_ptr[ip + 1];
				if (!jump_labels.has(target)) {
					jump_labels[target] = context.cc->newLabel();
					print_line("Created label for JUMP target: ", target);
				}
				incr = 2;
			} break;

			case GDScriptFunction::OPCODE_JUMP_IF: {
				int target = context.gdscript->_code_ptr[ip + 2];
				if (!jump_labels.has(target)) {
					jump_labels[target] = context.cc->newLabel();
					print_line("Created label for JUMP_IF target: ", target);
				}
				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_JUMP_IF_NOT: {
				int target = context.gdscript->_code_ptr[ip + 2];
				if (!jump_labels.has(target)) {
					jump_labels[target] = context.cc->newLabel();
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
				int jump_target = context.gdscript->_code_ptr[ip + 4];
				if (!jump_labels.has(jump_target)) {
					jump_labels[jump_target] = context.cc->newLabel();
					print_line("Created label for ITERATE target: ", jump_target);
				}
				incr = 5;
			} break;

			case GDScriptFunction::OPCODE_CALL_GDSCRIPT_UTILITY: {
				int instr_var_args = context.gdscript->_code_ptr[++ip];
				int argc = context.gdscript->_code_ptr[ip + 1 + instr_var_args];
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

OperatorTypes JitCompiler::get_operator_types(Variant::ValidatedOperatorEvaluator op_func) {
	if (evaluator_to_types_map.is_empty()) {
		for (int op = 0; op < Variant::OP_MAX; op++) {
			for (int type_a = 0; type_a < Variant::VARIANT_MAX; type_a++) {
				for (int type_b = 0; type_b < Variant::VARIANT_MAX; type_b++) {
					Variant::ValidatedOperatorEvaluator evaluator =
							Variant::get_validated_operator_evaluator(
									(Variant::Operator)op,
									(Variant::Type)type_a,
									(Variant::Type)type_b);

					if (evaluator != nullptr) {
						OperatorTypes types;
						types.op = (Variant::Operator)op;
						types.left_type = (Variant::Type)type_a;
						types.right_type = (Variant::Type)type_b;
						evaluator_to_types_map[evaluator] = types;
					}
				}
			}
		}
	}

	if (evaluator_to_types_map.has(op_func)) {
		return evaluator_to_types_map[op_func];
	}

	OperatorTypes unknown;
	unknown.op = Variant::OP_MAX;
	unknown.left_type = Variant::NIL;
	unknown.right_type = Variant::NIL;

	return unknown;
}

Variant::Type JitCompiler::get_result_type_for_operator(OperatorTypes types) {
	if (types.op >= Variant::OP_EQUAL && types.op <= Variant::OP_GREATER_EQUAL) {
		return Variant::BOOL;
	}

	if (types.op == Variant::OP_AND || types.op == Variant::OP_OR || types.op == Variant::OP_NOT) {
		return Variant::BOOL;
	}

	if (types.op >= Variant::OP_ADD && types.op <= Variant::OP_MODULE) {
		if (types.left_type == Variant::INT && types.right_type == Variant::INT) {
			return Variant::INT;
		}
		if (types.left_type == Variant::FLOAT || types.right_type == Variant::FLOAT) {
			return Variant::FLOAT;
		}
		return types.left_type;
	}

	if (types.op >= Variant::OP_BIT_AND && types.op <= Variant::OP_SHIFT_RIGHT) {
		return Variant::INT;
	}

	return Variant::NIL;
}

void JitCompiler::initialize_with_type(JitContext &context, int address, Variant::Type type) {
	int address_type, address_index;
	decode_address(address, address_type, address_index);

	asmjit::x86::Gp src_addr = context.cc->newIntPtr("src_addr");
	asmjit::x86::Gp dst_addr = context.cc->newIntPtr("dst_addr");

	get_variant_ptr(context, dst_addr, address);
	context.cc->mov(src_addr, context.result_ptr);

	copy_variant(context, dst_addr, src_addr);

	context.cc->mov(asmjit::x86::dword_ptr(dst_addr, 0), (int)type);
}

void JitCompiler::copy_variant(JitContext &context, asmjit::x86::Gp &dst_ptr, asmjit::x86::Gp &src_ptr) {
	asmjit::x86::Gp temp_reg64 = context.cc->newGpq("temp_reg64");

	context.cc->mov(temp_reg64, asmjit::x86::ptr(src_ptr, 0));
	context.cc->mov(asmjit::x86::ptr(dst_ptr, 0), temp_reg64);
	context.cc->mov(temp_reg64, asmjit::x86::ptr(src_ptr, 8));
	context.cc->mov(asmjit::x86::ptr(dst_ptr, 8), temp_reg64);
	context.cc->mov(temp_reg64, asmjit::x86::ptr(src_ptr, 16));
	context.cc->mov(asmjit::x86::ptr(dst_ptr, 16), temp_reg64);
}

void JitCompiler::extract_int_from_variant(JitContext &context, asmjit::x86::Gp &result_reg, int address) {
	int address_type, address_index;
	decode_address(address, address_type, address_index);

	asmjit::x86::Gp variant_ptr = context.cc->newIntPtr("variant_ptr");
	get_variant_ptr(context, variant_ptr, address);

	context.cc->mov(result_reg, asmjit::x86::qword_ptr(variant_ptr, OFFSET_INT));
}

void JitCompiler::store_reg_to_variant(JitContext &context, asmjit::x86::Gp &value, int address) {
	int address_type, address_index;
	decode_address(address, address_type, address_index);

	asmjit::x86::Gp variant_ptr = context.cc->newIntPtr("variant_ptr");
	get_variant_ptr(context, variant_ptr, address);

	context.cc->mov(asmjit::x86::qword_ptr(variant_ptr, OFFSET_INT), value);
}

void JitCompiler::store_int_to_variant(JitContext &context, int value, int address) {
	int address_type, address_index;
	decode_address(address, address_type, address_index);

	asmjit::x86::Gp variant_ptr = context.cc->newIntPtr("variant_ptr");
	get_variant_ptr(context, variant_ptr, address);

	context.cc->mov(asmjit::x86::qword_ptr(variant_ptr, OFFSET_INT), value);
}
