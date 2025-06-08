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
void set_named(Variant &base, const StringName &name, const Variant *value, bool &valid) {
	base.set_named(name, *value, valid);
}
void get_named(const Variant &base, const StringName &name, Variant *result, bool &valid) {
	*result = base.get_named(name, valid);
}

void initialize_counter_int(Variant *counter) {
	VariantInternal::initialize(counter, Variant::INT);
	*VariantInternal::get_int(counter) = 0;
}

bool is_array_empty(const Variant *container) {
	const Array *array = VariantInternal::get_array(container);
	return array->is_empty();
}

void get_array_first_element(const Variant *container, Variant *iterator) {
	const Array *array = VariantInternal::get_array(container);
	if (!array->is_empty()) {
		*iterator = array->get(0);
	}
}

bool iterate_array_step(Variant *counter, const Variant *container, Variant *iterator) {
	const Array *array = VariantInternal::get_array(container);
	int64_t *idx = VariantInternal::get_int(counter);
	(*idx)++;

	if (*idx >= array->size()) {
		return false;
	} else {
		*iterator = array->get(*idx);
		return true;
	}
}
}

JitCompiler *JitCompiler::singleton = nullptr;
HashMap<Variant::ValidatedOperatorEvaluator, String> JitCompiler::op_map;

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

	op_map[Variant::get_validated_operator_evaluator(Variant::OP_ADD, Variant::FLOAT, Variant::FLOAT)] = "ADD_FLOAT_FLOAT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_SUBTRACT, Variant::FLOAT, Variant::FLOAT)] = "SUBTRACT_FLOAT_FLOAT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_MULTIPLY, Variant::FLOAT, Variant::FLOAT)] = "MULTIPLY_FLOAT_FLOAT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_DIVIDE, Variant::FLOAT, Variant::FLOAT)] = "DIVIDE_FLOAT_FLOAT";

	op_map[Variant::get_validated_operator_evaluator(Variant::OP_EQUAL, Variant::FLOAT, Variant::FLOAT)] = "EQUAL_FLOAT_FLOAT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_NOT_EQUAL, Variant::FLOAT, Variant::FLOAT)] = "NOT_EQUAL_FLOAT_FLOAT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_LESS, Variant::FLOAT, Variant::FLOAT)] = "LESS_FLOAT_FLOAT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_LESS_EQUAL, Variant::FLOAT, Variant::FLOAT)] = "LESS_EQUAL_FLOAT_FLOAT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_GREATER, Variant::FLOAT, Variant::FLOAT)] = "GREATER_FLOAT_FLOAT";
	op_map[Variant::get_validated_operator_evaluator(Variant::OP_GREATER_EQUAL, Variant::FLOAT, Variant::FLOAT)] = "GREATER_EQUAL_FLOAT_FLOAT";

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
	asmjit::x86::Gp stack_ptr = cc.newIntPtr("stack_ptr");

	funcNode->setArg(0, result_ptr);
	funcNode->setArg(1, args_ptr);
	funcNode->setArg(2, members_ptr);
	funcNode->setArg(3, stack_ptr);

	JitContext context;
	context.gdscript = gdscript;
	context.stack_ptr = stack_ptr;
	context.members_ptr = members_ptr;
	context.args_ptr = args_ptr;
	context.cc = &cc;
	context.result_ptr = result_ptr;
	context.shared_call_error_ptr = create_call_error(context);

	HashMap<int, asmjit::Label> jump_labels = analyze_jump_targets(context);

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
			case GDScriptFunction::OPCODE_OPERATOR: { //NOT OPTIMIZED :-(
				constexpr int _pointer_size = sizeof(Variant::ValidatedOperatorEvaluator) / sizeof(*gdscript->_code_ptr);
				int left_addr = gdscript->_code_ptr[ip + 1];
				int right_addr = gdscript->_code_ptr[ip + 2];
				int result_addr = gdscript->_code_ptr[ip + 3];
				Variant::Operator operation = (Variant::Operator)gdscript->_code_ptr[ip + 4];

				asmjit::x86::Gp left_ptr = get_variant_ptr(context, left_addr);
				asmjit::x86::Gp right_ptr = get_variant_ptr(context, right_addr);
				asmjit::x86::Gp dst_ptr = get_variant_ptr(context, result_addr);

				asmjit::x86::Mem valid_mem = cc.newStack(sizeof(bool), 16);
				asmjit::x86::Gp valid_ptr = cc.newIntPtr();
				cc.lea(valid_ptr, valid_mem);
				cc.mov(asmjit::x86::byte_ptr(valid_ptr), 1);

				asmjit::x86::Mem op_mem = cc.newStack(sizeof(Variant::Operator), 16);
				asmjit::x86::Gp op_ptr = cc.newIntPtr();
				cc.lea(op_ptr, op_mem);
				cc.mov(asmjit::x86::dword_ptr(op_ptr), operation);

				asmjit::InvokeNode *evaluate_invoke;
				cc.invoke(&evaluate_invoke, static_cast<void (*)(const Variant::Operator &, const Variant &, const Variant &, Variant &, bool &)>(&Variant::evaluate),
						asmjit::FuncSignature::build<void, const Variant::Operator &, const Variant &, const Variant &, Variant &, bool &>());
				evaluate_invoke->setArg(0, op_ptr);
				evaluate_invoke->setArg(1, left_ptr);
				evaluate_invoke->setArg(2, right_ptr);
				evaluate_invoke->setArg(3, dst_ptr);
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
					if (operation_name.contains("FLOAT") || operation_name.contains("INT_FLOAT") || operation_name.contains("FLOAT_INT")) {
						handle_float_operation(operation_name, context, left_addr, right_addr, result_addr);
					} else {
						asmjit::x86::Gp left_val = extract_int_from_variant(context, left_addr);
						asmjit::x86::Gp right_val = extract_int_from_variant(context, right_addr);
						asmjit::x86::Gp result_val = cc.newInt64();

						handle_int_operation(operation_name, context, left_val, right_val, result_val);

						store_reg_to_variant(context, result_val, result_addr);
					}
				} else {
					asmjit::x86::Gp left_ptr = get_variant_ptr(context, left_addr);
					asmjit::x86::Gp right_ptr = get_variant_ptr(context, right_addr);
					asmjit::x86::Gp op_ptr = get_variant_ptr(context, result_addr);

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

			case GDScriptFunction::OPCODE_SET_KEYED: {
				int base_addr = gdscript->_code_ptr[ip + 1];
				int key_addr = gdscript->_code_ptr[ip + 2];
				int value_addr = gdscript->_code_ptr[ip + 3];

				print_line(ip, "SET_KEYED");

				asmjit::x86::Gp base_ptr = get_variant_ptr(context, base_addr);
				asmjit::x86::Gp key_ptr = get_variant_ptr(context, key_addr);
				asmjit::x86::Gp value_ptr = get_variant_ptr(context, value_addr);

				asmjit::x86::Mem valid_mem = cc.newStack(sizeof(bool), 16);
				asmjit::x86::Gp valid_ptr = cc.newIntPtr("valid_ptr");
				cc.lea(valid_ptr, valid_mem);
				cc.mov(asmjit::x86::byte_ptr(valid_ptr), 1);

				asmjit::InvokeNode *set_invoke;
				cc.invoke(&set_invoke, &set_keyed,
						asmjit::FuncSignature::build<void, Variant *, const Variant *, const Variant *, bool *>());
				set_invoke->setArg(0, base_ptr);
				set_invoke->setArg(1, key_ptr);
				set_invoke->setArg(2, value_ptr);
				set_invoke->setArg(3, valid_ptr);

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

				asmjit::x86::Gp base_ptr = get_variant_ptr(context, base_addr);
				asmjit::x86::Gp value_ptr = get_variant_ptr(context, value_addr);
				asmjit::x86::Gp index_val = extract_int_from_variant(context, index_addr);

				asmjit::x86::Mem oob_mem = cc.newStack(sizeof(bool), 16);
				asmjit::x86::Gp oob_ptr = cc.newIntPtr("oob_ptr"); //FIX
				cc.lea(oob_ptr, oob_mem);
				cc.mov(asmjit::x86::byte_ptr(oob_ptr), 0);

				asmjit::InvokeNode *setter_invoke;
				cc.invoke(&setter_invoke, setter_func,
						asmjit::FuncSignature::build<void, Variant *, int64_t, const Variant *, bool *>());
				setter_invoke->setArg(0, base_ptr);
				setter_invoke->setArg(1, index_val);
				setter_invoke->setArg(2, value_ptr);
				setter_invoke->setArg(3, oob_ptr);

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

				asmjit::x86::Gp base_ptr = get_variant_ptr(context, base_addr);
				asmjit::x86::Gp key_ptr = get_variant_ptr(context, key_addr);
				asmjit::x86::Gp dst_ptr = get_variant_ptr(context, result_addr);

				asmjit::x86::Mem valid_mem = cc.newStack(sizeof(bool), 16);
				asmjit::x86::Gp valid_ptr = cc.newIntPtr("valid_ptr");
				cc.lea(valid_ptr, valid_mem);
				cc.mov(asmjit::x86::byte_ptr(valid_ptr), 1);

				asmjit::InvokeNode *get_invoke;
				cc.invoke(&get_invoke, &get_keyed, asmjit::FuncSignature::build<void, const Variant *, const Variant *, Variant *, bool *>());
				get_invoke->setArg(0, base_ptr);
				get_invoke->setArg(1, key_ptr);
				get_invoke->setArg(2, dst_ptr);
				get_invoke->setArg(3, valid_ptr);

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

				asmjit::x86::Gp base_ptr = get_variant_ptr(context, base_addr);
				asmjit::x86::Gp dst_ptr = get_variant_ptr(context, result_addr);

				asmjit::x86::Gp index_val = extract_int_from_variant(context, index_addr);

				asmjit::x86::Mem oob_mem = cc.newStack(sizeof(bool), 16);
				asmjit::x86::Gp oob_ptr = cc.newIntPtr("oob_ptr"); //FIX
				cc.lea(oob_ptr, oob_mem);
				cc.mov(asmjit::x86::byte_ptr(oob_ptr), 0);

				asmjit::InvokeNode *getter_invoke;
				cc.invoke(&getter_invoke, getter_func,
						asmjit::FuncSignature::build<void, const Variant *, int64_t, Variant *, bool *>());
				getter_invoke->setArg(0, base_ptr);
				getter_invoke->setArg(1, index_val);
				getter_invoke->setArg(2, dst_ptr);
				getter_invoke->setArg(3, oob_ptr);

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

				asmjit::x86::Gp base_ptr = get_variant_ptr(context, base_addr);
				asmjit::x86::Gp value_ptr = get_variant_ptr(context, value_addr);

				asmjit::x86::Mem valid_mem = cc.newStack(sizeof(bool), 16);
				asmjit::x86::Gp valid_ptr = cc.newIntPtr("valid_ptr");
				cc.lea(valid_ptr, valid_mem);
				cc.mov(asmjit::x86::byte_ptr(valid_ptr), 1);

				asmjit::x86::Gp name_ptr = cc.newIntPtr("name_ptr");
				cc.mov(name_ptr, &gdscript->_global_names_ptr[name_idx]);

				asmjit::InvokeNode *set_invoke;
				cc.invoke(&set_invoke, &set_named,
						asmjit::FuncSignature::build<void, Variant &, const StringName &, const Variant *, bool &>());
				set_invoke->setArg(0, base_ptr);
				set_invoke->setArg(1, name_ptr);
				set_invoke->setArg(2, value_ptr);
				set_invoke->setArg(3, valid_ptr);

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

				asmjit::x86::Gp base_ptr = get_variant_ptr(context, base_addr);
				asmjit::x86::Gp value_ptr = get_variant_ptr(context, value_addr);

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

				asmjit::x86::Gp base_ptr = get_variant_ptr(context, base_addr);
				asmjit::x86::Gp value_ptr = get_variant_ptr(context, result_addr);

				asmjit::x86::Mem valid_mem = cc.newStack(sizeof(bool), 16);
				asmjit::x86::Gp valid_ptr = cc.newIntPtr("valid_ptr");
				cc.lea(valid_ptr, valid_mem);
				cc.mov(asmjit::x86::byte_ptr(valid_ptr), 1);

				asmjit::x86::Gp name_ptr = cc.newIntPtr("name_ptr");
				cc.mov(name_ptr, &gdscript->_global_names_ptr[name_idx]);

				asmjit::InvokeNode *get_invoke;
				cc.invoke(&get_invoke, &get_named,
						asmjit::FuncSignature::build<void, const Variant &, const StringName &, Variant *, bool &>());
				get_invoke->setArg(0, base_ptr);
				get_invoke->setArg(1, name_ptr);
				get_invoke->setArg(2, value_ptr);
				get_invoke->setArg(3, valid_ptr);

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

				asmjit::x86::Gp base_ptr = get_variant_ptr(context, base_addr);
				asmjit::x86::Gp value_ptr = get_variant_ptr(context, result_addr);

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

			case GDScriptFunction::OPCODE_ASSIGN: {
				int dst_addr = gdscript->_code_ptr[ip + 1];
				int src_addr = gdscript->_code_ptr[ip + 2];

				asmjit::x86::Gp src_ptr = get_variant_ptr(context, src_addr);
				asmjit::x86::Gp dst_ptr = get_variant_ptr(context, dst_addr);

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

				asmjit::x86::Gp dst_ptr = get_variant_ptr(context, dst_addr);
				asmjit::x86::Gp src_ptr = cc.newIntPtr("null_ptr");

				context.cc->lea(src_ptr, asmjit::x86::ptr(context.stack_ptr, 2 * STACK_SLOT_SIZE)); // STACK[2] - Nil

				copy_variant(context, dst_ptr, src_ptr);

				print_line(ip, "ASSIGN_NULL");
				print_line("    Destination:");
				print_address_info(gdscript, dst_addr);

				incr = 2;
			} break;
			case GDScriptFunction::OPCODE_ASSIGN_TRUE: {
				int dst_addr = gdscript->_code_ptr[ip + 1];

				int dst_type, dst_index;
				decode_address(dst_addr, dst_type, dst_index);

				//idk where its used

				print_line(ip, " ASSIGN_TRUE");
				incr = 2;
			} break;
			case GDScriptFunction::OPCODE_ASSIGN_FALSE: {
				int dst_addr = gdscript->_code_ptr[ip + 1];

				int dst_type, dst_index;
				decode_address(dst_addr, dst_type, dst_index);

				//idk where its used

				print_line(ip, " ASSIGN_FALSE");
				incr = 2;
			} break;
			case GDScriptFunction::OPCODE_ASSIGN_TYPED_BUILTIN: {
				int dst_addr = gdscript->_code_ptr[ip + 1];
				int src_addr = gdscript->_code_ptr[ip + 2];
				Variant::Type target_type = (Variant::Type)gdscript->_code_ptr[ip + 3];

				asmjit::x86::Gp src_ptr = get_variant_ptr(context, src_addr);
				asmjit::x86::Gp dst_ptr = get_variant_ptr(context, dst_addr);
				asmjit::x86::Gp arg_ptr = get_variant_ptr(context, src_addr);

				asmjit::x86::Gp args_array = cc.newIntPtr("args_array");
				cc.lea(args_array, cc.newStack(PTR_SIZE, 16));

				cc.mov(asmjit::x86::ptr(args_array, 0), arg_ptr);

				asmjit::x86::Gp call_error_ptr = get_call_error_ptr(context);

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
				int dst_addr = gdscript->_code_ptr[ip];
				int argc = gdscript->_code_ptr[ip + 1];
				Variant::Type construct_type = (Variant::Type)gdscript->_code_ptr[ip + 2];

				print_line(ip - instr_arg_count - 1, "CONSTRUCT: ", Variant::get_type_name(construct_type), ", argc=", argc);

				asmjit::x86::Gp dst_ptr = get_variant_ptr(context, dst_addr);

				asmjit::x86::Gp args_array = prepare_args_array(context, argc, ip - argc);
				asmjit::x86::Gp call_error_ptr = get_call_error_ptr(context);

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

				asmjit::x86::Gp args_array = prepare_args_array(context, argc, ip - argc);

				asmjit::x86::Gp dst_ptr = get_variant_ptr(context, dst_addr);

				asmjit::InvokeNode *construct_invoke;
				cc.invoke(&construct_invoke, constructor, asmjit::FuncSignature::build<void, Variant *, const Variant **>());
				construct_invoke->setArg(0, dst_ptr);
				construct_invoke->setArg(1, args_array);

				print_line("    Result:");
				print_address_info(gdscript, dst_addr);

				incr = 3;
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

				asmjit::x86::Gp base_ptr = get_variant_ptr(context, base_addr);
				asmjit::x86::Gp dst_ptr = get_variant_ptr(context, dst_addr);

				asmjit::x86::Gp args_array = prepare_args_array(context, argc, ip - instr_arg_count + 1);
				asmjit::x86::Gp call_error_ptr = get_call_error_ptr(context);

				asmjit::x86::Gp function_name_ptr = cc.newIntPtr("function_name_ptr");
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

				asmjit::x86::Gp args_array = prepare_args_array(context, argc, ip - argc);

				asmjit::x86::Gp dst_ptr = get_variant_ptr(context, dst_addr);

				asmjit::x86::Gp function_name_ptr = cc.newIntPtr("function_name_ptr");
				cc.mov(function_name_ptr, &gdscript->_global_names_ptr[utility_name_idx]);

				asmjit::x86::Gp call_error_ptr = get_call_error_ptr(context);

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

				asmjit::x86::Gp args_array = prepare_args_array(context, argc, ip - argc);

				asmjit::x86::Gp dst_ptr = get_variant_ptr(context, dst_addr);

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

				asmjit::x86::Gp args_array = prepare_args_array(context, argc, ip - argc);
				asmjit::x86::Gp dst_ptr = get_variant_ptr(context, dst_addr);
				asmjit::x86::Gp call_error_ptr = get_call_error_ptr(context);

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

				asmjit::x86::Gp base_ptr = get_variant_ptr(context, base_addr);
				asmjit::x86::Gp dst_ptr = get_variant_ptr(context, dst_addr);
				asmjit::x86::Gp args_array = prepare_args_array(context, argc, ip - argc);

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

			case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN: {
				int instr_arg_count = gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				int base_addr = gdscript->_code_ptr[ip - 1];
				int dst_addr = gdscript->_code_ptr[ip];
				int argc = gdscript->_code_ptr[ip + 1];
				int method_idx = gdscript->_code_ptr[ip + 2];

				MethodBind *method = gdscript->_methods_ptr[method_idx];
				print_line(ip - instr_arg_count - 1, "CALL_METHOD_BIND_VALIDATED_RETURN: ", method->get_name(), ", argc=", argc);

				asmjit::x86::Gp base_ptr = get_variant_ptr(context, base_addr);
				asmjit::x86::Gp dst_ptr = get_variant_ptr(context, dst_addr);

				asmjit::x86::Gp base_obj = context.cc->newIntPtr("base_obj");
				context.cc->mov(base_obj, asmjit::x86::ptr(base_ptr, offsetof(Variant, _data) + offsetof(Variant::ObjData, obj)));

				asmjit::x86::Gp args_array = prepare_args_array(context, argc, ip - argc);

				asmjit::InvokeNode *method_invoke;
				context.cc->invoke(&method_invoke,
						static_cast<void (*)(MethodBind *, Object *, const Variant **, Variant *)>(
								[](MethodBind *method, Object *obj, const Variant **args, Variant *ret) {
									method->validated_call(obj, args, ret);
								}),
						asmjit::FuncSignature::build<void, MethodBind *, Object *, const Variant **, Variant *>());
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

				cc.jmp(jump_labels[target]);

				print_line(ip, "JUMP to: ", gdscript->_code_ptr[ip + 1]);
				incr += 2;
			} break;
			case GDScriptFunction::OPCODE_JUMP_IF: {
				int condition_addr = gdscript->_code_ptr[ip + 1];
				int target = gdscript->_code_ptr[ip + 2];

				asmjit::x86::Gp condition = extract_int_from_variant(context, condition_addr);

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

				asmjit::x86::Gp condition = extract_int_from_variant(context, condition_addr);

				cc.test(condition, condition);
				cc.jz(jump_labels[target]);

				print_line(ip, "JUMP_IF_NOT to: ", target);
				print_line("    Condition:");
				print_address_info(gdscript, condition_addr);

				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_RETURN: {
				int dst_addr = gdscript->_code_ptr[ip + 1];

				asmjit::x86::Gp src_ptr = get_variant_ptr(context, dst_addr);
				asmjit::x86::Gp dst_ptr = cc.newIntPtr("dst_addr");

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

				asmjit::x86::Gp src_ptr = get_variant_ptr(context, dst_addr);
				asmjit::x86::Gp dst_ptr = cc.newIntPtr("dst_addr");

				cc.mov(dst_ptr, result_ptr);

				cast_and_store(context, src_ptr, dst_ptr, gdscript->return_type.builtin_type, dst_addr);
				cc.ret();

				print_line(ip, "RETURN BUILTIN: ", Variant::get_type_name(gdscript->return_type.builtin_type));
				print_line("    Return value:");
				print_address_info(gdscript, dst_addr);
				incr = 3;
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

				asmjit::x86::Gp container_ptr = get_variant_ptr(context, container_addr);
				asmjit::x86::Gp counter_ptr = get_variant_ptr(context, counter_addr);
				asmjit::x86::Gp iterator_ptr = get_variant_ptr(context, iterator_addr);

				asmjit::InvokeNode *init_counter;
				cc.invoke(&init_counter, &initialize_counter_int,
						asmjit::FuncSignature::build<void, Variant *>());
				init_counter->setArg(0, counter_ptr);

				asmjit::InvokeNode *is_empty_call;
				cc.invoke(&is_empty_call, &is_array_empty,
						asmjit::FuncSignature::build<bool, const Variant *>());
				is_empty_call->setArg(0, container_ptr);

				asmjit::x86::Gp is_empty_result = cc.newInt8("is_empty_result");
				is_empty_call->setRet(0, is_empty_result);

				asmjit::Label empty_array_label = cc.newLabel();
				asmjit::Label continue_label = cc.newLabel();

				cc.test(is_empty_result, is_empty_result);
				cc.jnz(empty_array_label);

				asmjit::InvokeNode *get_first_element;
				cc.invoke(&get_first_element, &get_array_first_element,
						asmjit::FuncSignature::build<void, const Variant *, Variant *>());
				get_first_element->setArg(0, container_ptr);
				get_first_element->setArg(1, iterator_ptr);

				cc.jmp(continue_label);

				cc.bind(empty_array_label);
				cc.jmp(jump_labels[jump_target]);

				cc.bind(continue_label);

				incr = 5;
			} break;

			case GDScriptFunction::OPCODE_ITERATE_ARRAY: {
				int counter_addr = gdscript->_code_ptr[ip + 1];
				int container_addr = gdscript->_code_ptr[ip + 2];
				int iterator_addr = gdscript->_code_ptr[ip + 3];
				int jump_target = gdscript->_code_ptr[ip + 4];

				print_line(ip, "ITERATE_ARRAY, jump to: ", jump_target);
				print_line("    Counter:");
				print_address_info(gdscript, counter_addr);
				print_line("    Container:");
				print_address_info(gdscript, container_addr);
				print_line("    Iterator:");
				print_address_info(gdscript, iterator_addr);

				asmjit::x86::Gp container_ptr = get_variant_ptr(context, container_addr);
				asmjit::x86::Gp counter_ptr = get_variant_ptr(context, counter_addr);
				asmjit::x86::Gp iterator_ptr = get_variant_ptr(context, iterator_addr);

				asmjit::InvokeNode *iterate_call;
				cc.invoke(&iterate_call, &iterate_array_step,
						asmjit::FuncSignature::build<bool, Variant *, const Variant *, Variant *>());
				iterate_call->setArg(0, counter_ptr);
				iterate_call->setArg(1, container_ptr);
				iterate_call->setArg(2, iterator_ptr);

				asmjit::x86::Gp continue_iteration = cc.newInt8("continue_iteration");
				iterate_call->setRet(0, continue_iteration);

				asmjit::Label end_iteration_label = cc.newLabel();
				asmjit::Label continue_label = cc.newLabel();

				cc.test(continue_iteration, continue_iteration);
				cc.jz(end_iteration_label);
				cc.jmp(continue_label);

				cc.bind(end_iteration_label);
				cc.jmp(jump_labels[jump_target]);

				cc.bind(continue_label);

				incr = 5;
			} break;

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

asmjit::x86::Gp JitCompiler::get_variant_ptr(JitContext &context, int address) {
	int address_type, address_index;
	decode_address(address, address_type, address_index);

	asmjit::x86::Gp variant_ptr = context.cc->newIntPtr();

	if (address_type == GDScriptFunction::ADDR_TYPE_CONSTANT) {
		context.cc->mov(variant_ptr, (intptr_t)context.gdscript->_constants_ptr);
		context.cc->lea(variant_ptr, asmjit::x86::ptr(variant_ptr, address_index * sizeof(Variant)));
	} else if (address_type == GDScriptFunction::ADDR_TYPE_STACK) {
		context.cc->lea(variant_ptr, asmjit::x86::ptr(context.stack_ptr, address_index * STACK_SLOT_SIZE));
	} else if (address_type == GDScriptFunction::ADDR_TYPE_MEMBER) {
		context.cc->lea(variant_ptr, asmjit::x86::ptr(context.members_ptr, address_index * sizeof(Variant)));
	}

	return variant_ptr;
}

void JitCompiler::handle_int_operation(String &operation_name, JitContext &ctx, asmjit::x86::Gp &left_val, asmjit::x86::Gp &right_val, asmjit::x86::Gp &result_mem) {
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

//FIX ME
void JitCompiler::handle_float_operation(String &operation_name, JitContext &ctx, int left_addr, int right_addr, int result_addr) {
	if (operation_name.begins_with("ADD_FLOAT") || operation_name.begins_with("SUBTRACT_FLOAT") ||
			operation_name.begins_with("MULTIPLY_FLOAT") || operation_name.begins_with("DIVIDE_FLOAT")) {
		asmjit::x86::Xmm left_val = ctx.cc->newXmmSd();
		asmjit::x86::Xmm right_val = ctx.cc->newXmmSd();

		if (operation_name.contains("INT_FLOAT")) {
			asmjit::x86::Gp int_val = extract_int_from_variant(ctx, left_addr);
			convert_int_to_float(ctx, int_val, left_val);
			extract_float_from_variant(ctx, right_val, right_addr);
		} else if (operation_name.contains("FLOAT_INT")) {
			extract_float_from_variant(ctx, left_val, left_addr);
			asmjit::x86::Gp int_val = extract_int_from_variant(ctx, right_addr);
			convert_int_to_float(ctx, int_val, right_val);
		} else {
			extract_float_from_variant(ctx, left_val, left_addr);
			extract_float_from_variant(ctx, right_val, right_addr);
		}

		if (operation_name.begins_with("ADD_")) {
			ctx.cc->addsd(left_val, right_val);
		} else if (operation_name.begins_with("SUBTRACT_")) {
			ctx.cc->subsd(left_val, right_val);
		} else if (operation_name.begins_with("MULTIPLY_")) {
			ctx.cc->mulsd(left_val, right_val);
		} else if (operation_name.begins_with("DIVIDE_")) {
			ctx.cc->divsd(left_val, right_val);
		}

		store_float_to_variant(ctx, left_val, result_addr);

	} else if (operation_name.begins_with("EQUAL_FLOAT") || operation_name.begins_with("NOT_EQUAL_FLOAT") ||
			operation_name.begins_with("LESS_FLOAT") || operation_name.begins_with("GREATER_FLOAT") ||
			operation_name.contains("EQUAL_") && (operation_name.contains("FLOAT") || operation_name.contains("INT_FLOAT") || operation_name.contains("FLOAT_INT"))) {
		asmjit::x86::Xmm left_val = ctx.cc->newXmmSd();
		asmjit::x86::Xmm right_val = ctx.cc->newXmmSd();

		if (operation_name.contains("INT_FLOAT")) {
			asmjit::x86::Gp int_val = extract_int_from_variant(ctx, left_addr);
			convert_int_to_float(ctx, int_val, left_val);
			extract_float_from_variant(ctx, right_val, right_addr);
		} else if (operation_name.contains("FLOAT_INT")) {
			extract_float_from_variant(ctx, left_val, left_addr);
			asmjit::x86::Gp int_val = extract_int_from_variant(ctx, right_addr);
			convert_int_to_float(ctx, int_val, right_val);
		} else {
			extract_float_from_variant(ctx, left_val, left_addr);
			extract_float_from_variant(ctx, right_val, right_addr);
		}

		ctx.cc->comisd(left_val, right_val);

		asmjit::x86::Gp result_reg = ctx.cc->newInt64();

		if (operation_name.begins_with("EQUAL_")) {
			ctx.cc->sete(result_reg.r8());
		} else if (operation_name.begins_with("NOT_EQUAL_")) {
			ctx.cc->setne(result_reg.r8());
		} else if (operation_name.begins_with("LESS_EQUAL_")) {
			ctx.cc->setbe(result_reg.r8());
		} else if (operation_name.begins_with("LESS_")) {
			ctx.cc->setb(result_reg.r8());
		} else if (operation_name.begins_with("GREATER_EQUAL_")) {
			ctx.cc->setae(result_reg.r8());
		} else if (operation_name.begins_with("GREATER_")) {
			ctx.cc->seta(result_reg.r8());
		}

		ctx.cc->movzx(result_reg, result_reg.r8());
		store_reg_to_variant(ctx, result_reg, result_addr);

		asmjit::x86::Gp variant_ptr = get_variant_ptr(ctx, result_addr);
		ctx.cc->mov(asmjit::x86::dword_ptr(variant_ptr, 0), (int)Variant::BOOL);
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

HashMap<int, asmjit::Label> JitCompiler::analyze_jump_targets(JitContext &context) {
	HashMap<int, asmjit::Label> jump_labels;

	print_line("\n=== Analyzing Jump Targets ===");

	int ip = 0;
	while (ip < context.gdscript->code.size()) {
		int incr = 0;
		GDScriptFunction::Opcode opcode = GDScriptFunction::Opcode(context.gdscript->_code_ptr[ip]);

		switch (opcode) {
			case GDScriptFunction::OPCODE_OPERATOR: {
				constexpr int _pointer_size = sizeof(Variant::ValidatedOperatorEvaluator) / sizeof(*context.gdscript->_code_ptr);
				incr = 7 + _pointer_size;
			} break;

			case GDScriptFunction::OPCODE_OPERATOR_VALIDATED: {
				incr = 5;
			} break;

			case GDScriptFunction::OPCODE_SET_KEYED:
			case GDScriptFunction::OPCODE_GET_KEYED: {
				incr = 4;
			} break;

			case GDScriptFunction::OPCODE_GET_INDEXED_VALIDATED:
			case GDScriptFunction::OPCODE_SET_INDEXED_VALIDATED: {
				incr = 5;
			} break;

			case GDScriptFunction::OPCODE_SET_NAMED:
			case GDScriptFunction::OPCODE_GET_NAMED:
			case GDScriptFunction::OPCODE_GET_NAMED_VALIDATED:
			case GDScriptFunction::OPCODE_SET_NAMED_VALIDATED: {
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
			case GDScriptFunction::OPCODE_CALL:
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
			case GDScriptFunction::OPCODE_CALL_GDSCRIPT_UTILITY: {
				int instr_arg_count = context.gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_CALL_BUILTIN_TYPE_VALIDATED: {
				int instr_arg_count = context.gdscript->_code_ptr[++ip];
				ip += instr_arg_count;
				incr = 3;
			} break;
			case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN: {
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

			case GDScriptFunction::OPCODE_RETURN: {
				incr = 2;
			} break;

			case GDScriptFunction::OPCODE_RETURN_TYPED_BUILTIN: {
				incr = 3;
			} break;

			case GDScriptFunction::OPCODE_ITERATE_BEGIN_ARRAY:
			case GDScriptFunction::OPCODE_ITERATE_ARRAY: {
				int jump_target = context.gdscript->_code_ptr[ip + 4];
				if (!jump_labels.has(jump_target)) {
					jump_labels[jump_target] = context.cc->newLabel();
					print_line("Created label for ITERATE target: ", jump_target);
				}
				incr = 5;
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

	return jump_labels;
}

void JitCompiler::copy_variant(JitContext &context, asmjit::x86::Gp &dst_ptr, asmjit::x86::Gp &src_ptr) {
	asmjit::InvokeNode *copy_invoke;
	context.cc->invoke(&copy_invoke,
			static_cast<void (*)(Variant *, const Variant *)>([](Variant *dst, const Variant *src) {
				*dst = *src;
			}),
			asmjit::FuncSignature::build<void, Variant *, const Variant *>());
	copy_invoke->setArg(0, dst_ptr);
	copy_invoke->setArg(1, src_ptr);
}

asmjit::x86::Gp JitCompiler::extract_int_from_variant(JitContext &context, int address) {
	asmjit::x86::Gp result_reg = context.cc->newInt64();
	int address_type, address_index;
	decode_address(address, address_type, address_index);

	asmjit::x86::Gp variant_ptr = get_variant_ptr(context, address);

	context.cc->mov(result_reg, asmjit::x86::qword_ptr(variant_ptr, OFFSET_INT));

	return result_reg;
}

void JitCompiler::extract_type_from_variant(JitContext &context, asmjit::x86::Gp &result_reg, int address) {
	int address_type, address_index;
	decode_address(address, address_type, address_index);

	asmjit::x86::Gp variant_ptr = get_variant_ptr(context, address);

	context.cc->mov(result_reg.r32(), asmjit::x86::dword_ptr(variant_ptr, 0));
	context.cc->movzx(result_reg, result_reg.r32());
}

void JitCompiler::extract_float_from_variant(JitContext &context, asmjit::x86::Xmm &result_reg, int address) {
	int address_type, address_index;
	decode_address(address, address_type, address_index);

	asmjit::x86::Gp variant_ptr = get_variant_ptr(context, address);

	context.cc->movsd(result_reg, asmjit::x86::qword_ptr(variant_ptr, OFFSET_FLOAT));
}

void JitCompiler::store_float_to_variant(JitContext &context, asmjit::x86::Xmm &value, int address) {
	int address_type, address_index;
	decode_address(address, address_type, address_index);

	asmjit::x86::Gp variant_ptr = get_variant_ptr(context, address);

	context.cc->mov(asmjit::x86::dword_ptr(variant_ptr, 0), (int)Variant::FLOAT);
	context.cc->movsd(asmjit::x86::qword_ptr(variant_ptr, OFFSET_FLOAT), value);
}

void JitCompiler::convert_int_to_float(JitContext &context, asmjit::x86::Gp &int_reg, asmjit::x86::Xmm &float_reg) {
	context.cc->cvtsi2sd(float_reg, int_reg);
}

void JitCompiler::store_reg_to_variant(JitContext &context, asmjit::x86::Gp &value, int address) {
	int address_type, address_index;
	decode_address(address, address_type, address_index);

	asmjit::x86::Gp variant_ptr = get_variant_ptr(context, address);

	context.cc->mov(asmjit::x86::qword_ptr(variant_ptr, OFFSET_INT), value);
}

void JitCompiler::store_int_to_variant(JitContext &context, int value, int address) {
	int address_type, address_index;
	decode_address(address, address_type, address_index);

	asmjit::x86::Gp variant_ptr = get_variant_ptr(context, address);
	context.cc->mov(asmjit::x86::qword_ptr(variant_ptr, OFFSET_INT), value);
}

asmjit::x86::Gp JitCompiler::create_call_error(JitContext &context) {
	asmjit::x86::Mem call_error_mem = context.cc->newStack(sizeof(Callable::CallError), 16);
	asmjit::x86::Gp call_error_ptr = context.cc->newIntPtr("call_error_ptr");
	context.cc->lea(call_error_ptr, call_error_mem);

	return call_error_ptr;
}

asmjit::x86::Gp JitCompiler::get_call_error_ptr(JitContext &context, bool reset) {
	if (reset) {
		context.cc->mov(asmjit::x86::dword_ptr(context.shared_call_error_ptr, 0), (int)Callable::CallError::CALL_OK);
	}
	return context.shared_call_error_ptr;
}

asmjit::x86::Gp JitCompiler::prepare_args_array(JitContext &context, int argc, int ip_base) {
	asmjit::x86::Gp args_array = context.cc->newIntPtr("args_array");

	if (argc > 0) {
		int args_array_size = argc * PTR_SIZE;
		asmjit::x86::Mem args_stack = context.cc->newStack(args_array_size, 16);
		context.cc->lea(args_array, args_stack);

		for (int i = 0; i < argc; i++) {
			int arg_addr = context.gdscript->_code_ptr[ip_base + i];

			asmjit::x86::Gp arg_ptr = get_variant_ptr(context, arg_addr);

			context.cc->mov(asmjit::x86::ptr(args_array, i * PTR_SIZE), arg_ptr);

			print_line("    Arg[", i, "]");
			print_address_info(context.gdscript, arg_addr);
		}
	} else {
		context.cc->mov(args_array, 0);
	}

	return args_array;
}

void JitCompiler::cast_and_store(JitContext &context, asmjit::x86::Gp &src_ptr, asmjit::x86::Gp &dst_ptr, Variant::Type expected_type, int return_addr) {
	if (expected_type == Variant::NIL) {
		copy_variant(context, dst_ptr, src_ptr);
		return;
	}

	asmjit::x86::Gp current_type = context.cc->newInt32("current_type");
	extract_type_from_variant(context, current_type, return_addr);

	asmjit::Label same_type_label = context.cc->newLabel();
	asmjit::Label end_label = context.cc->newLabel();

	context.cc->cmp(current_type, (int)expected_type);
	context.cc->je(same_type_label);

	{
		asmjit::x86::Gp args_array = context.cc->newIntPtr("cast_args_array");
		context.cc->lea(args_array, context.cc->newStack(PTR_SIZE, 16));
		context.cc->mov(asmjit::x86::ptr(args_array, 0), src_ptr);

		context.cc->mov(asmjit::x86::dword_ptr(dst_ptr, 0), (int)expected_type);

		asmjit::x86::Gp call_error_ptr = get_call_error_ptr(context);

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