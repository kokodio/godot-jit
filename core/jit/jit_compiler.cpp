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

extern "C" {
void call_variant_method(Variant &base, const StringName &method_name, const Variant **args, int argc, Variant &result, Callable::CallError &error) {
	base.callp(method_name, args, argc, result, error);
}

void get_keyed(const Variant *base, const Variant *key, Variant *result, bool *valid) {
	*result = base->get(*key, valid);
}
void set_keyed(Variant *base, const Variant *key, const Variant *value, bool *valid) {
	base->set(*key, *value, valid);
}
}

JitCompiler *JitCompiler::singleton = nullptr;
HashMap<intptr_t, OpInfo> JitCompiler::op_map;

JitCompiler::JitCompiler() {
	register_op(Variant::OP_ADD, Variant::INT, Variant::INT);
	register_op(Variant::OP_SUBTRACT, Variant::INT, Variant::INT);
	register_op(Variant::OP_MULTIPLY, Variant::INT, Variant::INT);
	register_op(Variant::OP_EQUAL, Variant::INT, Variant::INT);
	register_op(Variant::OP_NOT_EQUAL, Variant::INT, Variant::INT);
	register_op(Variant::OP_LESS, Variant::INT, Variant::INT);
	register_op(Variant::OP_LESS_EQUAL, Variant::INT, Variant::INT);
	register_op(Variant::OP_GREATER, Variant::INT, Variant::INT);
	register_op(Variant::OP_GREATER_EQUAL, Variant::INT, Variant::INT);

	register_op(Variant::OP_ADD, Variant::INT, Variant::FLOAT);
	register_op(Variant::OP_ADD, Variant::FLOAT, Variant::INT);
	register_op(Variant::OP_SUBTRACT, Variant::INT, Variant::FLOAT);
	register_op(Variant::OP_SUBTRACT, Variant::FLOAT, Variant::INT);
	register_op(Variant::OP_MULTIPLY, Variant::INT, Variant::FLOAT);
	register_op(Variant::OP_MULTIPLY, Variant::FLOAT, Variant::INT);
	register_op(Variant::OP_DIVIDE, Variant::INT, Variant::FLOAT);
	register_op(Variant::OP_DIVIDE, Variant::FLOAT, Variant::INT);

	register_op(Variant::OP_EQUAL, Variant::INT, Variant::FLOAT);
	register_op(Variant::OP_EQUAL, Variant::FLOAT, Variant::INT);
	register_op(Variant::OP_NOT_EQUAL, Variant::INT, Variant::FLOAT);
	register_op(Variant::OP_NOT_EQUAL, Variant::FLOAT, Variant::INT);
	register_op(Variant::OP_LESS, Variant::INT, Variant::FLOAT);
	register_op(Variant::OP_LESS, Variant::FLOAT, Variant::INT);
	register_op(Variant::OP_LESS_EQUAL, Variant::INT, Variant::FLOAT);
	register_op(Variant::OP_LESS_EQUAL, Variant::FLOAT, Variant::INT);
	register_op(Variant::OP_GREATER, Variant::INT, Variant::FLOAT);
	register_op(Variant::OP_GREATER, Variant::FLOAT, Variant::INT);
	register_op(Variant::OP_GREATER_EQUAL, Variant::INT, Variant::FLOAT);
	register_op(Variant::OP_GREATER_EQUAL, Variant::FLOAT, Variant::INT);

	register_op(Variant::OP_ADD, Variant::FLOAT, Variant::FLOAT);
	register_op(Variant::OP_SUBTRACT, Variant::FLOAT, Variant::FLOAT);
	register_op(Variant::OP_MULTIPLY, Variant::FLOAT, Variant::FLOAT);
	register_op(Variant::OP_DIVIDE, Variant::FLOAT, Variant::FLOAT);
	register_op(Variant::OP_EQUAL, Variant::FLOAT, Variant::FLOAT);
	register_op(Variant::OP_NOT_EQUAL, Variant::FLOAT, Variant::FLOAT);
	register_op(Variant::OP_LESS, Variant::FLOAT, Variant::FLOAT);
	register_op(Variant::OP_LESS_EQUAL, Variant::FLOAT, Variant::FLOAT);
	register_op(Variant::OP_GREATER, Variant::FLOAT, Variant::FLOAT);
	register_op(Variant::OP_GREATER_EQUAL, Variant::FLOAT, Variant::FLOAT);

	register_op(Variant::OP_MULTIPLY, Variant::VECTOR2, Variant::FLOAT);
	register_op(Variant::OP_MULTIPLY, Variant::FLOAT, Variant::VECTOR2);
	register_op(Variant::OP_MULTIPLY, Variant::VECTOR2, Variant::INT);
	register_op(Variant::OP_MULTIPLY, Variant::INT, Variant::VECTOR2);
	register_op(Variant::OP_MULTIPLY, Variant::VECTOR2, Variant::VECTOR2);

	register_op(Variant::OP_ADD, Variant::VECTOR2, Variant::FLOAT);
	register_op(Variant::OP_ADD, Variant::FLOAT, Variant::VECTOR2);
	register_op(Variant::OP_ADD, Variant::VECTOR2, Variant::INT);
	register_op(Variant::OP_ADD, Variant::INT, Variant::VECTOR2);
	register_op(Variant::OP_ADD, Variant::VECTOR2, Variant::VECTOR2);

	register_op(Variant::OP_SUBTRACT, Variant::VECTOR2, Variant::FLOAT);
	register_op(Variant::OP_SUBTRACT, Variant::FLOAT, Variant::VECTOR2);
	register_op(Variant::OP_SUBTRACT, Variant::VECTOR2, Variant::INT);
	register_op(Variant::OP_SUBTRACT, Variant::INT, Variant::VECTOR2);
	register_op(Variant::OP_SUBTRACT, Variant::VECTOR2, Variant::VECTOR2);

	register_op(Variant::OP_DIVIDE, Variant::VECTOR2, Variant::FLOAT);
	register_op(Variant::OP_DIVIDE, Variant::FLOAT, Variant::VECTOR2);
	register_op(Variant::OP_DIVIDE, Variant::VECTOR2, Variant::INT);
	register_op(Variant::OP_DIVIDE, Variant::INT, Variant::VECTOR2);
	register_op(Variant::OP_DIVIDE, Variant::VECTOR2, Variant::VECTOR2);

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

void JitCompiler::register_op(Variant::Operator op, Variant::Type left_type, Variant::Type right_type) {
	op_map[(intptr_t)Variant::get_validated_operator_evaluator(op, left_type, right_type)] = OpInfo{ op, left_type, right_type };
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
	auto start = OS::get_singleton()->get_ticks_usec();
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

	asmjit::FuncNode *funcNode = cc.addFunc(sig);

	Gp result_ptr = cc.newIntPtr("result_ptr");
	Gp args_ptr = cc.newIntPtr("args_ptr");
	Gp stack_ptr = cc.newIntPtr("stack_ptr");
	Gp members_ptr = cc.newIntPtr("members_ptr");

	funcNode->setArg(0, result_ptr);
	funcNode->setArg(1, args_ptr);
	funcNode->setArg(2, stack_ptr);
	funcNode->setArg(3, members_ptr);

	JitContext context;
	context.gdscript = gdscript;
	context.args_ptr = args_ptr;
	context.cc = &cc;
	context.result_ptr = result_ptr;
	context.stack_ptr = stack_ptr;
	context.members_ptr = members_ptr;
	context.constants_ptr = cc.newIntPtr("constants_ptr");
	cc.mov(context.constants_ptr, gdscript->_constants_ptr);

	auto analysis = analyze_function(context);
	initialize_context(context, analysis);

	print_line("\n=== Bytecode Analysis ===");
	int ip = 0;
	while (ip < gdscript->code.size()) {
		if (analysis.jump_labels.has(ip)) {
			cc.bind(analysis.jump_labels[ip]);
			print_line(">>> Label bound at position: ", ip);
		}

		int incr = 0;
		GDScriptFunction::Opcode opcode = GDScriptFunction::Opcode(gdscript->_code_ptr[ip]);
		switch (opcode) {
			case GDScriptFunction::OPCODE_OPERATOR: { //NOT OPTIMIZED :-(
				constexpr int _pointer_size = sizeof(Variant::ValidatedOperatorEvaluator) / sizeof(*gdscript->_code_ptr);
				int left_addr = gdscript->_code_ptr[ip + 1];
				int right_addr = gdscript->_code_ptr[ip + 2];
				int result_addr = gdscript->_code_ptr[ip + 3];
				Variant::Operator operation = (Variant::Operator)gdscript->_code_ptr[ip + 4];

				Gp left_ptr = get_variant_ptr(context, left_addr);
				Gp right_ptr = get_variant_ptr(context, right_addr);
				Gp dst_ptr = get_variant_ptr(context, result_addr);

				Gp op_signature = cc.newInt32("op_signature");
				cc.mov(op_signature, gdscript->_code_ptr[ip + 5]);

				Gp left_type = cc.newInt32("left_type");
				Gp right_type = cc.newInt32("right_type");
				Gp actual_signature = cc.newInt32("actual_signature");

				cc.mov(left_type, asmjit::x86::dword_ptr(left_ptr, 0));
				cc.mov(right_type, asmjit::x86::dword_ptr(right_ptr, 0));

				cc.shl(left_type, 8);
				cc.or_(left_type, right_type);
				cc.mov(actual_signature, left_type);

				asmjit::Label cached_path = cc.newLabel();
				asmjit::Label slow_path = cc.newLabel();
				asmjit::Label end_label = cc.newLabel();

				cc.cmp(op_signature, 0);
				cc.je(slow_path);

				cc.cmp(op_signature, actual_signature);
				cc.je(cached_path);

				cc.bind(slow_path);
				{
					cc.mov(asmjit::x86::byte_ptr(context.bool_ptr), 1);
					cc.mov(asmjit::x86::dword_ptr(context.operator_ptr), operation);

					asmjit::InvokeNode *evaluate_invoke;
					cc.invoke(&evaluate_invoke, static_cast<void (*)(const Variant::Operator &, const Variant &, const Variant &, Variant &, bool &)>(&Variant::evaluate),
							asmjit::FuncSignature::build<void, const Variant::Operator &, const Variant &, const Variant &, Variant &, bool &>());
					evaluate_invoke->setArg(0, context.operator_ptr);
					evaluate_invoke->setArg(1, left_ptr);
					evaluate_invoke->setArg(2, right_ptr);
					evaluate_invoke->setArg(3, dst_ptr);
					evaluate_invoke->setArg(4, context.bool_ptr);

					cc.jmp(end_label);
				}

				cc.bind(cached_path);
				{
					Gp ret_type = cc.newInt32("ret_type");
					cc.mov(ret_type, gdscript->_code_ptr[ip + 6]);

					Gp op_func = cc.newIntPtr("op_func");
					cc.mov(op_func, (intptr_t)(*reinterpret_cast<Variant::ValidatedOperatorEvaluator *>(&gdscript->_code_ptr[ip + 7])));

					asmjit::InvokeNode *init_invoke;
					cc.invoke(&init_invoke,
							static_cast<void (*)(Variant *, Variant::Type)>([](Variant *dst, Variant::Type type) {
								VariantInternal::initialize(dst, type);
							}),
							asmjit::FuncSignature::build<void, Variant *, Variant::Type>());
					init_invoke->setArg(0, dst_ptr);
					init_invoke->setArg(1, ret_type);

					asmjit::InvokeNode *op_invoke;
					cc.invoke(&op_invoke, op_func, asmjit::FuncSignature::build<void, const Variant *, const Variant *, Variant *>());
					op_invoke->setArg(0, left_ptr);
					op_invoke->setArg(1, right_ptr);
					op_invoke->setArg(2, dst_ptr);
				}

				cc.bind(end_label);

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

				OpInfo operation = get_operator_info((intptr_t)op_func);

				if (operation.left_type == Variant::VECTOR2 || operation.right_type == Variant::VECTOR2) {
					handle_vector2_operation(operation, context, left_addr, right_addr, result_addr);
				} else if (operation.left_type == Variant::FLOAT || operation.right_type == Variant::FLOAT) {
					handle_float_operation(operation, context, left_addr, right_addr, result_addr);
				} else if (operation.left_type == Variant::INT && operation.right_type == Variant::INT) {
					handle_int_operation(operation, context, left_addr, right_addr, result_addr);
				} else {
					Gp left_ptr = get_variant_ptr(context, left_addr);
					Gp right_ptr = get_variant_ptr(context, right_addr);
					Gp op_ptr = get_variant_ptr(context, result_addr);

					asmjit::InvokeNode *op_invoke;
					cc.invoke(&op_invoke, op_func, asmjit::FuncSignature::build<void, const Variant *, const Variant *, Variant *>());
					op_invoke->setArg(0, left_ptr);
					op_invoke->setArg(1, right_ptr);
					op_invoke->setArg(2, op_ptr);
				}

				print_line(ip, "OPERATOR_VALIDATED: ", operation.op != Variant::OP_MAX ? Variant::get_operator_name(operation.op) : "UNKNOWN",
						", left_type=", Variant::get_type_name(operation.left_type),
						", right_type=", Variant::get_type_name(operation.right_type),
						", function index: ", operation_idx);

				print_line("    Left operand:");
				print_address_info(gdscript, left_addr);
				print_line("    Right operand:");
				print_address_info(gdscript, right_addr);
				print_line("    Result:");
				print_address_info(gdscript, result_addr);

				incr = 5;
			} break;

			case GDScriptFunction::OPCODE_SET_KEYED: {
				int base_addr = gdscript->_code_ptr[ip + 1];
				int key_addr = gdscript->_code_ptr[ip + 2];
				int value_addr = gdscript->_code_ptr[ip + 3];

				print_line(ip, "SET_KEYED");

				Gp base_ptr = get_variant_ptr(context, base_addr);
				Gp key_ptr = get_variant_ptr(context, key_addr);
				Gp value_ptr = get_variant_ptr(context, value_addr);

				cc.mov(asmjit::x86::byte_ptr(context.bool_ptr), 1);

				asmjit::InvokeNode *set_invoke;
				cc.invoke(&set_invoke, &set_keyed,
						asmjit::FuncSignature::build<void, Variant *, const Variant *, const Variant *, bool *>());
				set_invoke->setArg(0, base_ptr);
				set_invoke->setArg(1, key_ptr);
				set_invoke->setArg(2, value_ptr);
				set_invoke->setArg(3, context.bool_ptr);

				print_line("    Base address:");
				print_address_info(gdscript, base_addr);
				print_line("    Key:");
				print_address_info(gdscript, key_addr);
				print_line("    Value:");
				print_address_info(gdscript, value_addr);

				incr = 4;
			} break;

			case GDScriptFunction::OPCODE_SET_INDEXED_VALIDATED: {
				int base_addr = gdscript->_code_ptr[ip + 1];
				int index_addr = gdscript->_code_ptr[ip + 2];
				int value_addr = gdscript->_code_ptr[ip + 3];
				int setter_idx = gdscript->_code_ptr[ip + 4];

				Variant::ValidatedIndexedSetter setter_func = gdscript->_indexed_setters_ptr[setter_idx];
				print_line(ip, "SET_INDEXED_VALIDATED: setter_idx=", setter_idx);

				Gp base_ptr = get_variant_ptr(context, base_addr);
				Gp value_ptr = get_variant_ptr(context, value_addr);
				Gp index_val = extract_int_from_variant(context, index_addr);

				cc.mov(asmjit::x86::byte_ptr(context.bool_ptr), 0);

				asmjit::InvokeNode *setter_invoke;
				cc.invoke(&setter_invoke, setter_func,
						asmjit::FuncSignature::build<void, Variant *, int64_t, const Variant *, bool *>());
				setter_invoke->setArg(0, base_ptr);
				setter_invoke->setArg(1, index_val);
				setter_invoke->setArg(2, value_ptr);
				setter_invoke->setArg(3, context.bool_ptr);

				print_line("    Base address:");
				print_address_info(gdscript, base_addr);
				print_line("    Index:");
				print_address_info(gdscript, index_addr);
				print_line("    Value:");
				print_address_info(gdscript, value_addr);

				incr = 5;
			} break;

			case GDScriptFunction::OPCODE_GET_KEYED: {
				int base_addr = gdscript->_code_ptr[ip + 1];
				int key_addr = gdscript->_code_ptr[ip + 2];
				int result_addr = gdscript->_code_ptr[ip + 3];

				print_line(ip, "GET_KEYED");

				Gp base_ptr = get_variant_ptr(context, base_addr);
				Gp key_ptr = get_variant_ptr(context, key_addr);
				Gp dst_ptr = get_variant_ptr(context, result_addr);

				cc.mov(asmjit::x86::byte_ptr(context.bool_ptr), 1);

				asmjit::InvokeNode *get_invoke;
				cc.invoke(&get_invoke, &get_keyed, asmjit::FuncSignature::build<void, const Variant *, const Variant *, Variant *, bool *>());
				get_invoke->setArg(0, base_ptr);
				get_invoke->setArg(1, key_ptr);
				get_invoke->setArg(2, dst_ptr);
				get_invoke->setArg(3, context.bool_ptr);

				print_line("    Base address:");
				print_address_info(gdscript, base_addr);
				print_line("    Key:");
				print_address_info(gdscript, key_addr);
				print_line("    Result:");
				print_address_info(gdscript, result_addr);

				incr = 4;
			} break;

			case GDScriptFunction::OPCODE_GET_INDEXED_VALIDATED: {
				int base_addr = gdscript->_code_ptr[ip + 1];
				int index_addr = gdscript->_code_ptr[ip + 2];
				int result_addr = gdscript->_code_ptr[ip + 3];
				int getter_idx = gdscript->_code_ptr[ip + 4];

				Variant::ValidatedIndexedGetter getter_func = gdscript->_indexed_getters_ptr[getter_idx];
				print_line(ip, "GET_INDEXED_VALIDATED: getter_idx=", getter_idx);

				Gp base_ptr = get_variant_ptr(context, base_addr);
				Gp dst_ptr = get_variant_ptr(context, result_addr);

				Gp index_val = extract_int_from_variant(context, index_addr);

				cc.mov(asmjit::x86::byte_ptr(context.bool_ptr), 0);

				asmjit::InvokeNode *getter_invoke;
				cc.invoke(&getter_invoke, getter_func,
						asmjit::FuncSignature::build<void, const Variant *, int64_t, Variant *, bool *>());
				getter_invoke->setArg(0, base_ptr);
				getter_invoke->setArg(1, index_val);
				getter_invoke->setArg(2, dst_ptr);
				getter_invoke->setArg(3, context.bool_ptr);

				print_line("    Base address:");
				print_address_info(gdscript, base_addr);
				print_line("    Index:");
				print_address_info(gdscript, index_addr);
				print_line("    Result:");
				print_address_info(gdscript, result_addr);

				incr = 5;
			} break;

			case GDScriptFunction::OPCODE_SET_NAMED: {
				int base_addr = gdscript->_code_ptr[ip + 1];
				int value_addr = gdscript->_code_ptr[ip + 2];
				int name_idx = gdscript->_code_ptr[ip + 3];

				print_line(ip, "SET_NAMED: name_idx=", name_idx);

				Gp base_ptr = get_variant_ptr(context, base_addr);
				Gp value_ptr = get_variant_ptr(context, value_addr);

				cc.mov(asmjit::x86::byte_ptr(context.bool_ptr), 1);

				asmjit::InvokeNode *set_invoke;
				cc.invoke(&set_invoke,
						static_cast<void (*)(Variant *, const StringName &, const Variant &, bool &)>([](Variant *base, const StringName &name, const Variant &value, bool &valid) {
							base->set_named(name, value, valid);
						}),
						asmjit::FuncSignature::build<void, Variant *, const StringName &, const Variant &, bool &>());
				set_invoke->setArg(0, base_ptr);
				set_invoke->setArg(1, &gdscript->_global_names_ptr[name_idx]);
				set_invoke->setArg(2, value_ptr);
				set_invoke->setArg(3, context.bool_ptr);

				print_line("    Base address:");
				print_address_info(gdscript, base_addr);
				print_line("    Value:");
				print_address_info(gdscript, value_addr);
				print_line("    Name: ", gdscript->_global_names_ptr[name_idx]);

				incr = 4;
			} break;

			case GDScriptFunction::OPCODE_SET_NAMED_VALIDATED: {
				int base_addr = gdscript->_code_ptr[ip + 1];
				int value_addr = gdscript->_code_ptr[ip + 2];
				int name_idx = gdscript->_code_ptr[ip + 3];

				Variant::ValidatedSetter setter_func = gdscript->_setters_ptr[name_idx];
				print_line(ip, "SET_NAMED_VALIDATED: ", name_idx);

				Gp base_ptr = get_variant_ptr(context, base_addr);
				Gp value_ptr = get_variant_ptr(context, value_addr);

				asmjit::InvokeNode *setter_invoke;
				cc.invoke(&setter_invoke, setter_func,
						asmjit::FuncSignature::build<void, Variant *, const Variant *>());
				setter_invoke->setArg(0, base_ptr);
				setter_invoke->setArg(1, value_ptr);

				print_line("    Base address:");
				print_address_info(gdscript, base_addr);
				print_line("    Value:");
				print_address_info(gdscript, value_addr);

				incr = 4;
			} break;

			case GDScriptFunction::OPCODE_GET_NAMED: {
				int base_addr = gdscript->_code_ptr[ip + 1];
				int result_addr = gdscript->_code_ptr[ip + 2];
				int name_idx = gdscript->_code_ptr[ip + 3];

				print_line(ip, "GET_NAMED: name_idx=", name_idx);

				Gp base_ptr = get_variant_ptr(context, base_addr);
				Gp value_ptr = get_variant_ptr(context, result_addr);

				cc.mov(asmjit::x86::byte_ptr(context.bool_ptr), 1);

				asmjit::InvokeNode *get_invoke;
				cc.invoke(&get_invoke,
						static_cast<void (*)(const Variant *, const StringName &, Variant *, bool &)>([](const Variant *base, const StringName &name, Variant *result, bool &valid) {
							*result = base->get_named(name, valid);
						}),
						asmjit::FuncSignature::build<void, const Variant *, const StringName &, Variant *, bool &>());
				get_invoke->setArg(0, base_ptr);
				get_invoke->setArg(1, &gdscript->_global_names_ptr[name_idx]);
				get_invoke->setArg(2, value_ptr);
				get_invoke->setArg(3, context.bool_ptr);

				print_line("    Base address:");
				print_address_info(gdscript, base_addr);
				print_line("    Result:");
				print_address_info(gdscript, result_addr);
				print_line("    Name: ", gdscript->_global_names_ptr[name_idx]);

				incr = 4;
			} break;

			case GDScriptFunction::OPCODE_GET_NAMED_VALIDATED: {
				int base_addr = gdscript->_code_ptr[ip + 1];
				int result_addr = gdscript->_code_ptr[ip + 2];
				int name_idx = gdscript->_code_ptr[ip + 3];

				Variant::ValidatedGetter getter_func = gdscript->_getters_ptr[name_idx];
				print_line(ip, "GET_NAMED_VALIDATED: ", name_idx);

				Gp base_ptr = get_variant_ptr(context, base_addr);
				Gp value_ptr = get_variant_ptr(context, result_addr);

				asmjit::InvokeNode *getter_invoke;
				cc.invoke(&getter_invoke, getter_func,
						asmjit::FuncSignature::build<void, const Variant *, Variant *>());
				getter_invoke->setArg(0, base_ptr);
				getter_invoke->setArg(1, value_ptr);

				print_line("    Base address:");
				print_address_info(gdscript, base_addr);
				print_line("    Result:");
				print_address_info(gdscript, result_addr);

				incr = 4;
			} break;

			case GDScriptFunction::OPCODE_SET_STATIC_VARIABLE: {
				int value_addr = gdscript->_code_ptr[ip + 1];
				int _class = gdscript->_code_ptr[ip + 2];
				int idx = gdscript->_code_ptr[ip + 3];

				Gp value_ptr = get_variant_ptr(context, value_addr);
				Gp class_ptr = get_variant_ptr(context, _class);

				asmjit::InvokeNode *set_static_invoke;
				cc.invoke(&set_static_invoke,
						static_cast<void (*)(Variant *, Variant *, int)>(
								[](Variant *value, Variant *class_p, int index) {
									GDScript *script_p = Object::cast_to<GDScript>(class_p->operator Object *());

									script_p->static_variables.write[index] = *value;
								}),
						asmjit::FuncSignature::build<void, Variant *, Variant *, int>());

				set_static_invoke->setArg(0, value_ptr);
				set_static_invoke->setArg(1, class_ptr);
				set_static_invoke->setArg(2, idx);

				print_line(ip, "SET_STATIC_VARIABLE: class=", _class, ", index=", idx);
				print_line("    Value:");
				print_address_info(gdscript, value_addr);

				incr = 4;
			} break;

			case GDScriptFunction::OPCODE_GET_STATIC_VARIABLE: {
				int dst_addr = gdscript->_code_ptr[ip + 1];
				int _class = gdscript->_code_ptr[ip + 2];
				int idx = gdscript->_code_ptr[ip + 3];

				Gp dst_ptr = get_variant_ptr(context, dst_addr);
				Gp class_ptr = get_variant_ptr(context, _class);

				asmjit::InvokeNode *get_static_invoke;
				cc.invoke(&get_static_invoke,
						static_cast<void (*)(Variant *, Variant *, int)>(
								[](Variant *dst, Variant *class_p, int index) {
									GDScript *script_p = Object::cast_to<GDScript>(class_p->operator Object *());

									*dst = script_p->static_variables[index];
								}),
						asmjit::FuncSignature::build<void, Variant *, Variant *, int>());

				get_static_invoke->setArg(0, dst_ptr);
				get_static_invoke->setArg(1, class_ptr);
				get_static_invoke->setArg(2, idx);

				print_line(ip, "GET_STATIC_VARIABLE: class=", _class, ", index=", idx);
				print_line("    Destination:");
				print_address_info(gdscript, dst_addr);

				incr = 4;
			} break;

			case GDScriptFunction::OPCODE_ASSIGN: {
				int dst_addr = gdscript->_code_ptr[ip + 1];
				int src_addr = gdscript->_code_ptr[ip + 2];

				Gp src_ptr = get_variant_ptr(context, src_addr);
				Gp dst_ptr = get_variant_ptr(context, dst_addr);

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

				print_line(ip, "ASSIGN_NULL");

				Gp dst_ptr = get_variant_ptr(context, dst_addr);

				asmjit::InvokeNode *assign_invoke;
				cc.invoke(&assign_invoke,
						static_cast<void (*)(Variant *)>([](Variant *dst) {
							*dst = Variant();
						}),
						asmjit::FuncSignature::build<void, Variant *>());
				assign_invoke->setArg(0, dst_ptr);

				print_line("    Destination:");
				print_address_info(gdscript, dst_addr);

				incr = 2;
			} break;
			case GDScriptFunction::OPCODE_ASSIGN_TRUE: {
				int dst_addr = gdscript->_code_ptr[ip + 1];

				int dst_type, dst_index;
				decode_address(dst_addr, dst_type, dst_index);

				print_line("Not implemented: OPCODE_ASSIGN_TRUE");

				//idk where its used

				print_line(ip, " ASSIGN_TRUE");
				incr = 2;
			} break;
			case GDScriptFunction::OPCODE_ASSIGN_FALSE: {
				int dst_addr = gdscript->_code_ptr[ip + 1];

				int dst_type, dst_index;
				decode_address(dst_addr, dst_type, dst_index);

				print_line("Not implemented: OPCODE_ASSIGN_FALSE");

				//idk where its used

				print_line(ip, " ASSIGN_FALSE");
				incr = 2;
			} break;
			case GDScriptFunction::OPCODE_ASSIGN_TYPED_BUILTIN: {
				int dst_addr = gdscript->_code_ptr[ip + 1];
				int src_addr = gdscript->_code_ptr[ip + 2];
				Variant::Type target_type = (Variant::Type)gdscript->_code_ptr[ip + 3];

				//Gp src_ptr = get_variant_ptr(context, src_addr);
				Gp dst_ptr = get_variant_ptr(context, dst_addr);
				Gp arg_ptr = get_variant_ptr(context, src_addr);

				Gp args_array = cc.newIntPtr("args_array");
				cc.lea(args_array, cc.newStack(PTR_SIZE, 16));

				cc.mov(Arch::ptr(args_array, 0), arg_ptr);

				Gp call_error_ptr = get_call_error_ptr(context);

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

			case GDScriptFunction::OPCODE_CAST_TO_SCRIPT: {
				int src_addr = gdscript->_code_ptr[ip + 1];
				int dst_addr = gdscript->_code_ptr[ip + 2];
				int to_type = gdscript->_code_ptr[ip + 3];

				Gp src_ptr = get_variant_ptr(context, src_addr);
				Gp dst_ptr = get_variant_ptr(context, dst_addr);
				Gp script_ptr = get_variant_ptr(context, to_type);

				asmjit::InvokeNode *cast_invoke;
				cc.invoke(&cast_invoke,
						static_cast<void (*)(const Variant *, Variant *, const Variant *)>(
								[](const Variant *src, Variant *dst, const Variant *script_p) {
									Script *base_type = Object::cast_to<Script>(script_p->operator Object *());
									bool valid = false;

									if (src->get_type() != Variant::NIL && src->operator Object *() != nullptr) {
										ScriptInstance *scr_inst = src->operator Object *()->get_script_instance();

										if (scr_inst) {
											Script *src_type = src->operator Object *()->get_script_instance()->get_script().ptr();

											while (src_type) {
												if (src_type == base_type) {
													valid = true;
													break;
												}
												src_type = src_type->get_base_script().ptr();
											}
										}
									}

									if (valid) {
										*dst = *src;
									} else {
										*dst = Variant();
									}
								}),
						asmjit::FuncSignature::build<void, const Variant *, Variant *, const Variant *>());
				cast_invoke->setArg(0, src_ptr);
				cast_invoke->setArg(1, dst_ptr);
				cast_invoke->setArg(2, script_ptr);

				print_line(ip, "CAST_TO_SCRIPT: to_type=", to_type);
				print_line("    Source:");
				print_address_info(gdscript, src_addr);
				print_line("    Destination:");
				print_address_info(gdscript, dst_addr);
				print_line("    Script type address:");
				print_address_info(gdscript, to_type);

				incr = 4;
			} break;

			case GDScriptFunction::OPCODE_CONSTRUCT: {
				int instr_arg_count = gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				int dst_addr = gdscript->_code_ptr[ip];
				int argc = gdscript->_code_ptr[ip + 1];
				Variant::Type construct_type = (Variant::Type)gdscript->_code_ptr[ip + 2];

				print_line(ip - instr_arg_count - 1, "CONSTRUCT: ", Variant::get_type_name(construct_type), ", argc=", argc);

				Gp dst_ptr = get_variant_ptr(context, dst_addr);

				Gp args_array = prepare_args_array(context, argc, ip - argc);
				Gp call_error_ptr = get_call_error_ptr(context);

				asmjit::InvokeNode *construct_invoke;
				cc.invoke(&construct_invoke, &Variant::construct, asmjit::FuncSignature::build<void, Variant::Type, Variant &, const Variant **, int, Callable::CallError &>());
				construct_invoke->setArg(0, construct_type);
				construct_invoke->setArg(1, dst_ptr);
				construct_invoke->setArg(2, args_array);
				construct_invoke->setArg(3, argc);
				construct_invoke->setArg(4, call_error_ptr);

				print_line("    Result:");
				print_address_info(gdscript, dst_addr);

				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_CONSTRUCT_VALIDATED: {
				int instr_arg_count = gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				int dst_addr = gdscript->_code_ptr[ip];
				int argc = gdscript->_code_ptr[ip + 1];
				int constructor_idx = gdscript->_code_ptr[ip + 2];

				Variant::ValidatedConstructor constructor = gdscript->_constructors_ptr[constructor_idx];

				print_line(ip - instr_arg_count - 1, "CONSTRUCT_VALIDATED: constructor_idx=", constructor_idx, ", argc=", argc);

				Gp args_array = prepare_args_array(context, argc, ip - argc);

				Gp dst_ptr = get_variant_ptr(context, dst_addr);

				asmjit::InvokeNode *construct_invoke;
				cc.invoke(&construct_invoke, constructor, asmjit::FuncSignature::build<void, Variant *, const Variant **>());
				construct_invoke->setArg(0, dst_ptr);
				construct_invoke->setArg(1, args_array);

				print_line("    Result:");
				print_address_info(gdscript, dst_addr);

				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_CONSTRUCT_ARRAY: {
				int instr_arg_count = gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				int dst_addr = gdscript->_code_ptr[ip];
				int argc = gdscript->_code_ptr[ip + 1];

				print_line(ip, "CONSTRUCT_ARRAY, argc=", argc);

				Gp args_array = prepare_args_array(context, argc, ip - argc);
				Gp dst_ptr = get_variant_ptr(context, dst_addr);

				asmjit::InvokeNode *construct_invoke;
				cc.invoke(&construct_invoke,
						static_cast<void (*)(Variant *, Variant **, int)>(
								[](Variant *dst, Variant **args, int argcount) {
									Array array;
									array.resize(argcount);
									for (int i = 0; i < argcount; i++) {
										array[i] = *args[i];
									}
									*dst = Variant();
									*dst = array;
								}),
						asmjit::FuncSignature::build<void, Variant *, Variant **, int>());

				construct_invoke->setArg(0, dst_ptr);
				construct_invoke->setArg(1, args_array);
				construct_invoke->setArg(2, argc);

				print_line("    Result:");
				print_address_info(gdscript, dst_addr);

				incr = 2;
			} break;

			case GDScriptFunction::OPCODE_CONSTRUCT_TYPED_ARRAY: {
				int instr_arg_count = gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				int argc = gdscript->_code_ptr[ip + 1];
				Variant::Type builtin_type = (Variant::Type)gdscript->_code_ptr[ip + 2];
				int native_type_idx = gdscript->_code_ptr[ip + 3];
				int script_type_addr = gdscript->_code_ptr[ip - argc - 1];
				int dst_addr = gdscript->_code_ptr[ip - 1];

				const StringName native_type = gdscript->_global_names_ptr[native_type_idx];

				print_line(ip, "CONSTRUCT_TYPED_ARRAY, argc=", argc, ", builtin_type=", Variant::get_type_name(builtin_type), ", native_type_idx=", native_type_idx, ", script_type_addr=", script_type_addr);

				Gp args_array = prepare_args_array(context, argc, ip - argc);
				Gp dst_ptr = get_variant_ptr(context, dst_addr);
				Gp script_type_ptr = get_variant_ptr(context, script_type_addr);

				asmjit::InvokeNode *construct_invoke;
				cc.invoke(&construct_invoke,
						static_cast<void (*)(Variant *, Variant **, int, Variant *, int, const StringName *)>(
								[](Variant *dst, Variant **args, int argcount, Variant *script_type, int builtin, const StringName *native) {
									Array array;
									array.resize(argcount);
									for (int i = 0; i < argcount; i++) {
										array[i] = *args[i];
									}
									*dst = Variant();

									StringName class_name = ((Variant::Type)builtin == Variant::OBJECT) ? *native : class_name;
									*dst = Array(array, (Variant::Type)builtin, class_name, *script_type);
								}),
						asmjit::FuncSignature::build<void, Variant *, Variant **, int, Variant *, int, const StringName *>());

				construct_invoke->setArg(0, dst_ptr);
				construct_invoke->setArg(1, args_array);
				construct_invoke->setArg(2, argc);
				construct_invoke->setArg(3, script_type_ptr);
				construct_invoke->setArg(4, builtin_type);
				construct_invoke->setArg(5, &native_type);

				print_line("    Result:");
				print_address_info(gdscript, dst_addr);

				incr = 4;
			} break;

			case GDScriptFunction::OPCODE_CALL:
			case GDScriptFunction::OPCODE_CALL_RETURN: {
				int instr_arg_count = gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				int base_addr = gdscript->_code_ptr[ip - 1];
				int dst_addr = gdscript->_code_ptr[ip];
				int argc = gdscript->_code_ptr[ip + 1];
				int function_name_idx = gdscript->_code_ptr[ip + 2];

				StringName function_name = gdscript->_global_names_ptr[function_name_idx];
				print_line(ip - instr_arg_count - 1, "CALL_RETURN: ", function_name, ", argc=", argc, ", instr_arg_count=", instr_arg_count);

				Gp base_ptr = get_variant_ptr(context, base_addr);
				Gp dst_ptr = get_variant_ptr(context, dst_addr);

				Gp args_array = prepare_args_array(context, argc, ip - instr_arg_count + 1);
				Gp call_error_ptr = get_call_error_ptr(context);

				Gp function_name_ptr = cc.newIntPtr("function_name_ptr");
				cc.mov(function_name_ptr, &gdscript->_global_names_ptr[function_name_idx]);

				asmjit::InvokeNode *call_invoke;
				cc.invoke(&call_invoke, &call_variant_method,
						asmjit::FuncSignature::build<void, const Variant &, const StringName &, const Variant **, int, Variant &, Callable::CallError &>());

				call_invoke->setArg(0, base_ptr);
				call_invoke->setArg(1, function_name_ptr);
				call_invoke->setArg(2, args_array);
				call_invoke->setArg(3, argc);
				call_invoke->setArg(4, dst_ptr);
				call_invoke->setArg(5, call_error_ptr);

				print_line("    Return value:");
				print_address_info(gdscript, dst_addr);
				print_line("    Base adress:");
				print_address_info(gdscript, base_addr);

				incr = 3;

			} break;

			case GDScriptFunction::OPCODE_CALL_UTILITY: {
				int instr_arg_count = gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				int dst_addr = gdscript->_code_ptr[ip];
				int argc = gdscript->_code_ptr[ip + 1];
				int utility_name_idx = gdscript->_code_ptr[ip + 2];
				StringName function_name = gdscript->_global_names_ptr[utility_name_idx];
				print_line(ip - instr_arg_count - 1, "CALL_UTILITY: ", function_name, ", argc=", argc, ", instr_arg_count=", instr_arg_count);

				Gp args_array = prepare_args_array(context, argc, ip - argc);

				Gp dst_ptr = get_variant_ptr(context, dst_addr);

				Gp function_name_ptr = cc.newIntPtr("function_name_ptr");
				cc.mov(function_name_ptr, &gdscript->_global_names_ptr[utility_name_idx]);

				Gp call_error_ptr = get_call_error_ptr(context);

				asmjit::InvokeNode *utility_invoke;
				cc.invoke(&utility_invoke, &Variant::call_utility_function, asmjit::FuncSignature::build<void, StringName &, Variant *, const Variant **, int, Callable::CallError &>());
				utility_invoke->setArg(0, function_name_ptr);
				utility_invoke->setArg(1, dst_ptr);
				utility_invoke->setArg(2, args_array);
				utility_invoke->setArg(3, argc);
				utility_invoke->setArg(4, call_error_ptr);

				print_line("    Return:");
				print_address_info(gdscript, dst_addr);

				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_CALL_UTILITY_VALIDATED: {
				int instr_arg_count = gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				int dst_addr = gdscript->_code_ptr[ip];
				int argc = gdscript->_code_ptr[ip + 1];
				int utility_idx = gdscript->_code_ptr[ip + 2];
				print_line(ip - instr_arg_count - 1, "CALL_UTILITY_VALIDATED: utility_index=", utility_idx, ", argc=", argc, ", instr_arg_count=", instr_arg_count);

				Variant::ValidatedUtilityFunction utility_func = gdscript->_utilities_ptr[utility_idx];

				Gp args_array = prepare_args_array(context, argc, ip - argc);

				Gp dst_ptr = get_variant_ptr(context, dst_addr);

				asmjit::InvokeNode *utility_invoke;
				cc.invoke(&utility_invoke, utility_func, asmjit::FuncSignature::build<void, Variant *, const Variant **, int>());
				utility_invoke->setArg(0, dst_ptr);
				utility_invoke->setArg(1, args_array);
				utility_invoke->setArg(2, argc);

				print_line("    Return:");
				print_address_info(gdscript, dst_addr);

				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_CALL_GDSCRIPT_UTILITY: {
				int instr_var_args = gdscript->_code_ptr[++ip];
				ip += instr_var_args;
				int dst_addr = gdscript->_code_ptr[ip];
				int argc = gdscript->_code_ptr[ip + 1];
				int utility_idx = gdscript->_code_ptr[ip + 2];

				GDScriptUtilityFunctions::FunctionPtr utility_func = gdscript->_gds_utilities_ptr[utility_idx];
				print_line(ip - instr_var_args - 1, "CALL_GDSCRIPT_UTILITY: utility_index=", utility_idx, ", argc=", argc);

				Gp args_array = prepare_args_array(context, argc, ip - argc);
				Gp dst_ptr = get_variant_ptr(context, dst_addr);
				Gp call_error_ptr = get_call_error_ptr(context);

				asmjit::InvokeNode *utility_invoke;
				context.cc->invoke(&utility_invoke, utility_func,
						asmjit::FuncSignature::build<void, Variant *, const Variant **, int, Callable::CallError &>());
				utility_invoke->setArg(0, dst_ptr);
				utility_invoke->setArg(1, args_array);
				utility_invoke->setArg(2, argc);
				utility_invoke->setArg(3, call_error_ptr);

				print_line("    Return:");
				print_address_info(gdscript, dst_addr);

				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_CALL_BUILTIN_TYPE_VALIDATED: {
				int instr_arg_count = gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				int base_addr = gdscript->_code_ptr[ip - 1];
				int dst_addr = gdscript->_code_ptr[ip];
				int argc = gdscript->_code_ptr[ip + 1];
				int method_idx = gdscript->_code_ptr[ip + 2];

				Variant::ValidatedBuiltInMethod method_func = gdscript->_builtin_methods_ptr[method_idx];
				print_line(ip, "CALL_BUILTIN_TYPE_VALIDATED: method_idx=", method_idx, " arg_count=", argc);

				Gp base_ptr = get_variant_ptr(context, base_addr);
				Gp dst_ptr = get_variant_ptr(context, dst_addr);
				Gp args_array = prepare_args_array(context, argc, ip - instr_arg_count + 1);

				asmjit::InvokeNode *call_invoke;
				cc.invoke(&call_invoke, method_func,
						asmjit::FuncSignature::build<void, Variant *, const Variant **, int, Variant *>());
				call_invoke->setArg(0, base_ptr);
				call_invoke->setArg(1, args_array);
				call_invoke->setArg(2, argc);
				call_invoke->setArg(3, dst_ptr);

				print_line("    Result:");
				print_address_info(gdscript, dst_addr);

				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_CALL_METHOD_BIND:
			case GDScriptFunction::OPCODE_CALL_METHOD_BIND_RET: {
				int instr_arg_count = gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				int base_addr = gdscript->_code_ptr[ip - 1];
				int dst_addr = gdscript->_code_ptr[ip];
				int argc = gdscript->_code_ptr[ip + 1];
				int method_idx = gdscript->_code_ptr[ip + 2];

				MethodBind *method = gdscript->_methods_ptr[method_idx];
				print_line(ip - instr_arg_count - 1, "OPCODE_CALL_METHOD_BIND: ", method->get_name(), ", argc=", argc);

				Gp base_ptr = get_variant_ptr(context, base_addr);
				Gp dst_ptr = get_variant_ptr(context, dst_addr);
				Gp call_error_ptr = get_call_error_ptr(context);

				Gp base_obj = context.cc->newIntPtr("base_obj");
				context.cc->mov(base_obj, Arch::ptr(base_ptr, offsetof(Variant, _data) + offsetof(Variant::ObjData, obj)));

				Gp args_array = prepare_args_array(context, argc, ip - instr_arg_count + 1);
				asmjit::InvokeNode *call_invoke;

				if (opcode == GDScriptFunction::OPCODE_CALL_METHOD_BIND) {
					context.cc->invoke(&call_invoke,
							static_cast<void (*)(MethodBind *, Object *, const Variant **, int, Callable::CallError &)>(
									[](MethodBind *method_p, Object *obj, const Variant **args, int argcount, Callable::CallError &err) -> void {
										method_p->call(obj, args, argcount, err);
									}),
							asmjit::FuncSignature::build<void, MethodBind *, Object *, const Variant **, int, Callable::CallError &>());
				} else {
					context.cc->invoke(&call_invoke,
							static_cast<void (*)(MethodBind *, Object *, const Variant **, int, Callable::CallError &, Variant *dst)>(
									[](MethodBind *method_p, Object *obj, const Variant **args, int argcount, Callable::CallError &err, Variant *dst) -> void {
										Variant temp_ret = method_p->call(obj, args, argcount, err);
										*dst = temp_ret;
									}),
							asmjit::FuncSignature::build<void, MethodBind *, Object *, const Variant **, int, Callable::CallError &, Variant *>());

					call_invoke->setArg(5, dst_ptr);
				}

				call_invoke->setArg(0, method);
				call_invoke->setArg(1, base_obj);
				call_invoke->setArg(2, args_array);
				call_invoke->setArg(3, argc);
				call_invoke->setArg(4, call_error_ptr);

				print_line("    Base address:");
				print_address_info(gdscript, base_addr);
				print_line("    Result:");
				print_address_info(gdscript, dst_addr);

				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN:
			case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_NO_RETURN: {
				int instr_arg_count = gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				int base_addr = gdscript->_code_ptr[ip - 1]; // -argc?
				int dst_addr = gdscript->_code_ptr[ip];
				int argc = gdscript->_code_ptr[ip + 1];
				int method_idx = gdscript->_code_ptr[ip + 2];

				MethodBind *method = gdscript->_methods_ptr[method_idx];

				Gp base_ptr = get_variant_ptr(context, base_addr);
				Gp dst_ptr = get_variant_ptr(context, dst_addr);

				Gp base_obj = context.cc->newIntPtr("base_obj");
				context.cc->mov(base_obj, Arch::ptr(base_ptr, offsetof(Variant, _data) + offsetof(Variant::ObjData, obj)));

				Gp args_array = prepare_args_array(context, argc, ip - instr_arg_count + 1);

				asmjit::InvokeNode *method_invoke;
				if (opcode == GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN) {
					context.cc->invoke(&method_invoke,
							static_cast<void (*)(MethodBind *, Object *, const Variant **, Variant *)>(
									[](MethodBind *method_p, Object *obj, const Variant **args, Variant *ret) {
										method_p->validated_call(obj, args, ret);
									}),
							asmjit::FuncSignature::build<void, MethodBind *, Object *, const Variant **, Variant *>());
				} else {
					context.cc->invoke(&method_invoke,
							static_cast<void (*)(MethodBind *, Object *, const Variant **, Variant *)>(
									[](MethodBind *method_p, Object *obj, const Variant **args, Variant *ret) {
										VariantInternal::initialize(ret, Variant::NIL);
										method_p->validated_call(obj, args, nullptr);
									}),
							asmjit::FuncSignature::build<void, MethodBind *, Object *, const Variant **, Variant *>());
				}
				method_invoke->setArg(0, method);
				method_invoke->setArg(1, base_obj);
				method_invoke->setArg(2, args_array);
				method_invoke->setArg(3, dst_ptr);

				print_line("    Base address:");
				print_address_info(gdscript, base_addr);
				print_line("    Result:");
				print_address_info(gdscript, dst_addr);

				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_JUMP: {
				int target = gdscript->_code_ptr[ip + 1];

				cc.jmp(analysis.jump_labels[target]);

				print_line(ip, "JUMP to: ", gdscript->_code_ptr[ip + 1]);
				incr += 2;
			} break;

			case GDScriptFunction::OPCODE_JUMP_IF:
			case GDScriptFunction::OPCODE_JUMP_IF_NOT:
			case GDScriptFunction::OPCODE_JUMP_IF_SHARED: {
				int condition_addr = gdscript->_code_ptr[ip + 1];
				int target = gdscript->_code_ptr[ip + 2];

				Gp condition_ptr = get_variant_ptr(context, condition_addr);
				Gp boolean_result = cc.newInt8("boolean_result");

				asmjit::InvokeNode *booleanize_invoke;
				if (opcode == GDScriptFunction::OPCODE_JUMP_IF_SHARED) {
					cc.invoke(&booleanize_invoke,
							static_cast<bool (*)(const Variant *)>([](const Variant *v) -> bool {
								return v->is_shared();
							}),
							asmjit::FuncSignature::build<bool, const Variant *>());
				} else {
					cc.invoke(&booleanize_invoke,
							static_cast<bool (*)(const Variant *)>([](const Variant *v) -> bool {
								return v->booleanize();
							}),
							asmjit::FuncSignature::build<bool, const Variant *>());
				}
				booleanize_invoke->setArg(0, condition_ptr);
				booleanize_invoke->setRet(0, boolean_result);

				cc.test(boolean_result, boolean_result);
				if (opcode == GDScriptFunction::OPCODE_JUMP_IF_NOT) {
					cc.jz(analysis.jump_labels[target]);
				} else {
					cc.jnz(analysis.jump_labels[target]);
				}

				print_line(ip, "JUMP to: ", target);
				print_line("    Condition:");
				print_address_info(gdscript, condition_addr);

				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_RETURN: {
				int dst_addr = gdscript->_code_ptr[ip + 1];

				Gp src_ptr = get_variant_ptr(context, dst_addr);
				Gp dst_ptr = cc.newIntPtr("dst_addr");

				cc.mov(dst_ptr, result_ptr);

				copy_variant(context, dst_ptr, src_ptr);
				cc.ret();

				print_line(ip, "RETURN");
				print_line("    Return value:");
				print_address_info(gdscript, dst_addr);
				incr = 2;
			} break;

			case GDScriptFunction::OPCODE_RETURN_TYPED_BUILTIN: {
				int dst_addr = gdscript->_code_ptr[ip + 1];

				Gp src_ptr = get_variant_ptr(context, dst_addr);
				Gp dst_ptr = cc.newIntPtr("dst_addr");

				cc.mov(dst_ptr, result_ptr);

				cast_and_store(context, src_ptr, dst_ptr, gdscript->return_type.builtin_type, dst_addr);
				cc.ret();

				print_line(ip, "RETURN BUILTIN: ", Variant::get_type_name(gdscript->return_type.builtin_type));
				print_line("    Return value:");
				print_address_info(gdscript, dst_addr);
				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_ITERATE_BEGIN_INT: {
				int counter_addr = gdscript->_code_ptr[ip + 1];
				int container_addr = gdscript->_code_ptr[ip + 2];
				int iterator_addr = gdscript->_code_ptr[ip + 3];
				int jump_target = gdscript->_code_ptr[ip + 4];

				print_line(ip, "ITERATE_BEGIN_INT, jump to: ", jump_target);
				print_line("    Counter:");
				print_address_info(gdscript, counter_addr);
				print_line("    Container:");
				print_address_info(gdscript, container_addr);
				print_line("    Iterator:");
				print_address_info(gdscript, iterator_addr);

				Gp container_ptr = get_variant_ptr(context, container_addr);
				Gp counter_ptr = get_variant_ptr(context, counter_addr);

				Gp size = cc.newInt64("size");
				cc.mov(size, asmjit::x86::qword_ptr(container_ptr, OFFSET_INT));

				cc.mov(asmjit::x86::dword_ptr(counter_ptr, 0), (int)Variant::INT);
				cc.mov(asmjit::x86::qword_ptr(counter_ptr, OFFSET_INT), 0);

				cc.cmp(size, 0);
				cc.jle(analysis.jump_labels[jump_target]);

				Gp iterator_ptr = get_variant_ptr(context, iterator_addr);
				cc.mov(asmjit::x86::dword_ptr(iterator_ptr, 0), (int)Variant::INT);
				cc.mov(asmjit::x86::qword_ptr(iterator_ptr, OFFSET_INT), 0);

				incr = 5;
			} break;

			case GDScriptFunction::OPCODE_ITERATE_BEGIN_ARRAY: {
				int counter_addr = gdscript->_code_ptr[ip + 1];
				int container_addr = gdscript->_code_ptr[ip + 2];
				int iterator_addr = gdscript->_code_ptr[ip + 3];
				int jump_target = gdscript->_code_ptr[ip + 4];

				print_line(ip, "ITERATE_BEGIN_ARRAY, jump to: ", jump_target);
				print_line("    Counter:");
				print_address_info(gdscript, counter_addr);
				print_line("    Container:");
				print_address_info(gdscript, container_addr);
				print_line("    Iterator:");
				print_address_info(gdscript, iterator_addr);

				Gp container_ptr = get_variant_ptr(context, container_addr);
				Gp counter_ptr = get_variant_ptr(context, counter_addr);
				Gp iterator_ptr = get_variant_ptr(context, iterator_addr);

				cc.mov(asmjit::x86::dword_ptr(counter_ptr, 0), (int)Variant::INT);
				cc.mov(asmjit::x86::qword_ptr(counter_ptr, OFFSET_INT), 0);

				Gp array_ptr = cc.newIntPtr("array_ptr");
				asmjit::InvokeNode *get_array_invoke;
				cc.invoke(&get_array_invoke,
						static_cast<const Array *(*)(const Variant *)>([](const Variant *v) -> const Array * {
							return VariantInternal::get_array(v);
						}),
						asmjit::FuncSignature::build<const Array *, const Variant *>());
				get_array_invoke->setArg(0, container_ptr);
				get_array_invoke->setRet(0, array_ptr);

				Gp array_size = cc.newInt32("array_size");
				asmjit::InvokeNode *size_invoke;
				cc.invoke(&size_invoke,
						static_cast<int (*)(const Array *)>([](const Array *arr) -> int {
							return arr->size();
						}),
						asmjit::FuncSignature::build<int, const Array *>());
				size_invoke->setArg(0, array_ptr);
				size_invoke->setRet(0, array_size);

				cc.test(array_size, array_size);
				cc.jz(analysis.jump_labels[jump_target]);

				asmjit::InvokeNode *get_first_invoke;
				cc.invoke(&get_first_invoke,
						static_cast<void (*)(const Array *, Variant *)>([](const Array *arr, Variant *dst) -> void {
							*dst = arr->operator[](0);
						}),
						asmjit::FuncSignature::build<void, const Array *, Variant *>());
				get_first_invoke->setArg(0, array_ptr);
				get_first_invoke->setArg(1, iterator_ptr);

				incr = 5;
			} break;

			case GDScriptFunction::OPCODE_ITERATE_BEGIN_RANGE: {
				int counter_addr = gdscript->_code_ptr[ip + 1];
				int from_addr = gdscript->_code_ptr[ip + 2];
				int to_addr = gdscript->_code_ptr[ip + 3];
				int step_addr = gdscript->_code_ptr[ip + 4];
				int iterator_addr = gdscript->_code_ptr[ip + 5];
				int jump_target = gdscript->_code_ptr[ip + 6];

				print_line(ip, "ITERATE_BEGIN_RANGE, jump to: ", jump_target);
				print_line("    Counter:");
				print_address_info(gdscript, counter_addr);
				print_line("    From:");
				print_address_info(gdscript, from_addr);
				print_line("    To:");
				print_address_info(gdscript, to_addr);
				print_line("    Step:");
				print_address_info(gdscript, step_addr);
				print_line("    Iterator:");
				print_address_info(gdscript, iterator_addr);

				Gp from = extract_int_from_variant(context, from_addr);
				Mem to = get_variant_mem(context, to_addr, OFFSET_INT);
				Mem step = get_variant_mem(context, step_addr, OFFSET_INT);

				cc.mov(get_variant_type_mem(context, counter_addr), (int)Variant::INT);
				cc.mov(get_variant_mem(context, counter_addr, OFFSET_INT), from);

				Gp condition = cc.newInt64("condition");
				cc.mov(condition, to);
				cc.sub(condition, from);
				cc.imul(condition, step);

				cc.cmp(condition, 0);
				cc.jle(analysis.jump_labels[jump_target]);
				
				cc.mov(get_variant_type_mem(context, iterator_addr), (int)Variant::INT);
				cc.mov(get_variant_mem(context, iterator_addr, OFFSET_INT), from);

				incr = 7;
			} break;

			case GDScriptFunction::OPCODE_ITERATE_INT: {
				int counter_addr = gdscript->_code_ptr[ip + 1];
				int container_addr = gdscript->_code_ptr[ip + 2];
				int iterator_addr = gdscript->_code_ptr[ip + 3];
				int jump_target = gdscript->_code_ptr[ip + 4];

				print_line(ip, "ITERATE_INT, jump to: ", jump_target);
				print_line("    Counter:");
				print_address_info(gdscript, counter_addr);
				print_line("    Container:");
				print_address_info(gdscript, container_addr);
				print_line("    Iterator:");
				print_address_info(gdscript, iterator_addr);

				Gp size = extract_int_from_variant(context, container_addr);
				Gp count = cc.newInt64("count");

				context.cc->mov(count, get_variant_mem(context, counter_addr, OFFSET_INT));
				context.cc->add(count, 1);
				context.cc->mov(get_variant_mem(context, counter_addr, OFFSET_INT), count);

				cc.cmp(count, size);
				cc.jae(analysis.jump_labels[jump_target]);
				cc.mov(get_variant_mem(context, iterator_addr, OFFSET_INT), count);

				incr = 5;
			} break;

			case GDScriptFunction::OPCODE_ITERATE_ARRAY: {
				int counter_addr = gdscript->_code_ptr[ip + 1];
				int container_addr = gdscript->_code_ptr[ip + 2];
				int iterator_addr = gdscript->_code_ptr[ip + 3];
				int jump_target = gdscript->_code_ptr[ip + 4];

				Gp container_ptr = get_variant_ptr(context, container_addr);
				Gp counter_ptr = get_variant_ptr(context, counter_addr);

				Gp array_ptr = cc.newIntPtr("array_ptr");
				asmjit::InvokeNode *get_array_invoke;
				cc.invoke(&get_array_invoke,
						static_cast<const Array *(*)(const Variant *)>([](const Variant *v) -> const Array * {
							return VariantInternal::get_array(v);
						}),
						asmjit::FuncSignature::build<const Array *, const Variant *>());
				get_array_invoke->setArg(0, container_ptr);
				get_array_invoke->setRet(0, array_ptr);

				Gp idx = cc.newInt64("index");
				context.cc->mov(idx, asmjit::x86::qword_ptr(counter_ptr, OFFSET_INT));
				context.cc->add(idx, 1);
				context.cc->mov(asmjit::x86::qword_ptr(counter_ptr, OFFSET_INT), idx);

				Gp array_size = cc.newInt32("array_size");
				asmjit::InvokeNode *size_invoke;
				cc.invoke(&size_invoke,
						static_cast<int (*)(const Array *)>([](const Array *arr) -> int {
							return arr->size();
						}),
						asmjit::FuncSignature::build<int, const Array *>());
				size_invoke->setArg(0, array_ptr);
				size_invoke->setRet(0, array_size);

				cc.cmp(idx.r32(), array_size);
				cc.jae(analysis.jump_labels[jump_target]);

				Gp iterator_ptr = get_variant_ptr(context, iterator_addr);
				asmjit::InvokeNode *get_invoke;
				cc.invoke(&get_invoke,
						static_cast<void (*)(const Array *, int, Variant *)>([](const Array *arr, int index, Variant *dst) -> void {
							*dst = arr->operator[](index);
						}),
						asmjit::FuncSignature::build<void, const Array *, int, Variant *>());
				get_invoke->setArg(0, array_ptr);
				get_invoke->setArg(1, idx);
				get_invoke->setArg(2, iterator_ptr);

				incr = 5;
			} break;

			case GDScriptFunction::OPCODE_ITERATE_RANGE: {
				int counter_addr = gdscript->_code_ptr[ip + 1];
				int to_addr = gdscript->_code_ptr[ip + 2];
				int step_addr = gdscript->_code_ptr[ip + 3];
				int iterator_addr = gdscript->_code_ptr[ip + 4];
				int jump_target = gdscript->_code_ptr[ip + 5];

				print_line(ip, "ITERATE_RANGE, jump to: ", jump_target);
				print_line("    Counter:");
				print_address_info(gdscript, counter_addr);
				print_line("    To:");
				print_address_info(gdscript, to_addr);
				print_line("    Step:");
				print_address_info(gdscript, step_addr);
				print_line("    Iterator:");
				print_address_info(gdscript, iterator_addr);

				Mem counter_ptr = get_variant_mem(context, counter_addr, OFFSET_INT);
				Mem to = get_variant_mem(context, to_addr, OFFSET_INT);
				Gp step = extract_int_from_variant(context, step_addr);

				Gp count = cc.newInt64("count");
				context.cc->mov(count, counter_ptr);
				context.cc->add(count, step);
				context.cc->mov(counter_ptr, count);

				Gp condition = cc.newInt64("condition");
				cc.mov(condition, count);
				cc.sub(condition, to);
				cc.imul(condition, step);

				cc.cmp(condition, 0);
				cc.jge(analysis.jump_labels[jump_target]);
				cc.mov(get_variant_mem(context, iterator_addr, OFFSET_INT), count);

				incr = 6;
			} break;

#define JIT_OPCODE_TYPE_ADJUST(m_v_type, m_c_type)                  \
	case GDScriptFunction::OPCODE_TYPE_ADJUST_##m_v_type: {         \
		int dst_addr = gdscript->_code_ptr[ip + 1];                 \
                                                                    \
		print_line(ip, "TYPE_ADJUST_" #m_v_type);                   \
                                                                    \
		Gp dst_ptr = get_variant_ptr(context, dst_addr);            \
                                                                    \
		asmjit::InvokeNode *adjust_invoke;                          \
		cc.invoke(&adjust_invoke,                                   \
				static_cast<void (*)(Variant *)>([](Variant *arg) { \
					VariantTypeAdjust<m_c_type>::adjust(arg);       \
				}),                                                 \
				asmjit::FuncSignature::build<void, Variant *>());   \
		adjust_invoke->setArg(0, dst_ptr);                          \
                                                                    \
		print_line("    Destination:");                             \
		print_address_info(gdscript, dst_addr);                     \
                                                                    \
		incr = 2;                                                   \
	} break;

				JIT_OPCODE_TYPE_ADJUST(BOOL, bool)
				JIT_OPCODE_TYPE_ADJUST(INT, int64_t)
				JIT_OPCODE_TYPE_ADJUST(FLOAT, double)
				JIT_OPCODE_TYPE_ADJUST(STRING, String)
				JIT_OPCODE_TYPE_ADJUST(VECTOR2, Vector2)
				JIT_OPCODE_TYPE_ADJUST(VECTOR2I, Vector2i)
				JIT_OPCODE_TYPE_ADJUST(RECT2, Rect2)
				JIT_OPCODE_TYPE_ADJUST(RECT2I, Rect2i)
				JIT_OPCODE_TYPE_ADJUST(VECTOR3, Vector3)
				JIT_OPCODE_TYPE_ADJUST(VECTOR3I, Vector3i)
				JIT_OPCODE_TYPE_ADJUST(TRANSFORM2D, Transform2D)
				JIT_OPCODE_TYPE_ADJUST(VECTOR4, Vector4)
				JIT_OPCODE_TYPE_ADJUST(VECTOR4I, Vector4i)
				JIT_OPCODE_TYPE_ADJUST(PLANE, Plane)
				JIT_OPCODE_TYPE_ADJUST(QUATERNION, Quaternion)
				JIT_OPCODE_TYPE_ADJUST(AABB, AABB)
				JIT_OPCODE_TYPE_ADJUST(BASIS, Basis)
				JIT_OPCODE_TYPE_ADJUST(TRANSFORM3D, Transform3D)
				JIT_OPCODE_TYPE_ADJUST(PROJECTION, Projection)
				JIT_OPCODE_TYPE_ADJUST(COLOR, Color)
				JIT_OPCODE_TYPE_ADJUST(STRING_NAME, StringName)
				JIT_OPCODE_TYPE_ADJUST(NODE_PATH, NodePath)
				JIT_OPCODE_TYPE_ADJUST(RID, RID)
				JIT_OPCODE_TYPE_ADJUST(OBJECT, Object *)
				JIT_OPCODE_TYPE_ADJUST(CALLABLE, Callable)
				JIT_OPCODE_TYPE_ADJUST(SIGNAL, Signal)
				JIT_OPCODE_TYPE_ADJUST(DICTIONARY, Dictionary)
				JIT_OPCODE_TYPE_ADJUST(ARRAY, Array)
				JIT_OPCODE_TYPE_ADJUST(PACKED_BYTE_ARRAY, PackedByteArray)
				JIT_OPCODE_TYPE_ADJUST(PACKED_INT32_ARRAY, PackedInt32Array)
				JIT_OPCODE_TYPE_ADJUST(PACKED_INT64_ARRAY, PackedInt64Array)
				JIT_OPCODE_TYPE_ADJUST(PACKED_FLOAT32_ARRAY, PackedFloat32Array)
				JIT_OPCODE_TYPE_ADJUST(PACKED_FLOAT64_ARRAY, PackedFloat64Array)
				JIT_OPCODE_TYPE_ADJUST(PACKED_STRING_ARRAY, PackedStringArray)
				JIT_OPCODE_TYPE_ADJUST(PACKED_VECTOR2_ARRAY, PackedVector2Array)
				JIT_OPCODE_TYPE_ADJUST(PACKED_VECTOR3_ARRAY, PackedVector3Array)
				JIT_OPCODE_TYPE_ADJUST(PACKED_COLOR_ARRAY, PackedColorArray)
				JIT_OPCODE_TYPE_ADJUST(PACKED_VECTOR4_ARRAY, PackedVector4Array)

			case GDScriptFunction::OPCODE_LINE: {
				print_line(ip, "LINE: ", gdscript->_code_ptr[ip + 1]);
				incr += 2;
			} break;

			case GDScriptFunction::OPCODE_END: {
				print_line(ip, "END");
				incr += 1;
			} break;

			default: {
				print_line(ip, "Unknown opcode: ", opcode);
				incr += 1;
			} break;
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

	auto end = OS::get_singleton()->get_ticks_usec() - start;
	print_line("Compile time: ", end);
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

Gp JitCompiler::get_variant_ptr(JitContext &context, int address) {
	int type, index;
	decode_address(address, type, index);

	Gp variant_ptr = context.cc->newIntPtr();

	if (type == GDScriptFunction::ADDR_TYPE_CONSTANT) {
		context.cc->lea(variant_ptr, Arch::ptr(context.constants_ptr, index * sizeof(Variant)));
	} else if (type == GDScriptFunction::ADDR_TYPE_STACK) {
		context.cc->lea(variant_ptr, Arch::ptr(context.stack_ptr, index * sizeof(Variant)));
	} else if (type == GDScriptFunction::ADDR_TYPE_MEMBER) {
		context.cc->lea(variant_ptr, Arch::ptr(context.members_ptr, index * sizeof(Variant)));
	}

	return variant_ptr;
}

Mem JitCompiler::get_variant_mem(const JitContext &ctx, int address, int offset_field) {
	int type, index;
	decode_address(address, type, index);
	int disp = index * sizeof(Variant) + offset_field;
	if (type == GDScriptFunction::ADDR_TYPE_CONSTANT) {
		return mem_qword_ptr(ctx.constants_ptr, disp);
	} else if (type == GDScriptFunction::ADDR_TYPE_STACK) {
		return mem_qword_ptr(ctx.stack_ptr, disp);
	} else {
		return mem_qword_ptr(ctx.members_ptr, disp);
	}
}

Mem JitCompiler::get_variant_type_mem(const JitContext &ctx, int address) {
	int type, index;
	decode_address(address, type, index);
	int disp = index * sizeof(Variant);

	if (type == GDScriptFunction::ADDR_TYPE_CONSTANT) {
		return asmjit::x86::dword_ptr(ctx.constants_ptr, disp);
	} else if (type == GDScriptFunction::ADDR_TYPE_STACK) {
		return asmjit::x86::dword_ptr(ctx.stack_ptr, disp);
	} else {
		return asmjit::x86::dword_ptr(ctx.members_ptr, disp);
	}
}

Mem JitCompiler::get_int_mem_ptr(JitContext &ctx, int address) {
	int type, index;
	decode_address(address, type, index);
	int disp = index * sizeof(Variant) + OFFSET_INT;
	if (type == GDScriptFunction::ADDR_TYPE_CONSTANT) {
		return Arch::ptr(ctx.constants_ptr, disp);
	} else if (type == GDScriptFunction::ADDR_TYPE_STACK) {
		return Arch::ptr(ctx.stack_ptr, disp);
	} else {
		return Arch::ptr(ctx.members_ptr, disp);
	}
}

void JitCompiler::handle_int_operation(const OpInfo operation, JitContext &ctx, int left_addr, int right_addr, int result_addr) {
	Gp left = extract_int_from_variant(ctx, left_addr);
	Mem right = get_int_mem_ptr(ctx, right_addr);
	Mem result_ptr = get_variant_mem(ctx, result_addr, OFFSET_INT);

	switch (operation.op) {
		case Variant::OP_ADD:
			ctx.cc->add(left, right);
			break;
		case Variant::OP_SUBTRACT:
			ctx.cc->sub(left, right);
			break;
		case Variant::OP_MULTIPLY:
			ctx.cc->imul(left, right);
			break;
		case Variant::OP_EQUAL:
			gen_compare_int(ctx, left, right, result_addr, CondCode::kEqual);
			break;
		case Variant::OP_NOT_EQUAL:
			gen_compare_int(ctx, left, right, result_addr, CondCode::kNotEqual);
			break;
		case Variant::OP_LESS:
			gen_compare_int(ctx, left, right, result_addr, CondCode::kL);
			break;
		case Variant::OP_LESS_EQUAL:
			gen_compare_int(ctx, left, right, result_addr, CondCode::kLE);
			break;
		case Variant::OP_GREATER:
			gen_compare_int(ctx, left, right, result_addr, CondCode::kG);
			break;
		case Variant::OP_GREATER_EQUAL:
			gen_compare_int(ctx, left, right, result_addr, CondCode::kGE);
			break;
		default: {
			print_error("Unsupported operation for int operation: " + String::num(operation.op));
			return;
		}
	}

	ctx.cc->mov(result_ptr, left);
}

void JitCompiler::gen_compare_int(JitContext &ctx, Gp &lhs, Mem &rhs, int result_addr, CondCode cc) {
	ctx.cc->cmp(lhs, rhs);
	ctx.cc->set(cc, lhs.r8());
	ctx.cc->movzx(lhs, lhs.r8());
	ctx.cc->mov(get_variant_type_mem(ctx, result_addr), (int)Variant::BOOL);
}

void JitCompiler::gen_compare_float(JitContext &ctx, Vec &lhs, Vec &rhs, int result_addr, CondCode cc) {
	ctx.cc->comisd(lhs, rhs);
	ctx.cc->set(cc, get_variant_mem(ctx, result_addr, OFFSET_INT));
	ctx.cc->mov(get_variant_type_mem(ctx, result_addr), (int)Variant::BOOL);
}

void JitCompiler::handle_float_operation(const OpInfo operation, JitContext &ctx, int left_addr, int right_addr, int result_addr) {
	Vec left_val = ctx.cc->newXmmSd();
	Vec right_val = ctx.cc->newXmmSd();

	if (operation.left_type == Variant::INT && operation.right_type == Variant::FLOAT) {
		ctx.cc->cvtsi2sd(left_val, get_variant_mem(ctx, left_addr, OFFSET_INT));
		extract_float_from_variant(ctx, right_val, right_addr);
	} else if (operation.left_type == Variant::FLOAT && operation.right_type == Variant::INT) {
		extract_float_from_variant(ctx, left_val, left_addr);
		ctx.cc->cvtsi2sd(right_val, get_variant_mem(ctx, right_addr, OFFSET_INT));
	} else {
		extract_float_from_variant(ctx, left_val, left_addr);
		extract_float_from_variant(ctx, right_val, right_addr);
	}

	switch (operation.op) {
		case Variant::OP_ADD: {
			ctx.cc->addsd(left_val, right_val);
			store_float_to_variant(ctx, left_val, result_addr);
		} break;
		case Variant::OP_SUBTRACT: {
			ctx.cc->subsd(left_val, right_val);
			store_float_to_variant(ctx, left_val, result_addr);
		} break;
		case Variant::OP_MULTIPLY: {
			ctx.cc->mulsd(left_val, right_val);
			store_float_to_variant(ctx, left_val, result_addr);
		} break;
		case Variant::OP_DIVIDE: {
			ctx.cc->divsd(left_val, right_val);
			store_float_to_variant(ctx, left_val, result_addr);
		} break;
		case Variant::OP_EQUAL:
			gen_compare_float(ctx, left_val, right_val, result_addr, CondCode::kE);
			break;
		case Variant::OP_NOT_EQUAL:
			gen_compare_float(ctx, left_val, right_val, result_addr, CondCode::kNE);
			break;
		case Variant::OP_LESS:
			gen_compare_float(ctx, left_val, right_val, result_addr, CondCode::kB);
			break;
		case Variant::OP_LESS_EQUAL:
			gen_compare_float(ctx, left_val, right_val, result_addr, CondCode::kBE);
			break;
		case Variant::OP_GREATER:
			gen_compare_float(ctx, left_val, right_val, result_addr, CondCode::kA);
			break;
		case Variant::OP_GREATER_EQUAL:
			gen_compare_float(ctx, left_val, right_val, result_addr, CondCode::kAE);
			break;
		default: {
			print_error("Unsupported operation for float operation: " + String::num(operation.op));
			return;
		}
	}
}

//todo
void JitCompiler::handle_vector2_operation(const OpInfo operation, JitContext &context, int left_addr, int right_addr, int result_addr) {
	Vec left_x = context.cc->newXmmSs("left_x");
	Vec left_y = context.cc->newXmmSs("left_y");
	Vec right_x = context.cc->newXmmSs("right_x");
	Vec right_y = context.cc->newXmmSs("right_y");

	Gp left_ptr = get_variant_ptr(context, left_addr);
	Gp right_ptr = get_variant_ptr(context, right_addr);

	if (operation.left_type == Variant::VECTOR2) {
		context.cc->movss(left_x, asmjit::x86::dword_ptr(left_ptr, OFFSET_VECTOR2_X));
		context.cc->movss(left_y, asmjit::x86::dword_ptr(left_ptr, OFFSET_VECTOR2_Y));
	} else if (operation.left_type == Variant::FLOAT) {
		context.cc->movsd(left_x, asmjit::x86::qword_ptr(left_ptr, OFFSET_FLOAT));
		context.cc->cvtsd2ss(left_x, left_x);
		context.cc->movss(left_y, left_x);
	} else if (operation.left_type == Variant::INT) {
		context.cc->cvtsi2ss(left_x, asmjit::x86::qword_ptr(left_ptr, OFFSET_INT));
		context.cc->movss(left_y, left_x);
	}

	if (operation.right_type == Variant::VECTOR2) {
		context.cc->movss(right_x, asmjit::x86::dword_ptr(right_ptr, OFFSET_VECTOR2_X));
		context.cc->movss(right_y, asmjit::x86::dword_ptr(right_ptr, OFFSET_VECTOR2_Y));
	} else if (operation.right_type == Variant::FLOAT) {
		context.cc->movsd(right_x, asmjit::x86::qword_ptr(right_ptr, OFFSET_FLOAT));
		context.cc->cvtsd2ss(right_x, right_x);
		context.cc->movss(right_y, right_x);
	} else if (operation.right_type == Variant::INT) {
		context.cc->cvtsi2ss(right_x, asmjit::x86::qword_ptr(right_ptr, OFFSET_INT));
		context.cc->movss(right_y, right_x);
	}

	switch (operation.op) {
		case Variant::OP_ADD: {
			context.cc->addss(left_x, right_x);
			context.cc->addss(left_y, right_y);
			break;
		}
		case Variant::OP_SUBTRACT: {
			context.cc->subss(left_x, right_x);
			context.cc->subss(left_y, right_y);
			break;
		}
		case Variant::OP_MULTIPLY: {
			context.cc->mulss(left_x, right_x);
			context.cc->mulss(left_y, right_y);
			break;
		}
		case Variant::OP_DIVIDE: {
			context.cc->divss(left_x, right_x);
			context.cc->divss(left_y, right_y);
			break;
		}
		default: {
			print_line("Unsupported Vector2 operation: ", operation.op);
			return;
		}
	}

	store_vector2_to_variant(context, left_x, left_y, result_addr);
}

void JitCompiler::release_function(void *func_ptr) {
	if (!func_ptr) {
		return;
	}

	runtime.release(func_ptr);
}

OpInfo JitCompiler::get_operator_info(intptr_t op_func) {
	if (op_map.has(op_func)) {
		return op_map[op_func];
	}

	return OpInfo{ Variant::OP_MAX, Variant::VARIANT_MAX, Variant::VARIANT_MAX };
}

FunctionAnalysis JitCompiler::analyze_function(JitContext &context) {
	FunctionAnalysis analysis;

	print_line("\n=== Analyzing Jump Targets ===");

	int ip = 0;
	while (ip < context.gdscript->code.size()) {
		int incr = 0;
		GDScriptFunction::Opcode opcode = GDScriptFunction::Opcode(context.gdscript->_code_ptr[ip]);

		switch (opcode) {
			case GDScriptFunction::OPCODE_OPERATOR: {
				constexpr int _pointer_size = sizeof(Variant::ValidatedOperatorEvaluator) / sizeof(*context.gdscript->_code_ptr);
				incr = 7 + _pointer_size;
				analysis.uses_operator = true;
				analysis.uses_bool = true;
			} break;

			case GDScriptFunction::OPCODE_OPERATOR_VALIDATED: {
				incr = 5;
			} break;

			case GDScriptFunction::OPCODE_SET_KEYED: {
				analysis.uses_bool = true;
				incr = 4;
			} break;

			case GDScriptFunction::OPCODE_SET_INDEXED_VALIDATED: {
				analysis.uses_bool = true;
				incr = 5;
			} break;

			case GDScriptFunction::OPCODE_GET_KEYED: {
				analysis.uses_bool = true;
				incr = 4;
			} break;

			case GDScriptFunction::OPCODE_GET_INDEXED_VALIDATED: {
				analysis.uses_bool = true;
				incr = 5;
			} break;

			case GDScriptFunction::OPCODE_SET_NAMED:
			case GDScriptFunction::OPCODE_GET_NAMED: {
				analysis.uses_bool = true;
				incr = 4;
			} break;
			case GDScriptFunction::OPCODE_GET_NAMED_VALIDATED:
			case GDScriptFunction::OPCODE_SET_NAMED_VALIDATED: {
				incr = 4;
			} break;

			case GDScriptFunction::OPCODE_SET_STATIC_VARIABLE:
			case GDScriptFunction::OPCODE_GET_STATIC_VARIABLE: {
				incr = 4;
			} break;

			case GDScriptFunction::OPCODE_ASSIGN: {
				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_ASSIGN_NULL:
			case GDScriptFunction::OPCODE_ASSIGN_TRUE:
			case GDScriptFunction::OPCODE_ASSIGN_FALSE: {
				incr = 2;
			} break;

			case GDScriptFunction::OPCODE_ASSIGN_TYPED_BUILTIN: {
				analysis.uses_error = true;
				incr = 4;
				break;
			}
			case GDScriptFunction::OPCODE_CAST_TO_SCRIPT: {
				incr = 4;
			} break;
			case GDScriptFunction::OPCODE_CONSTRUCT: {
				int instr_arg_count = context.gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				analysis.uses_error = true;
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_CONSTRUCT_VALIDATED: {
				int instr_arg_count = context.gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_CONSTRUCT_ARRAY: {
				int instr_arg_count = context.gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				incr = 2;
			} break;
			case GDScriptFunction::OPCODE_CONSTRUCT_TYPED_ARRAY: {
				int instr_arg_count = context.gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				incr = 4;
			} break;
			case GDScriptFunction::OPCODE_CALL:
			case GDScriptFunction::OPCODE_CALL_RETURN: {
				int instr_arg_count = context.gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				analysis.uses_error = true;
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_CALL_UTILITY: {
				int instr_arg_count = context.gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				analysis.uses_error = true;
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_CALL_UTILITY_VALIDATED: {
				int instr_arg_count = context.gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_CALL_GDSCRIPT_UTILITY: {
				int instr_arg_count = context.gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				analysis.uses_error = true;
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_CALL_METHOD_BIND:
			case GDScriptFunction::OPCODE_CALL_METHOD_BIND_RET: {
				int instr_arg_count = context.gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				analysis.uses_error = true;
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_CALL_BUILTIN_TYPE_VALIDATED: {
				int instr_arg_count = context.gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN:
			case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_NO_RETURN: {
				int instr_arg_count = context.gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_JUMP: {
				int target = context.gdscript->_code_ptr[ip + 1];
				if (!analysis.jump_labels.has(target)) {
					analysis.jump_labels[target] = context.cc->newLabel();
					print_line("Created label for JUMP target: ", target);
				}
				incr = 2;
			} break;

			case GDScriptFunction::OPCODE_JUMP_IF:
			case GDScriptFunction::OPCODE_JUMP_IF_NOT:
			case GDScriptFunction::OPCODE_JUMP_IF_SHARED: {
				int target = context.gdscript->_code_ptr[ip + 2];
				if (!analysis.jump_labels.has(target)) {
					analysis.jump_labels[target] = context.cc->newLabel();
					print_line("Created label for JUMP target: ", target);
				}
				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_RETURN: {
				incr = 2;
			} break;

			case GDScriptFunction::OPCODE_RETURN_TYPED_BUILTIN: {
				analysis.uses_error = true;
				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_ITERATE_BEGIN_INT:
			case GDScriptFunction::OPCODE_ITERATE_BEGIN_ARRAY:
			case GDScriptFunction::OPCODE_ITERATE_INT:
			case GDScriptFunction::OPCODE_ITERATE_ARRAY: {
				int jump_target = context.gdscript->_code_ptr[ip + 4];
				if (!analysis.jump_labels.has(jump_target)) {
					analysis.jump_labels[jump_target] = context.cc->newLabel();
					print_line("Created label for ITERATE target: ", jump_target);
				}
				incr = 5;
			} break;

			case GDScriptFunction::OPCODE_ITERATE_BEGIN_RANGE: {
				int jump_target = context.gdscript->_code_ptr[ip + 6];
				if (!analysis.jump_labels.has(jump_target)) {
					analysis.jump_labels[jump_target] = context.cc->newLabel();
					print_line("Created label for ITERATE target: ", jump_target);
				}
				incr = 7;
			} break;

			case GDScriptFunction::OPCODE_ITERATE_RANGE: {
				int jump_target = context.gdscript->_code_ptr[ip + 5];
				if (!analysis.jump_labels.has(jump_target)) {
					analysis.jump_labels[jump_target] = context.cc->newLabel();
					print_line("Created label for ITERATE target: ", jump_target);
				}
				incr = 6;
			} break;

			case GDScriptFunction::OPCODE_TYPE_ADJUST_BOOL:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_INT:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_FLOAT:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_STRING:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_VECTOR2:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_VECTOR2I:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_RECT2:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_RECT2I:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_VECTOR3:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_VECTOR3I:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_TRANSFORM2D:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_VECTOR4:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_VECTOR4I:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_PLANE:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_QUATERNION:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_AABB:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_BASIS:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_TRANSFORM3D:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_PROJECTION:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_COLOR:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_STRING_NAME:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_NODE_PATH:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_RID:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_OBJECT:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_CALLABLE:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_SIGNAL:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_DICTIONARY:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_ARRAY:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_BYTE_ARRAY:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_INT32_ARRAY:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_INT64_ARRAY:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_FLOAT32_ARRAY:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_FLOAT64_ARRAY:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_STRING_ARRAY:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_VECTOR2_ARRAY:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_VECTOR3_ARRAY:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_COLOR_ARRAY:
			case GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_VECTOR4_ARRAY: {
				incr = 2;
			} break;

			case GDScriptFunction::OPCODE_LINE: {
				incr = 2;
			} break;

			case GDScriptFunction::OPCODE_END: {
				incr = 1;
			} break;

			default: {
				incr = 1;
			} break;
		}
		ip += incr;
	}

	return analysis;
}

void JitCompiler::copy_variant(JitContext &context, Gp &dst_ptr, Gp &src_ptr) {
	asmjit::InvokeNode *copy_invoke;
	context.cc->invoke(&copy_invoke,
			static_cast<void (*)(Variant *, const Variant *)>([](Variant *dst, const Variant *src) {
				*dst = *src;
			}),
			asmjit::FuncSignature::build<void, Variant *, const Variant *>());
	copy_invoke->setArg(0, dst_ptr);
	copy_invoke->setArg(1, src_ptr);
}

Gp JitCompiler::extract_int_from_variant(JitContext &context, int address) {
	Gp result_reg = context.cc->newInt64("result_int");
	int type, index;
	decode_address(address, type, index);
	int disp = index * sizeof(Variant) + OFFSET_INT;

	if (type == GDScriptFunction::ADDR_TYPE_CONSTANT) {
		context.cc->mov(result_reg, mem_qword_ptr(context.constants_ptr, disp));
	} else if (type == GDScriptFunction::ADDR_TYPE_STACK) {
		context.cc->mov(result_reg, mem_qword_ptr(context.stack_ptr, disp));
	} else if (type == GDScriptFunction::ADDR_TYPE_MEMBER) {
		context.cc->mov(result_reg, mem_qword_ptr(context.members_ptr, disp));
	}

	return result_reg;
}

void JitCompiler::extract_type_from_variant(JitContext &context, Gp &result_reg, int address) {
	Gp variant_ptr = get_variant_ptr(context, address);
	context.cc->mov(result_reg, asmjit::x86::dword_ptr(variant_ptr, 0));
}

void JitCompiler::extract_float_from_variant(JitContext &ctx, Vec &result_reg, int address) {
#if defined(__x86_64) || defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
	Mem variant_ptr = get_variant_mem(ctx, address, OFFSET_FLOAT);
	ctx.cc->movsd(result_reg, variant_ptr);
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(ARM64_ENABLED)
	Gp variant_ptr = get_variant_ptr(context, address);
	ctx.cc->ldr(result_reg.d(), Arch::ptr(variant_ptr, OFFSET_FLOAT));
#endif
}

void JitCompiler::store_float_to_variant(JitContext &ctx, Vec &value, int address) {
	ctx.cc->mov(get_variant_type_mem(ctx, address), (int)Variant::FLOAT);
	ctx.cc->movsd(get_variant_mem(ctx, address, OFFSET_FLOAT), value);
}

void JitCompiler::store_vector2_to_variant(JitContext &context, Vec &x_reg, Vec &y_reg, int address) {
	Gp variant_ptr = get_variant_ptr(context, address);
	context.cc->mov(asmjit::x86::dword_ptr(variant_ptr, 0), (int)Variant::VECTOR2);
	context.cc->movss(asmjit::x86::dword_ptr(variant_ptr, OFFSET_VECTOR2_X), x_reg);
	context.cc->movss(asmjit::x86::dword_ptr(variant_ptr, OFFSET_VECTOR2_Y), y_reg);
}

void JitCompiler::store_int_to_variant(JitContext &context, int value, int address) {
	Gp variant_ptr = get_variant_ptr(context, address);
	context.cc->mov(asmjit::x86::qword_ptr(variant_ptr, OFFSET_INT), value);
}

Gp JitCompiler::get_call_error_ptr(JitContext &context, bool reset) {
	if (reset) {
		context.cc->mov(asmjit::x86::dword_ptr(context.call_error_ptr, 0), (int)Callable::CallError::CALL_OK);
	}
	return context.call_error_ptr;
}

Gp JitCompiler::get_bool_ptr(JitContext &context, bool value) {
	context.cc->mov(asmjit::x86::byte_ptr(context.bool_ptr), (int)value);
	return context.bool_ptr;
}

Gp JitCompiler::prepare_args_array(JitContext &context, int argc, int ip_base) {
	Gp args_array = context.cc->newIntPtr("args_array");

	if (argc > 0) {
		int args_array_size = argc * PTR_SIZE;
		Mem args_stack = context.cc->newStack(args_array_size, 16);
		context.cc->lea(args_array, args_stack);

		for (int i = 0; i < argc; i++) {
			int arg_addr = context.gdscript->_code_ptr[ip_base + i];

			Gp arg_ptr = get_variant_ptr(context, arg_addr);

			context.cc->mov(Arch::ptr(args_array, i * PTR_SIZE), arg_ptr);

			print_line("    Arg[", i, "]");
			print_address_info(context.gdscript, arg_addr);
		}
	} else {
		context.cc->mov(args_array, 0);
	}

	return args_array;
}

void JitCompiler::cast_and_store(JitContext &context, Gp &src_ptr, Gp &dst_ptr, Variant::Type expected_type, int return_addr) {
	if (expected_type == Variant::NIL) {
		copy_variant(context, dst_ptr, src_ptr);
		return;
	}

	Gp current_type = context.cc->newInt32("current_type");
	extract_type_from_variant(context, current_type, return_addr);

	asmjit::Label same_type_label = context.cc->newLabel();
	asmjit::Label end_label = context.cc->newLabel();

	context.cc->cmp(current_type, (int)expected_type);
	context.cc->je(same_type_label);

	{
		Gp args_array = context.cc->newIntPtr("cast_args_array");
		context.cc->lea(args_array, context.cc->newStack(PTR_SIZE, 16));
		context.cc->mov(Arch::ptr(args_array, 0), src_ptr);

		context.cc->mov(asmjit::x86::dword_ptr(dst_ptr, 0), (int)expected_type);

		Gp call_error_ptr = get_call_error_ptr(context);

		asmjit::InvokeNode *construct_invoke;
		context.cc->invoke(&construct_invoke, &Variant::construct,
				asmjit::FuncSignature::build<void, Variant::Type, Variant &, const Variant **, int, Callable::CallError &>());
		construct_invoke->setArg(0, expected_type);
		construct_invoke->setArg(1, dst_ptr);
		construct_invoke->setArg(2, args_array);
		construct_invoke->setArg(3, 1);
		construct_invoke->setArg(4, call_error_ptr);

		context.cc->jmp(end_label);
	}

	context.cc->bind(same_type_label);
	copy_variant(context, dst_ptr, src_ptr);

	context.cc->bind(end_label);
}

void JitCompiler::initialize_context(JitContext &context, const FunctionAnalysis &analysis) {
	if (analysis.uses_error) {
		Mem mem = context.cc->newStack(sizeof(Callable::CallError), 16);
		context.call_error_ptr = context.cc->newIntPtr("call_error_ptr");
		context.cc->lea(context.call_error_ptr, mem);
		context.cc->mov(asmjit::x86::dword_ptr(context.call_error_ptr), 0);
	}

	if (analysis.uses_operator) {
		Mem mem = context.cc->newStack(sizeof(Variant::Operator), 16);
		context.operator_ptr = context.cc->newIntPtr("operator_ptr");
		context.cc->lea(context.operator_ptr, mem);
		context.cc->mov(asmjit::x86::dword_ptr(context.operator_ptr), 0);
	}

	if (analysis.uses_bool) {
		Mem mem = context.cc->newStack(sizeof(bool), 16);
		context.bool_ptr = context.cc->newIntPtr("bool_ptr");
		context.cc->lea(context.bool_ptr, mem);
		context.cc->mov(asmjit::x86::byte_ptr(context.bool_ptr), 0);
	}
}

Mem JitCompiler::mem_qword_ptr(const Gp &base, int disp) {
#if defined(__x86_64) || defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
	return asmjit::x86::qword_ptr(base, disp);
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(ARM64_ENABLED)
	return asmjit::a64::ptr_64(base, disp);
#endif
}

Mem JitCompiler::mem_dword_ptr(const Gp &base, int disp) {
#if defined(__x86_64) || defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
	return asmjit::x86::dword_ptr(base, disp);
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(ARM64_ENABLED)
	return asmjit::a64::ptr_32(base, disp);
#endif
}

Mem JitCompiler::mem_byte_ptr(const Gp &base, int disp) {
#if defined(__x86_64) || defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
	return asmjit::x86::byte_ptr(base, disp);
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(ARM64_ENABLED)
	return asmjit::a64::ptr_8(base, disp);
#endif
}