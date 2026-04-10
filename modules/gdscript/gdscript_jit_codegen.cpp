/**************************************************************************/
/*  gdscript_jit_codegen.cpp                                             */
/**************************************************************************/
/*                         This file is part of:                          */
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

#include "gdscript_jit_codegen.h"

#include "core/debugger/engine_debugger.h"

template <typename... Args>
void GDScriptJitCodeGenerator::print_debug(Args... p_args) const {
#if defined(DEBUG_ENABLED) && !defined(TESTS_ENABLED)
	Variant variants[sizeof...(p_args)] = { p_args... };
	__print_line(stringify_variants(Span(variants)));
#endif
}

String GDScriptJitCodeGenerator::get_address_name(const Address &address) {
	String mode;
	switch (address.mode) {
		case Address::SELF:
			mode = "SELF";
			break;
		case Address::CLASS:
			mode = "CLASS";
			break;
		case Address::MEMBER:
			mode = "MEMBER";
			break;
		case Address::CONSTANT:
			mode = "CONSTANT";
			break;
		case Address::LOCAL_VARIABLE:
			mode = "LOCAL_VARIABLE";
			break;
		case Address::FUNCTION_PARAMETER:
			mode = "FUNCTION_PARAMETER";
			break;
		case Address::TEMPORARY:
			mode = "TEMPORARY";
			break;
		default:
			mode = "UNKOWN";
			break;
	}

	return "Type: " + Variant::get_type_name(address.type.builtin_type) + ", " + mode + "(" + itos(address.address) + ")";
}

uint32_t GDScriptJitCodeGenerator::add_parameter(const StringName &p_name, bool p_is_optional, const GDScriptDataType &p_type) {
	print_debug("add_parameter");
	function->_argument_count++;
	function->argument_types.push_back(p_type);
	if (p_is_optional) {
		function->_default_arg_count++;
	}

	return add_local(p_name, p_type);
}

uint32_t GDScriptJitCodeGenerator::add_local(const StringName &p_name, const GDScriptDataType &p_type) {
	print_debug("add_local");
	int stack_pos = locals.size() + GDScriptFunction::FIXED_ADDRESSES_MAX;
	locals.push_back(StackSlot(p_type.builtin_type, p_type.can_contain_object()));
	add_stack_identifier(p_name, stack_pos);
	return stack_pos;
}

uint32_t GDScriptJitCodeGenerator::add_local_constant(const StringName &p_name, const Variant &p_constant) {
	print_debug("add_local_constant");
	int index = add_or_get_constant(p_constant);
	local_constants[p_name] = index;
	return index;
}

uint32_t GDScriptJitCodeGenerator::add_or_get_constant(const Variant &p_constant) {
	print_debug("add_or_get_constant");
	return get_constant_pos(p_constant);
}

uint32_t GDScriptJitCodeGenerator::add_or_get_name(const StringName &p_name) {
	print_debug("add_or_get_name");
	return get_name_map_pos(p_name);
}

uint32_t GDScriptJitCodeGenerator::add_temporary(const GDScriptDataType &p_type) {
	print_debug("add_temporary");
	Variant::Type temp_type = Variant::NIL;
	if (p_type.kind == GDScriptDataType::BUILTIN) {
		switch (p_type.builtin_type) {
			case Variant::NIL:
			case Variant::BOOL:
			case Variant::INT:
			case Variant::FLOAT:
			case Variant::STRING:
			case Variant::VECTOR2:
			case Variant::VECTOR2I:
			case Variant::RECT2:
			case Variant::RECT2I:
			case Variant::VECTOR3:
			case Variant::VECTOR3I:
			case Variant::TRANSFORM2D:
			case Variant::VECTOR4:
			case Variant::VECTOR4I:
			case Variant::PLANE:
			case Variant::QUATERNION:
			case Variant::AABB:
			case Variant::BASIS:
			case Variant::TRANSFORM3D:
			case Variant::PROJECTION:
			case Variant::COLOR:
			case Variant::STRING_NAME:
			case Variant::NODE_PATH:
			case Variant::RID:
			case Variant::CALLABLE:
			case Variant::SIGNAL:
				temp_type = p_type.builtin_type;
				break;
			case Variant::OBJECT:
			case Variant::DICTIONARY:
			case Variant::ARRAY:
			case Variant::PACKED_BYTE_ARRAY:
			case Variant::PACKED_INT32_ARRAY:
			case Variant::PACKED_INT64_ARRAY:
			case Variant::PACKED_FLOAT32_ARRAY:
			case Variant::PACKED_FLOAT64_ARRAY:
			case Variant::PACKED_STRING_ARRAY:
			case Variant::PACKED_VECTOR2_ARRAY:
			case Variant::PACKED_VECTOR3_ARRAY:
			case Variant::PACKED_COLOR_ARRAY:
			case Variant::PACKED_VECTOR4_ARRAY:
			case Variant::VARIANT_MAX:
				// Arrays, dictionaries, and objects are reference counted, so we don't use the pool for them.
				temp_type = Variant::NIL;
				break;
		}
	}

	if (!temporaries_pool.has(temp_type)) {
		temporaries_pool[temp_type] = List<int>();
	}

	List<int> &pool = temporaries_pool[temp_type];
	if (pool.is_empty()) {
		StackSlot new_temp(temp_type, p_type.can_contain_object());
		int idx = temporaries.size();
		pool.push_back(idx);
		temporaries.push_back(new_temp);
	}
	int slot = pool.front()->get();
	pool.pop_front();
	used_temporaries.push_back(slot);
	return slot;
}

void GDScriptJitCodeGenerator::pop_temporary() {
	print_debug("pop_temporary");
	ERR_FAIL_COND(used_temporaries.is_empty());
	int slot_idx = used_temporaries.back()->get();
	if (temporaries[slot_idx].can_contain_object) {
		// Avoid keeping in the stack long-lived references to objects,
		// which may prevent `RefCounted` objects from being freed.
		// However, the cleanup will be performed an the end of the
		// statement, to allow object references to survive chaining.
		temporaries_pending_clear.insert(slot_idx);
	}
	temporaries_pool[temporaries[slot_idx].type].push_back(slot_idx);
	used_temporaries.pop_back();
}

void GDScriptJitCodeGenerator::start_parameters() {
	print_debug("start_parameters");
	if (function->_default_arg_count > 0) {
		function->default_arguments.push_back(opcodes.size());
		default_parameters_active = true;
		default_parameter_index = 0;
		if (function->_default_arg_count > 1) {
			default_parameter_labels.resize(function->_default_arg_count - 1);
			for (int i = 0; i < function->_default_arg_count - 1; i++) {
				default_parameter_labels.write[i] = ir.new_label();
			}
		}
		default_parameter_body_label = ir.new_label();
		default_parameter_defarg = ir.emit_load_defarg();
		ValueId zero = ir.emit_zero64();
		for (int missing = 0; missing < function->_default_arg_count; missing++) {
			ValueId expected = missing == 0 ? zero : ir.emit_add64(zero, missing);
			LabelId target = missing == 0 ? default_parameter_body_label : default_parameter_labels.write[function->_default_arg_count - missing - 1];
			ir.emit_jump_cc(IRCond::EQ, default_parameter_defarg, expected, target);
		}
	}
}

void GDScriptJitCodeGenerator::end_parameters() {
	print_debug("end_parameters");
	function->default_arguments.reverse();
	if (default_parameters_active) {
		ir.bind_label(default_parameter_body_label);
		default_parameters_active = false;
	}
}

void GDScriptJitCodeGenerator::write_start(GDScript *p_script, const StringName &p_function_name, bool p_static, Variant p_rpc_config, const GDScriptDataType &p_return_type) {
	function = memnew(GDScriptFunction);
	time = OS::get_singleton()->get_ticks_usec();
	default_parameters_active = false;
	default_parameter_index = 0;
	default_parameter_labels.clear();

	function->name = p_function_name;
	function->_script = p_script;
	function->source = p_script->get_script_path();

#ifdef DEBUG_ENABLED
	function->func_cname = (String(function->source) + " - " + String(p_function_name)).utf8();
	function->_func_cname = function->func_cname.get_data();
	print_debug((String(function->source) + " - " + String(p_function_name)));
	print_debug("write_start");
#endif

	function->_static = p_static;
	function->return_type = p_return_type;
	function->rpc_config = p_rpc_config;
	function->_argument_count = 0;
}

GDScriptFunction *GDScriptJitCodeGenerator::write_end() {
	print_debug("write_end \n");
#ifdef DEBUG_ENABLED
	if (!used_temporaries.is_empty()) {
		ERR_PRINT("Non-zero temporary variables at end of function: " + itos(used_temporaries.size()));
	}
#endif
	append_opcode(GDScriptFunction::OPCODE_END);

	for (int i = 0; i < temporaries.size(); i++) {
		int stack_index = i + max_locals + GDScriptFunction::FIXED_ADDRESSES_MAX;
		for (int j = 0; j < temporaries[i].bytecode_indices.size(); j++) {
			opcodes.write[temporaries[i].bytecode_indices[j]] = stack_index | (GDScriptFunction::ADDR_TYPE_STACK << GDScriptFunction::ADDR_BITS);
		}
		if (temporaries[i].type != Variant::NIL) {
			function->temporary_slots[stack_index] = temporaries[i].type;
		}
	}

	if (constant_map.size()) {
		function->_constant_count = constant_map.size();
		function->constants.resize(constant_map.size());
		function->_constants_ptr = function->constants.ptrw();
		for (const KeyValue<Variant, int> &K : constant_map) {
			function->constants.write[K.value] = K.key;
		}
	} else {
		function->_constants_ptr = nullptr;
		function->_constant_count = 0;
	}

	if (name_map.size()) {
		function->global_names.resize(name_map.size());
		function->_global_names_ptr = &function->global_names[0];
		for (const KeyValue<StringName, int> &E : name_map) {
			function->global_names.write[E.value] = E.key;
		}
		function->_global_names_count = function->global_names.size();

	} else {
		function->_global_names_ptr = nullptr;
		function->_global_names_count = 0;
	}

	if (opcodes.size()) {
		function->code = opcodes;
		function->_code_ptr = &function->code.write[0];
		function->_code_size = opcodes.size();

	} else {
		function->_code_ptr = nullptr;
		function->_code_size = 0;
	}

	if (function->default_arguments.size()) {
		function->_default_arg_count = function->default_arguments.size() - 1;
		function->_default_arg_ptr = &function->default_arguments[0];
	} else {
		function->_default_arg_count = 0;
		function->_default_arg_ptr = nullptr;
	}

	if (operator_func_map.size()) {
		function->operator_funcs.resize(operator_func_map.size());
		function->_operator_funcs_count = function->operator_funcs.size();
		function->_operator_funcs_ptr = function->operator_funcs.ptr();
		for (const KeyValue<Variant::ValidatedOperatorEvaluator, int> &E : operator_func_map) {
			function->operator_funcs.write[E.value] = E.key;
		}
	} else {
		function->_operator_funcs_count = 0;
		function->_operator_funcs_ptr = nullptr;
	}

	if (setters_map.size()) {
		function->setters.resize(setters_map.size());
		function->_setters_count = function->setters.size();
		function->_setters_ptr = function->setters.ptr();
		for (const KeyValue<Variant::ValidatedSetter, int> &E : setters_map) {
			function->setters.write[E.value] = E.key;
		}
	} else {
		function->_setters_count = 0;
		function->_setters_ptr = nullptr;
	}

	if (getters_map.size()) {
		function->getters.resize(getters_map.size());
		function->_getters_count = function->getters.size();
		function->_getters_ptr = function->getters.ptr();
		for (const KeyValue<Variant::ValidatedGetter, int> &E : getters_map) {
			function->getters.write[E.value] = E.key;
		}
	} else {
		function->_getters_count = 0;
		function->_getters_ptr = nullptr;
	}

	if (keyed_setters_map.size()) {
		function->keyed_setters.resize(keyed_setters_map.size());
		function->_keyed_setters_count = function->keyed_setters.size();
		function->_keyed_setters_ptr = function->keyed_setters.ptr();
		for (const KeyValue<Variant::ValidatedKeyedSetter, int> &E : keyed_setters_map) {
			function->keyed_setters.write[E.value] = E.key;
		}
	} else {
		function->_keyed_setters_count = 0;
		function->_keyed_setters_ptr = nullptr;
	}

	if (keyed_getters_map.size()) {
		function->keyed_getters.resize(keyed_getters_map.size());
		function->_keyed_getters_count = function->keyed_getters.size();
		function->_keyed_getters_ptr = function->keyed_getters.ptr();
		for (const KeyValue<Variant::ValidatedKeyedGetter, int> &E : keyed_getters_map) {
			function->keyed_getters.write[E.value] = E.key;
		}
	} else {
		function->_keyed_getters_count = 0;
		function->_keyed_getters_ptr = nullptr;
	}

	if (indexed_setters_map.size()) {
		function->indexed_setters.resize(indexed_setters_map.size());
		function->_indexed_setters_count = function->indexed_setters.size();
		function->_indexed_setters_ptr = function->indexed_setters.ptr();
		for (const KeyValue<Variant::ValidatedIndexedSetter, int> &E : indexed_setters_map) {
			function->indexed_setters.write[E.value] = E.key;
		}
	} else {
		function->_indexed_setters_count = 0;
		function->_indexed_setters_ptr = nullptr;
	}

	if (indexed_getters_map.size()) {
		function->indexed_getters.resize(indexed_getters_map.size());
		function->_indexed_getters_count = function->indexed_getters.size();
		function->_indexed_getters_ptr = function->indexed_getters.ptr();
		for (const KeyValue<Variant::ValidatedIndexedGetter, int> &E : indexed_getters_map) {
			function->indexed_getters.write[E.value] = E.key;
		}
	} else {
		function->_indexed_getters_count = 0;
		function->_indexed_getters_ptr = nullptr;
	}

	if (builtin_method_map.size()) {
		function->builtin_methods.resize(builtin_method_map.size());
		function->_builtin_methods_ptr = function->builtin_methods.ptr();
		function->_builtin_methods_count = builtin_method_map.size();
		for (const KeyValue<Variant::ValidatedBuiltInMethod, int> &E : builtin_method_map) {
			function->builtin_methods.write[E.value] = E.key;
		}
	} else {
		function->_builtin_methods_ptr = nullptr;
		function->_builtin_methods_count = 0;
	}

	if (constructors_map.size()) {
		function->constructors.resize(constructors_map.size());
		function->_constructors_ptr = function->constructors.ptr();
		function->_constructors_count = constructors_map.size();
		for (const KeyValue<Variant::ValidatedConstructor, int> &E : constructors_map) {
			function->constructors.write[E.value] = E.key;
		}
	} else {
		function->_constructors_ptr = nullptr;
		function->_constructors_count = 0;
	}

	if (utilities_map.size()) {
		function->utilities.resize(utilities_map.size());
		function->_utilities_ptr = function->utilities.ptr();
		function->_utilities_count = utilities_map.size();
		for (const KeyValue<Variant::ValidatedUtilityFunction, int> &E : utilities_map) {
			function->utilities.write[E.value] = E.key;
		}
	} else {
		function->_utilities_ptr = nullptr;
		function->_utilities_count = 0;
	}

	if (gds_utilities_map.size()) {
		function->gds_utilities.resize(gds_utilities_map.size());
		function->_gds_utilities_ptr = function->gds_utilities.ptr();
		function->_gds_utilities_count = gds_utilities_map.size();
		for (const KeyValue<GDScriptUtilityFunctions::FunctionPtr, int> &E : gds_utilities_map) {
			function->gds_utilities.write[E.value] = E.key;
		}
	} else {
		function->_gds_utilities_ptr = nullptr;
		function->_gds_utilities_count = 0;
	}

	if (method_bind_map.size()) {
		function->methods.resize(method_bind_map.size());
		function->_methods_ptr = function->methods.ptrw();
		function->_methods_count = method_bind_map.size();
		for (const KeyValue<MethodBind *, int> &E : method_bind_map) {
			function->methods.write[E.value] = E.key;
		}
	} else {
		function->_methods_ptr = nullptr;
		function->_methods_count = 0;
	}

	if (lambdas_map.size()) {
		function->lambdas.resize(lambdas_map.size());
		function->_lambdas_ptr = function->lambdas.ptrw();
		function->_lambdas_count = lambdas_map.size();
		for (const KeyValue<GDScriptFunction *, int> &E : lambdas_map) {
			function->lambdas.write[E.value] = E.key;
		}
	} else {
		function->_lambdas_ptr = nullptr;
		function->_lambdas_count = 0;
	}

	if (GDScriptLanguage::get_singleton()->should_track_locals()) {
		function->stack_debug = stack_debug;
	}
	function->_stack_size = GDScriptFunction::FIXED_ADDRESSES_MAX + max_locals + temporaries.size();
	function->_instruction_args_size = instr_args_max;

#ifdef DEBUG_ENABLED
	function->operator_names = operator_names;
	function->setter_names = setter_names;
	function->getter_names = getter_names;
	function->builtin_methods_names = builtin_methods_names;
	function->constructors_names = constructors_names;
	function->utilities_names = utilities_names;
	function->gds_utilities_names = gds_utilities_names;
#endif

	JitRuntimeManager::get_singleton()->compile(ir, function, max_locals);

	const auto elapsed = OS::get_singleton()->get_ticks_usec() - time;
	print_debug("write_end - elapsed time (usec): ", elapsed);

	ended = true;
	return function;
}

#ifdef DEBUG_ENABLED
void GDScriptJitCodeGenerator::set_signature(const String &p_signature) {
	print_debug("set_signature");
	function->profile.signature = p_signature;
}
#endif

void GDScriptJitCodeGenerator::set_initial_line(int p_line) {
	print_debug("set_initial_line");
	function->_initial_line = p_line;
}

#define HAS_BUILTIN_TYPE(m_var) \
	(m_var.type.kind == GDScriptDataType::BUILTIN)

#define IS_BUILTIN_TYPE(m_var, m_type) \
	(m_var.type.kind == GDScriptDataType::BUILTIN && m_var.type.builtin_type == m_type && m_type != Variant::NIL)

void GDScriptJitCodeGenerator::write_type_adjust(const Address &p_target, Variant::Type p_new_type) {
	print_debug("write_type_adjust");
	if (p_new_type == Variant::NIL || p_new_type == Variant::VARIANT_MAX) {
		return;
	}

	const ValueId target_ptr = ir.emit_load_ptr(p_target);
	ir.emit_type_adjust(target_ptr, p_new_type);
	switch (p_new_type) {
		case Variant::BOOL:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_BOOL);
			break;
		case Variant::INT:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_INT);
			break;
		case Variant::FLOAT:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_FLOAT);
			break;
		case Variant::STRING:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_STRING);
			break;
		case Variant::VECTOR2:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_VECTOR2);
			break;
		case Variant::VECTOR2I:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_VECTOR2I);
			break;
		case Variant::RECT2:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_RECT2);
			break;
		case Variant::RECT2I:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_RECT2I);
			break;
		case Variant::VECTOR3:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_VECTOR3);
			break;
		case Variant::VECTOR3I:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_VECTOR3I);
			break;
		case Variant::TRANSFORM2D:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_TRANSFORM2D);
			break;
		case Variant::VECTOR4:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_VECTOR4);
			break;
		case Variant::VECTOR4I:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_VECTOR4I);
			break;
		case Variant::PLANE:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_PLANE);
			break;
		case Variant::QUATERNION:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_QUATERNION);
			break;
		case Variant::AABB:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_AABB);
			break;
		case Variant::BASIS:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_BASIS);
			break;
		case Variant::TRANSFORM3D:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_TRANSFORM3D);
			break;
		case Variant::PROJECTION:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_PROJECTION);
			break;
		case Variant::COLOR:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_COLOR);
			break;
		case Variant::STRING_NAME:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_STRING_NAME);
			break;
		case Variant::NODE_PATH:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_NODE_PATH);
			break;
		case Variant::RID:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_RID);
			break;
		case Variant::OBJECT:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_OBJECT);
			break;
		case Variant::CALLABLE:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_CALLABLE);
			break;
		case Variant::SIGNAL:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_SIGNAL);
			break;
		case Variant::DICTIONARY:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_DICTIONARY);
			break;
		case Variant::ARRAY:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_ARRAY);
			break;
		case Variant::PACKED_BYTE_ARRAY:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_BYTE_ARRAY);
			break;
		case Variant::PACKED_INT32_ARRAY:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_INT32_ARRAY);
			break;
		case Variant::PACKED_INT64_ARRAY:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_INT64_ARRAY);
			break;
		case Variant::PACKED_FLOAT32_ARRAY:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_FLOAT32_ARRAY);
			break;
		case Variant::PACKED_FLOAT64_ARRAY:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_FLOAT64_ARRAY);
			break;
		case Variant::PACKED_STRING_ARRAY:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_STRING_ARRAY);
			break;
		case Variant::PACKED_VECTOR2_ARRAY:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_VECTOR2_ARRAY);
			break;
		case Variant::PACKED_VECTOR3_ARRAY:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_VECTOR3_ARRAY);
			break;
		case Variant::PACKED_COLOR_ARRAY:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_COLOR_ARRAY);
			break;
		case Variant::PACKED_VECTOR4_ARRAY:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_PACKED_VECTOR4_ARRAY);
			break;
		case Variant::NIL:
		case Variant::VARIANT_MAX:
			return;
	}
	append(p_target);
}

void GDScriptJitCodeGenerator::write_unary_operator(const Address &p_target, Variant::Operator p_operator, const Address &p_left_operand) {
	print_debug("write_unary_operator");
	if (HAS_BUILTIN_TYPE(p_left_operand)) {
		// Gather specific operator.
		Variant::ValidatedOperatorEvaluator op_func = Variant::get_validated_operator_evaluator(p_operator, p_left_operand.type.builtin_type, Variant::NIL);
		const int operator_pos = get_operation_pos(op_func);
		const auto v1 = ir.emit_load_ptr(p_left_operand);
		const auto v2 = ir.emit_load_ptr(Address());
		const auto v3 = ir.emit_load_ptr(p_target);
		ir.emit_call_binop({v1, v2, v3}, op_func);

		append_opcode(GDScriptFunction::OPCODE_OPERATOR_VALIDATED);
		append(p_left_operand);
		append(Address());
		append(p_target);
		append(op_func);
#ifdef DEBUG_ENABLED
		add_debug_name(operator_names, operator_pos, Variant::get_operator_name(p_operator));
#endif
		return;
	}

	// No specific types, perform variant evaluation.
	append_opcode(GDScriptFunction::OPCODE_OPERATOR);
	append(p_left_operand);
	append(Address());
	append(p_target);
	append(p_operator);
	append(0); // Signature storage.
	append(0); // Return type storage.
	constexpr int _pointer_size = sizeof(Variant::ValidatedOperatorEvaluator) / sizeof(*(opcodes.ptr()));
	for (int i = 0; i < _pointer_size; i++) {
		append(0); // Space for function pointer.
	}
}

void GDScriptJitCodeGenerator::write_binary_operator(const Address &p_target, Variant::Operator p_operator, const Address &p_left_operand, const Address &p_right_operand) {
	print_debug("write_binary_operator");
	bool valid = HAS_BUILTIN_TYPE(p_left_operand) && HAS_BUILTIN_TYPE(p_right_operand);

	if (valid &&
			p_left_operand.type.builtin_type == Variant::INT &&
			p_right_operand.type.builtin_type == Variant::INT &&
			p_target.type.kind == GDScriptDataType::BUILTIN &&
			p_target.type.builtin_type == Variant::INT) {
		bool used_fast_path = true;
		ValueId result{};
		const ValueId left = ir.emit_load(p_left_operand);
		const ValueId right = ir.emit_load(p_right_operand);

		switch (p_operator) {
			case Variant::OP_ADD:
				result = ir.emit_add64(left, right);
				break;
			case Variant::OP_SUBTRACT:
				result = ir.emit_sub64(left, right);
				break;
			case Variant::OP_MULTIPLY:
				result = ir.emit_mul64(left, right);
				break;
			default:
				used_fast_path = false;
				break;
		}

		if (used_fast_path) {
			ir.emit_store_type(p_target, Variant::INT);
			ir.emit_store(p_target, result);

			append_opcode(GDScriptFunction::OPCODE_OPERATOR_VALIDATED);
			append(p_left_operand);
			append(p_right_operand);
			append(p_target);
			append(Variant::get_validated_operator_evaluator(p_operator, Variant::INT, Variant::INT));
			return;
		}
	}

	if (valid &&
			p_left_operand.type.builtin_type == Variant::FLOAT &&
			p_right_operand.type.builtin_type == Variant::FLOAT &&
			p_target.type.kind == GDScriptDataType::BUILTIN &&
			p_target.type.builtin_type == Variant::FLOAT) {
		bool used_fast_path = true;
		ValueId result{};
		const ValueId left = ir.emit_loadf64(p_left_operand);
		const ValueId right = ir.emit_loadf64(p_right_operand);

		switch (p_operator) {
			case Variant::OP_ADD:
				result = ir.emit_addf64(left, right);
				break;
			case Variant::OP_SUBTRACT:
				result = ir.emit_subf64(left, right);
				break;
			case Variant::OP_MULTIPLY:
				result = ir.emit_mulf64(left, right);
				break;
			default:
				used_fast_path = false;
				break;
		}

		if (used_fast_path) {
			ir.emit_store_type(p_target, Variant::FLOAT);
			ir.emit_storef64(p_target, result);

			append_opcode(GDScriptFunction::OPCODE_OPERATOR_VALIDATED);
			append(p_left_operand);
			append(p_right_operand);
			append(p_target);
			append(Variant::get_validated_operator_evaluator(p_operator, Variant::FLOAT, Variant::FLOAT));
			return;
		}
	}

	if (valid &&
			p_left_operand.type.builtin_type == Variant::INT &&
			p_right_operand.type.builtin_type == Variant::INT &&
			p_target.type.kind == GDScriptDataType::BUILTIN &&
			p_target.type.builtin_type == Variant::BOOL) {
		bool used_fast_path = true;
		ValueId result{};
		const ValueId left = ir.emit_load(p_left_operand);
		const ValueId right = ir.emit_load(p_right_operand);

		switch (p_operator) {
			case Variant::OP_GREATER:
				result = ir.emit_gt64(left, right);
				break;
			case Variant::OP_GREATER_EQUAL:
				result = ir.emit_ge64(left, right);
				break;
			case Variant::OP_EQUAL:
				result = ir.emit_eq64(left, right);
				break;
			case Variant::OP_LESS:
				result = ir.emit_lt64(left, right);
				break;
			case Variant::OP_LESS_EQUAL:
				result = ir.emit_le64(left, right);
				break;
			case Variant::OP_NOT_EQUAL:
				result = ir.emit_ne64(left, right);
				break;
			default:
				used_fast_path = false;
				break;
		}

		if (used_fast_path) {
			ir.emit_store_type(p_target, Variant::BOOL);
			ir.emit_store(p_target, result);

			append_opcode(GDScriptFunction::OPCODE_OPERATOR_VALIDATED);
			append(p_left_operand);
			append(p_right_operand);
			append(p_target);
			append(Variant::get_validated_operator_evaluator(p_operator, Variant::INT, Variant::INT));
			return;
		}
	}

	if (valid &&
			p_left_operand.type.builtin_type == Variant::FLOAT &&
			p_right_operand.type.builtin_type == Variant::FLOAT &&
			p_target.type.kind == GDScriptDataType::BUILTIN &&
			p_target.type.builtin_type == Variant::BOOL) {
		bool used_fast_path = true;
		ValueId result{};
		const ValueId left = ir.emit_loadf64(p_left_operand);
		const ValueId right = ir.emit_loadf64(p_right_operand);

		switch (p_operator) {
			case Variant::OP_GREATER:
				result = ir.emit_gtf64(left, right);
				break;
			case Variant::OP_GREATER_EQUAL:
				result = ir.emit_gef64(left, right);
				break;
			case Variant::OP_EQUAL:
				result = ir.emit_eqf64(left, right);
				break;
			case Variant::OP_LESS:
				result = ir.emit_ltf64(left, right);
				break;
			case Variant::OP_LESS_EQUAL:
				result = ir.emit_lef64(left, right);
				break;
			case Variant::OP_NOT_EQUAL:
				result = ir.emit_nef64(left, right);
				break;
			default:
				used_fast_path = false;
				break;
		}

		if (used_fast_path) {
			ir.emit_store_type(p_target, Variant::BOOL);
			ir.emit_store(p_target, result);

			append_opcode(GDScriptFunction::OPCODE_OPERATOR_VALIDATED);
			append(p_left_operand);
			append(p_right_operand);
			append(p_target);
			append(Variant::get_validated_operator_evaluator(p_operator, Variant::FLOAT, Variant::FLOAT));
			return;
		}
	}

	// Avoid validated evaluator for modulo and division when operands are int or integer vector, since there's no check for division by zero.
	if (valid && (p_operator == Variant::OP_DIVIDE || p_operator == Variant::OP_MODULE)) {
		switch (p_left_operand.type.builtin_type) {
			case Variant::INT:
				// Cannot use modulo between int / float, we should raise an error later in GDScript
				valid = p_right_operand.type.builtin_type != Variant::INT && p_operator == Variant::OP_DIVIDE;
				break;
			case Variant::VECTOR2I:
			case Variant::VECTOR3I:
			case Variant::VECTOR4I:
				valid = p_right_operand.type.builtin_type != Variant::INT && p_right_operand.type.builtin_type != p_left_operand.type.builtin_type;
				break;
			default:
				break;
		}
	}

	if (valid) {
		if (p_target.mode == Address::TEMPORARY) {
			Variant::Type result_type = Variant::get_operator_return_type(p_operator, p_left_operand.type.builtin_type, p_right_operand.type.builtin_type);
			Variant::Type temp_type = temporaries[p_target.address].type;
			if (result_type != temp_type) {
				write_type_adjust(p_target, result_type);
			}
		}
		// Gather specific operator.
		Variant::ValidatedOperatorEvaluator op_func = Variant::get_validated_operator_evaluator(p_operator, p_left_operand.type.builtin_type, p_right_operand.type.builtin_type);
		const int operator_pos = get_operation_pos(op_func);
		const auto v1 = ir.emit_load_ptr(p_left_operand);
		const auto v2 = ir.emit_load_ptr(p_right_operand);
		const auto v3 = ir.emit_load_ptr(p_target);
		ir.emit_call_binop({v1, v2, v3}, op_func);

		append_opcode(GDScriptFunction::OPCODE_OPERATOR_VALIDATED);
		append(p_left_operand);
		append(p_right_operand);
		append(p_target);
		append(op_func);
#ifdef DEBUG_ENABLED
		add_debug_name(operator_names, operator_pos, Variant::get_operator_name(p_operator));
#endif
		return;
	}

	const auto v1 = ir.emit_load_ptr(p_left_operand);
	const auto v2 = ir.emit_load_ptr(p_right_operand);
	const auto v3 = ir.emit_load_ptr(p_target);
	ir.emit_call_operator(v1, v2, v3, p_operator);

	// No specific types, perform variant evaluation.
	append_opcode(GDScriptFunction::OPCODE_OPERATOR);
	append(p_left_operand);
	append(p_right_operand);
	append(p_target);
	append(p_operator);
	append(0); // Signature storage.
	append(0); // Return type storage.
	constexpr int _pointer_size = sizeof(Variant::ValidatedOperatorEvaluator) / sizeof(*(opcodes.ptr()));
	for (int i = 0; i < _pointer_size; i++) {
		append(0); // Space for function pointer.
	}
}

void GDScriptJitCodeGenerator::write_type_test(const Address &p_target, const Address &p_source, const GDScriptDataType &p_type) {
	print_debug("write_type_test");
	switch (p_type.kind) {
		case GDScriptDataType::BUILTIN: {
			if (p_type.builtin_type == Variant::ARRAY && p_type.has_container_element_type(0)) {
				const GDScriptDataType &element_type = p_type.get_container_element_type(0);
				append_opcode(GDScriptFunction::OPCODE_TYPE_TEST_ARRAY);
				append(p_target);
				append(p_source);
				append(get_constant_pos(element_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
				append(element_type.builtin_type);
				append(element_type.native_type);
			} else if (p_type.builtin_type == Variant::DICTIONARY && p_type.has_container_element_types()) {
				const GDScriptDataType &key_element_type = p_type.get_container_element_type_or_variant(0);
				const GDScriptDataType &value_element_type = p_type.get_container_element_type_or_variant(1);
				append_opcode(GDScriptFunction::OPCODE_TYPE_TEST_DICTIONARY);
				append(p_target);
				append(p_source);
				append(get_constant_pos(key_element_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
				append(get_constant_pos(value_element_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
				append(key_element_type.builtin_type);
				append(key_element_type.native_type);
				append(value_element_type.builtin_type);
				append(value_element_type.native_type);
			} else {
				append_opcode(GDScriptFunction::OPCODE_TYPE_TEST_BUILTIN);
				append(p_target);
				append(p_source);
				append(p_type.builtin_type);
			}
		} break;
		case GDScriptDataType::NATIVE: {
			append_opcode(GDScriptFunction::OPCODE_TYPE_TEST_NATIVE);
			append(p_target);
			append(p_source);
			append(p_type.native_type);
		} break;
		case GDScriptDataType::SCRIPT:
		case GDScriptDataType::GDSCRIPT: {
			const Variant &script = p_type.script_type;
			append_opcode(GDScriptFunction::OPCODE_TYPE_TEST_SCRIPT);
			append(p_target);
			append(p_source);
			append(get_constant_pos(script) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
		} break;
		default: {
			ERR_PRINT("Compiler bug: unresolved type in type test.");
			append_opcode(GDScriptFunction::OPCODE_ASSIGN_FALSE);
			append(p_target);
		}
	}
}

void GDScriptJitCodeGenerator::write_and_left_operand(const Address &p_left_operand) {
	print_debug("write_and_left_operand");
	append_opcode(GDScriptFunction::OPCODE_JUMP_IF_NOT);
	append(p_left_operand);
	logic_op_jump_pos1.push_back(opcodes.size());
	append(0); // Jump target, will be patched.
}

void GDScriptJitCodeGenerator::write_and_right_operand(const Address &p_right_operand) {
	print_debug("write_and_right_operand");
	append_opcode(GDScriptFunction::OPCODE_JUMP_IF_NOT);
	append(p_right_operand);
	logic_op_jump_pos2.push_back(opcodes.size());
	append(0); // Jump target, will be patched.
}

void GDScriptJitCodeGenerator::write_end_and(const Address &p_target) {
	print_debug("write_end_and");
	// If here means both operands are true.
	append_opcode(GDScriptFunction::OPCODE_ASSIGN_TRUE);
	append(p_target);
	// Jump away from the fail condition.
	append_opcode(GDScriptFunction::OPCODE_JUMP);
	append(opcodes.size() + 3);
	// Here it means one of operands is false.
	patch_jump(logic_op_jump_pos1.back()->get());
	patch_jump(logic_op_jump_pos2.back()->get());
	logic_op_jump_pos1.pop_back();
	logic_op_jump_pos2.pop_back();
	append_opcode(GDScriptFunction::OPCODE_ASSIGN_FALSE);
	append(p_target);
}

void GDScriptJitCodeGenerator::write_or_left_operand(const Address &p_left_operand) {
	print_debug("write_or_left_operand");
	append_opcode(GDScriptFunction::OPCODE_JUMP_IF);
	append(p_left_operand);
	logic_op_jump_pos1.push_back(opcodes.size());
	append(0); // Jump target, will be patched.
}

void GDScriptJitCodeGenerator::write_or_right_operand(const Address &p_right_operand) {
	print_debug("write_or_right_operand");
	append_opcode(GDScriptFunction::OPCODE_JUMP_IF);
	append(p_right_operand);
	logic_op_jump_pos2.push_back(opcodes.size());
	append(0); // Jump target, will be patched.
}

void GDScriptJitCodeGenerator::write_end_or(const Address &p_target) {
	print_debug("write_end_or");
	// If here means both operands are false.
	append_opcode(GDScriptFunction::OPCODE_ASSIGN_FALSE);
	append(p_target);
	// Jump away from the success condition.
	append_opcode(GDScriptFunction::OPCODE_JUMP);
	append(opcodes.size() + 3);
	// Here it means one of operands is true.
	patch_jump(logic_op_jump_pos1.back()->get());
	patch_jump(logic_op_jump_pos2.back()->get());
	logic_op_jump_pos1.pop_back();
	logic_op_jump_pos2.pop_back();
	append_opcode(GDScriptFunction::OPCODE_ASSIGN_TRUE);
	append(p_target);
}

void GDScriptJitCodeGenerator::write_start_ternary(const Address &p_target) {
	print_debug("write_start_ternary");
	ternary_result.push_back(p_target);
}

void GDScriptJitCodeGenerator::write_ternary_condition(const Address &p_condition) {
	print_debug("write_ternary_condition");
	append_opcode(GDScriptFunction::OPCODE_JUMP_IF_NOT);
	append(p_condition);
	ternary_jump_fail_pos.push_back(opcodes.size());
	append(0); // Jump target, will be patched.
}

void GDScriptJitCodeGenerator::write_ternary_true_expr(const Address &p_expr) {
	print_debug("write_ternary_true_expr");
	append_opcode(GDScriptFunction::OPCODE_ASSIGN);
	append(ternary_result.back()->get());
	append(p_expr);
	// Jump away from the false path.
	append_opcode(GDScriptFunction::OPCODE_JUMP);
	ternary_jump_skip_pos.push_back(opcodes.size());
	append(0);
	// Fail must jump here.
	patch_jump(ternary_jump_fail_pos.back()->get());
	ternary_jump_fail_pos.pop_back();
}

void GDScriptJitCodeGenerator::write_ternary_false_expr(const Address &p_expr) {
	print_debug("write_ternary_false_expr");
	append_opcode(GDScriptFunction::OPCODE_ASSIGN);
	append(ternary_result.back()->get());
	append(p_expr);
}

void GDScriptJitCodeGenerator::write_end_ternary() {
	print_debug("write_end_ternary");
	patch_jump(ternary_jump_skip_pos.back()->get());
	ternary_jump_skip_pos.pop_back();
	ternary_result.pop_back();
}

void GDScriptJitCodeGenerator::write_set(const Address &p_target, const Address &p_index, const Address &p_source) {
	print_debug("write_set");
	if (HAS_BUILTIN_TYPE(p_target)) {
		if (IS_BUILTIN_TYPE(p_index, Variant::INT) && Variant::get_member_validated_indexed_setter(p_target.type.builtin_type) &&
				IS_BUILTIN_TYPE(p_source, Variant::get_indexed_element_type(p_target.type.builtin_type))) {
			// Use indexed setter instead.
			Variant::ValidatedIndexedSetter setter = Variant::get_member_validated_indexed_setter(p_target.type.builtin_type);
			const ValueId target_ptr = ir.emit_load_ptr(p_target);
			const ValueId index_ptr = ir.emit_load(p_index);
			const ValueId source_ptr = ir.emit_load_ptr(p_source);
			ir.emit_set_indexed_validated(target_ptr, index_ptr, source_ptr, setter);
			append_opcode(GDScriptFunction::OPCODE_SET_INDEXED_VALIDATED);
			append(p_target);
			append(p_index);
			append(p_source);
			append(setter);
			return;
		} else if (Variant::get_member_validated_keyed_setter(p_target.type.builtin_type)) {
			Variant::ValidatedKeyedSetter setter = Variant::get_member_validated_keyed_setter(p_target.type.builtin_type);
			const ValueId target_ptr = ir.emit_load_ptr(p_target);
			const ValueId index_ptr = ir.emit_load_ptr(p_index);
			const ValueId source_ptr = ir.emit_load_ptr(p_source);
			ir.emit_set_keyed_validated(target_ptr, index_ptr, source_ptr, setter);
			append_opcode(GDScriptFunction::OPCODE_SET_KEYED_VALIDATED);
			append(p_target);
			append(p_index);
			append(p_source);
			append(setter);
			return;
		}
	}

	const ValueId target_ptr = ir.emit_load_ptr(p_target);
	const ValueId index_ptr = ir.emit_load_ptr(p_index);
	const ValueId source_ptr = ir.emit_load_ptr(p_source);
	ir.emit_set_keyed(target_ptr, index_ptr, source_ptr);
	append_opcode(GDScriptFunction::OPCODE_SET_KEYED);
	append(p_target);
	append(p_index);
	append(p_source);
}

void GDScriptJitCodeGenerator::write_get(const Address &p_target, const Address &p_index, const Address &p_source) {
	print_debug("write_get");
	if (HAS_BUILTIN_TYPE(p_source)) {
		if (IS_BUILTIN_TYPE(p_index, Variant::INT) && Variant::get_member_validated_indexed_getter(p_source.type.builtin_type)) {
			// Use indexed getter instead.
			Variant::ValidatedIndexedGetter getter = Variant::get_member_validated_indexed_getter(p_source.type.builtin_type);
			const ValueId source_ptr = ir.emit_load_ptr(p_source);
			const ValueId index_ptr = ir.emit_load(p_index);
			const ValueId target_ptr = ir.emit_load_ptr(p_target);
			ir.emit_get_indexed_validated(source_ptr, index_ptr, target_ptr, getter);
			append_opcode(GDScriptFunction::OPCODE_GET_INDEXED_VALIDATED);
			append(p_source);
			append(p_index);
			append(p_target);
			append(getter);
			return;
		} else if (Variant::get_member_validated_keyed_getter(p_source.type.builtin_type)) {
			Variant::ValidatedKeyedGetter getter = Variant::get_member_validated_keyed_getter(p_source.type.builtin_type);
			const ValueId source_ptr = ir.emit_load_ptr(p_source);
			const ValueId index_ptr = ir.emit_load_ptr(p_index);
			const ValueId target_ptr = ir.emit_load_ptr(p_target);
			ir.emit_get_keyed_validated(source_ptr, index_ptr, target_ptr, getter);
			append_opcode(GDScriptFunction::OPCODE_GET_KEYED_VALIDATED);
			append(p_source);
			append(p_index);
			append(p_target);
			append(getter);
			return;
		}
	}
	const ValueId source_ptr = ir.emit_load_ptr(p_source);
	const ValueId index_ptr = ir.emit_load_ptr(p_index);
	const ValueId target_ptr = ir.emit_load_ptr(p_target);
	ir.emit_get_keyed(source_ptr, index_ptr, target_ptr);
	append_opcode(GDScriptFunction::OPCODE_GET_KEYED);
	append(p_source);
	append(p_index);
	append(p_target);
}

void GDScriptJitCodeGenerator::write_set_named(const Address &p_target, const StringName &p_name, const Address &p_source) {
	print_debug("write_set_named");
	if (HAS_BUILTIN_TYPE(p_target) && Variant::get_member_validated_setter(p_target.type.builtin_type, p_name) &&
			IS_BUILTIN_TYPE(p_source, Variant::get_member_type(p_target.type.builtin_type, p_name))) {
		Variant::ValidatedSetter setter = Variant::get_member_validated_setter(p_target.type.builtin_type, p_name);
		const bool float_source = p_source.type.kind == GDScriptDataType::BUILTIN && p_source.type.builtin_type == Variant::FLOAT;
		int component_index = -1;
		if (p_name == "x") {
			component_index = 0;
		} else if (p_name == "y") {
			component_index = 1;
		} else if (p_name == "z") {
			component_index = 2;
		} else if (p_name == "w") {
			component_index = 3;
		}
		int component_count = 0;
		switch (p_target.type.builtin_type) {
			case Variant::VECTOR2:
				component_count = 2;
				break;
			case Variant::VECTOR3:
				component_count = 3;
				break;
			case Variant::VECTOR4:
				component_count = 4;
				break;
			default:
				break;
		}
		if (float_source && component_index >= 0 && component_index < component_count) {
			const ValueId value = ir.emit_loadf64(p_source);
			ir.emit_store_real_member_f64(p_target, uint32_t(component_index * sizeof(real_t)), value);
			append_opcode(GDScriptFunction::OPCODE_SET_NAMED_VALIDATED);
			append(p_target);
			append(p_source);
			append(setter);
#ifdef DEBUG_ENABLED
			add_debug_name(setter_names, get_setter_pos(setter), p_name);
#endif
			return;
		}
		const ValueId target_ptr = ir.emit_load_ptr(p_target);
		const ValueId source_ptr = ir.emit_load_ptr(p_source);
		ir.emit_set_named_validated(target_ptr, source_ptr, setter);
		append_opcode(GDScriptFunction::OPCODE_SET_NAMED_VALIDATED);
		append(p_target);
		append(p_source);
		append(setter);
#ifdef DEBUG_ENABLED
		add_debug_name(setter_names, get_setter_pos(setter), p_name);
#endif
		return;
	}
	const ValueId target_ptr = ir.emit_load_ptr(p_target);
	const ValueId source_ptr = ir.emit_load_ptr(p_source);
	ir.emit_set_named(target_ptr, source_ptr, get_name_map_pos(p_name));
	append_opcode(GDScriptFunction::OPCODE_SET_NAMED);
	append(p_target);
	append(p_source);
	append(p_name);
}

void GDScriptJitCodeGenerator::write_get_named(const Address &p_target, const StringName &p_name, const Address &p_source) {
	print_debug("write_get_named");
	if (HAS_BUILTIN_TYPE(p_source) && Variant::get_member_validated_getter(p_source.type.builtin_type, p_name)) {
		Variant::ValidatedGetter getter = Variant::get_member_validated_getter(p_source.type.builtin_type, p_name);
		const bool float_target = p_target.type.kind == GDScriptDataType::BUILTIN && p_target.type.builtin_type == Variant::FLOAT;
		int component_index = -1;
		if (p_name == "x") {
			component_index = 0;
		} else if (p_name == "y") {
			component_index = 1;
		} else if (p_name == "z") {
			component_index = 2;
		} else if (p_name == "w") {
			component_index = 3;
		}
		int component_count = 0;
		switch (p_source.type.builtin_type) {
			case Variant::VECTOR2:
				component_count = 2;
				break;
			case Variant::VECTOR3:
				component_count = 3;
				break;
			case Variant::VECTOR4:
				component_count = 4;
				break;
			default:
				break;
		}
		if (float_target && component_index >= 0 && component_index < component_count) {
			ir.emit_store_type(p_target, Variant::FLOAT);
			const ValueId value = ir.emit_load_real_member_f64(p_source, uint32_t(component_index * sizeof(real_t)));
			ir.emit_storef64(p_target, value);
			append_opcode(GDScriptFunction::OPCODE_GET_NAMED_VALIDATED);
			append(p_source);
			append(p_target);
			append(getter);
#ifdef DEBUG_ENABLED
			add_debug_name(getter_names, get_getter_pos(getter), p_name);
#endif
			return;
		}
		const ValueId source_ptr = ir.emit_load_ptr(p_source);
		const ValueId target_ptr = ir.emit_load_ptr(p_target);
		ir.emit_get_named_validated(source_ptr, target_ptr, getter);
		append_opcode(GDScriptFunction::OPCODE_GET_NAMED_VALIDATED);
		append(p_source);
		append(p_target);
		append(getter);
#ifdef DEBUG_ENABLED
		add_debug_name(getter_names, get_getter_pos(getter), p_name);
#endif
		return;
	}
	const ValueId source_ptr = ir.emit_load_ptr(p_source);
	const ValueId target_ptr = ir.emit_load_ptr(p_target);
	ir.emit_get_named(source_ptr, target_ptr, get_name_map_pos(p_name));
	append_opcode(GDScriptFunction::OPCODE_GET_NAMED);
	append(p_source);
	append(p_target);
	append(p_name);
}

void GDScriptJitCodeGenerator::write_set_member(const Address &p_value, const StringName &p_name) {
	print_debug("write_set_member");
	const ValueId value_ptr = ir.emit_load_ptr(p_value);
	ir.emit_set_member(value_ptr, get_name_map_pos(p_name));
	append_opcode(GDScriptFunction::OPCODE_SET_MEMBER);
	append(p_value);
	append(p_name);
}

void GDScriptJitCodeGenerator::write_get_member(const Address &p_target, const StringName &p_name) {
	print_debug("write_get_member");
	const ValueId target_ptr = ir.emit_load_ptr(p_target);
	ir.emit_get_member(target_ptr, get_name_map_pos(p_name));
	append_opcode(GDScriptFunction::OPCODE_GET_MEMBER);
	append(p_target);
	append(p_name);
}

void GDScriptJitCodeGenerator::write_set_static_variable(const Address &p_value, const Address &p_class, int p_index) {
	print_debug("write_set_static_variable");
	append_opcode(GDScriptFunction::OPCODE_SET_STATIC_VARIABLE);
	append(p_value);
	append(p_class);
	append(p_index);
}

void GDScriptJitCodeGenerator::write_get_static_variable(const Address &p_target, const Address &p_class, int p_index) {
	print_debug("write_get_static_variable");
	append_opcode(GDScriptFunction::OPCODE_GET_STATIC_VARIABLE);
	append(p_target);
	append(p_class);
	append(p_index);
}

void GDScriptJitCodeGenerator::write_assign_with_conversion(const Address &p_target, const Address &p_source) {
	print_debug("write_assign_with_conversion");
	switch (p_target.type.kind) {
		case GDScriptDataType::BUILTIN: {
			if (p_target.type.builtin_type == Variant::ARRAY && p_target.type.has_container_element_type(0)) {
				const ValueId target_ptr = ir.emit_load_ptr(p_target);
				const ValueId source_ptr = ir.emit_load_ptr(p_source);
				const GDScriptDataType &element_type = p_target.type.get_container_element_type(0);
				const Address script_type_addr(GDScriptCodeGenerator::Address::CONSTANT, get_constant_pos(element_type.script_type), GDScriptDataType());
				const ValueId script_type_ptr = ir.emit_load_ptr(script_type_addr);
				ir.emit_assign_typed_array(target_ptr, source_ptr, script_type_ptr, element_type.builtin_type, get_name_map_pos(element_type.native_type));
				append_opcode(GDScriptFunction::OPCODE_ASSIGN_TYPED_ARRAY);
				append(p_target);
				append(p_source);
				append(get_constant_pos(element_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
				append(element_type.builtin_type);
				append(element_type.native_type);
			} else if (p_target.type.builtin_type == Variant::DICTIONARY && p_target.type.has_container_element_types()) {
				const ValueId target_ptr = ir.emit_load_ptr(p_target);
				const ValueId source_ptr = ir.emit_load_ptr(p_source);
				const GDScriptDataType &key_type = p_target.type.get_container_element_type_or_variant(0);
				const GDScriptDataType &value_type = p_target.type.get_container_element_type_or_variant(1);
				const Address key_script_type_addr(GDScriptCodeGenerator::Address::CONSTANT, get_constant_pos(key_type.script_type), GDScriptDataType());
				const Address value_script_type_addr(GDScriptCodeGenerator::Address::CONSTANT, get_constant_pos(value_type.script_type), GDScriptDataType());
				const ValueId key_script_type_ptr = ir.emit_load_ptr(key_script_type_addr);
				const ValueId value_script_type_ptr = ir.emit_load_ptr(value_script_type_addr);
				ir.emit_assign_typed_dictionary(target_ptr, source_ptr, key_script_type_ptr, value_script_type_ptr, key_type.builtin_type, get_name_map_pos(key_type.native_type), value_type.builtin_type, get_name_map_pos(value_type.native_type));
				append_opcode(GDScriptFunction::OPCODE_ASSIGN_TYPED_DICTIONARY);
				append(p_target);
				append(p_source);
				append(get_constant_pos(key_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
				append(get_constant_pos(value_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
				append(key_type.builtin_type);
				append(key_type.native_type);
				append(value_type.builtin_type);
				append(value_type.native_type);
			} else {
				const ValueId target_ptr = ir.emit_load_ptr(p_target);
				const ValueId source_ptr = ir.emit_load_ptr(p_source);
				ir.emit_assign_typed_builtin(target_ptr, source_ptr, p_target.type.builtin_type);
				append_opcode(GDScriptFunction::OPCODE_ASSIGN_TYPED_BUILTIN);
				append(p_target);
				append(p_source);
				append(p_target.type.builtin_type);
			}
		} break;
		case GDScriptDataType::NATIVE: {
			int class_idx = GDScriptLanguage::get_singleton()->get_global_map()[p_target.type.native_type];
			Variant nc = GDScriptLanguage::get_singleton()->get_global_array()[class_idx];
			class_idx = get_constant_pos(nc) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS);
			const ValueId target_ptr = ir.emit_load_ptr(p_target);
			const ValueId source_ptr = ir.emit_load_ptr(p_source);
			const Address type_addr(GDScriptCodeGenerator::Address::CONSTANT, class_idx & GDScriptFunction::ADDR_MASK, GDScriptDataType());
			const ValueId type_ptr = ir.emit_load_ptr(type_addr);
			ir.emit_assign_typed_native(target_ptr, source_ptr, type_ptr);
			append_opcode(GDScriptFunction::OPCODE_ASSIGN_TYPED_NATIVE);
			append(p_target);
			append(p_source);
			append(class_idx);
		} break;
		case GDScriptDataType::SCRIPT:
		case GDScriptDataType::GDSCRIPT: {
			Variant script = p_target.type.script_type;
			int idx = get_constant_pos(script) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS);
			const ValueId target_ptr = ir.emit_load_ptr(p_target);
			const ValueId source_ptr = ir.emit_load_ptr(p_source);
			const Address type_addr(GDScriptCodeGenerator::Address::CONSTANT, idx & GDScriptFunction::ADDR_MASK, GDScriptDataType());
			const ValueId type_ptr = ir.emit_load_ptr(type_addr);
			ir.emit_assign_typed_script(target_ptr, source_ptr, type_ptr);

			append_opcode(GDScriptFunction::OPCODE_ASSIGN_TYPED_SCRIPT);
			append(p_target);
			append(p_source);
			append(idx);
		} break;
		default: {
			ERR_PRINT("Compiler bug: unresolved assign.");

			// Shouldn't get here, but fail-safe to a regular assignment
			append_opcode(GDScriptFunction::OPCODE_ASSIGN);
			append(p_target);
			append(p_source);
		}
	}
}

void GDScriptJitCodeGenerator::write_assign(const Address &p_target, const Address &p_source) {
	print_debug("write_assign");
	if (p_target.type.kind == GDScriptDataType::BUILTIN && p_target.type.builtin_type == Variant::ARRAY && p_target.type.has_container_element_type(0)) {
		const ValueId target_ptr = ir.emit_load_ptr(p_target);
		const ValueId source_ptr = ir.emit_load_ptr(p_source);
		const GDScriptDataType &element_type = p_target.type.get_container_element_type(0);
		const Address script_type_addr(GDScriptCodeGenerator::Address::CONSTANT, get_constant_pos(element_type.script_type), GDScriptDataType());
		const ValueId script_type_ptr = ir.emit_load_ptr(script_type_addr);
		ir.emit_assign_typed_array(target_ptr, source_ptr, script_type_ptr, element_type.builtin_type, get_name_map_pos(element_type.native_type));
		append_opcode(GDScriptFunction::OPCODE_ASSIGN_TYPED_ARRAY);
		append(p_target);
		append(p_source);
		append(get_constant_pos(element_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
		append(element_type.builtin_type);
		append(element_type.native_type);
	} else if (p_target.type.kind == GDScriptDataType::BUILTIN && p_target.type.builtin_type == Variant::DICTIONARY && p_target.type.has_container_element_types()) {
		const ValueId target_ptr = ir.emit_load_ptr(p_target);
		const ValueId source_ptr = ir.emit_load_ptr(p_source);
		const GDScriptDataType &key_type = p_target.type.get_container_element_type_or_variant(0);
		const GDScriptDataType &value_type = p_target.type.get_container_element_type_or_variant(1);
		const Address key_script_type_addr(GDScriptCodeGenerator::Address::CONSTANT, get_constant_pos(key_type.script_type), GDScriptDataType());
		const Address value_script_type_addr(GDScriptCodeGenerator::Address::CONSTANT, get_constant_pos(value_type.script_type), GDScriptDataType());
		const ValueId key_script_type_ptr = ir.emit_load_ptr(key_script_type_addr);
		const ValueId value_script_type_ptr = ir.emit_load_ptr(value_script_type_addr);
		ir.emit_assign_typed_dictionary(target_ptr, source_ptr, key_script_type_ptr, value_script_type_ptr, key_type.builtin_type, get_name_map_pos(key_type.native_type), value_type.builtin_type, get_name_map_pos(value_type.native_type));
		append_opcode(GDScriptFunction::OPCODE_ASSIGN_TYPED_DICTIONARY);
		append(p_target);
		append(p_source);
		append(get_constant_pos(key_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
		append(get_constant_pos(value_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
		append(key_type.builtin_type);
		append(key_type.native_type);
		append(value_type.builtin_type);
		append(value_type.native_type);
	} else if (p_target.type.kind == GDScriptDataType::BUILTIN && p_source.type.kind == GDScriptDataType::BUILTIN && p_target.type.builtin_type != p_source.type.builtin_type) {
		// Need conversion.
		const ValueId target_ptr = ir.emit_load_ptr(p_target);
		const ValueId source_ptr = ir.emit_load_ptr(p_source);
		ir.emit_assign_typed_builtin(target_ptr, source_ptr, p_target.type.builtin_type);
		append_opcode(GDScriptFunction::OPCODE_ASSIGN_TYPED_BUILTIN);
		append(p_target);
		append(p_source);
		append(p_target.type.builtin_type);
	} else {
		emit_assign_ir_simple(p_target, p_source);

		append_opcode(GDScriptFunction::OPCODE_ASSIGN);
		append(p_target);
		append(p_source);
	}
}

void GDScriptJitCodeGenerator::write_assign_null(const Address &p_target) {
	print_debug("write_assign_null");
	const ValueId target_ptr = ir.emit_load_ptr(p_target);
	ir.emit_assign_null(target_ptr);
	append_opcode(GDScriptFunction::OPCODE_ASSIGN_NULL);
	append(p_target);
}

void GDScriptJitCodeGenerator::write_assign_true(const Address &p_target) {
	print_debug("write_assign_true");
	const ValueId target_ptr = ir.emit_load_ptr(p_target);
	ir.emit_assign_true(target_ptr);
	append_opcode(GDScriptFunction::OPCODE_ASSIGN_TRUE);
	append(p_target);
}

void GDScriptJitCodeGenerator::write_assign_false(const Address &p_target) {
	print_debug("write_assign_false");
	const ValueId target_ptr = ir.emit_load_ptr(p_target);
	ir.emit_assign_false(target_ptr);
	append_opcode(GDScriptFunction::OPCODE_ASSIGN_FALSE);
	append(p_target);
}

void GDScriptJitCodeGenerator::write_assign_default_parameter(const Address &p_dst, const Address &p_src, bool p_use_conversion) {
	print_debug("write_assign_default_parameter");
	if (p_use_conversion) {
		write_assign_with_conversion(p_dst, p_src);
	} else {
		write_assign(p_dst, p_src);
	}
	function->default_arguments.push_back(opcodes.size());
	if (default_parameters_active) {
		default_parameter_index++;
		if (default_parameter_index < function->_default_arg_count) {
			if (default_parameter_index - 1 < default_parameter_labels.size()) {
				ir.bind_label(default_parameter_labels.write[default_parameter_index - 1]);
			}
		} else {
			ir.bind_label(default_parameter_body_label);
			default_parameters_active = false;
		}
	}
}

void GDScriptJitCodeGenerator::write_store_global(const Address &p_dst, int p_global_index) {
	print_debug("write_store_global");
	append_opcode(GDScriptFunction::OPCODE_STORE_GLOBAL);
	append(p_dst);
	append(p_global_index);
}

void GDScriptJitCodeGenerator::write_store_named_global(const Address &p_dst, const StringName &p_global) {
	print_debug("write_store_named_global");
	append_opcode(GDScriptFunction::OPCODE_STORE_NAMED_GLOBAL);
	append(p_dst);
	append(p_global);
}

void GDScriptJitCodeGenerator::write_cast(const Address &p_target, const Address &p_source, const GDScriptDataType &p_type) {
	print_debug("write_cast");
	int index = 0;

	switch (p_type.kind) {
		case GDScriptDataType::BUILTIN: {
			append_opcode(GDScriptFunction::OPCODE_CAST_TO_BUILTIN);
			index = p_type.builtin_type;
		} break;
		case GDScriptDataType::NATIVE: {
			int class_idx = GDScriptLanguage::get_singleton()->get_global_map()[p_type.native_type];
			Variant nc = GDScriptLanguage::get_singleton()->get_global_array()[class_idx];
			append_opcode(GDScriptFunction::OPCODE_CAST_TO_NATIVE);
			index = get_constant_pos(nc) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS);
		} break;
		case GDScriptDataType::SCRIPT:
		case GDScriptDataType::GDSCRIPT: {
			Variant script = p_type.script_type;
			int idx = get_constant_pos(script) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS);
			append_opcode(GDScriptFunction::OPCODE_CAST_TO_SCRIPT);
			index = idx;
		} break;
		default: {
			return;
		}
	}

	append(p_source);
	append(p_target);
	append(index);
}

GDScriptJitCodeGenerator::CallTarget GDScriptJitCodeGenerator::get_call_target(const GDScriptCodeGenerator::Address &p_target, Variant::Type p_type) {
	print_debug("CallTarget GDScriptJitCodeGenerator::get_call_target");
	if (p_target.mode == Address::NIL) {
		GDScriptDataType type;
		if (p_type != Variant::NIL) {
			type.kind = GDScriptDataType::BUILTIN;
			type.builtin_type = p_type;
		}
		uint32_t addr = add_temporary(type);
		return CallTarget(Address(Address::TEMPORARY, addr, type), true, this);
	} else {
		return CallTarget(p_target, false, this);
	}
}

void GDScriptJitCodeGenerator::write_call(const Address &p_target, const Address &p_base, const StringName &p_function_name, const Vector<Address> &p_arguments) {
	print_debug("write_call");
	Vector<ValueId> arg_ptrs;
	arg_ptrs.resize(p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		arg_ptrs.write[i] = ir.emit_load_ptr(p_arguments[i]);
	}
	const ValueId base_ptr = ir.emit_load_ptr(p_base);
	CallTarget ct_ir = get_call_target(p_target);
	const ValueId dst_ptr = ir.emit_load_ptr(ct_ir.target);
	ir.emit_call(base_ptr, arg_ptrs, dst_ptr, get_name_map_pos(p_function_name));
	ct_ir.cleanup();

	append_opcode_and_argcount(p_target.mode == Address::NIL ? GDScriptFunction::OPCODE_CALL : GDScriptFunction::OPCODE_CALL_RETURN, 2 + p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	append(p_base);
	CallTarget ct = get_call_target(p_target);
	append(ct.target);
	append(p_arguments.size());
	append(p_function_name);
	ct.cleanup();
}

void GDScriptJitCodeGenerator::write_super_call(const Address &p_target, const StringName &p_function_name, const Vector<Address> &p_arguments) {
	print_debug("write_super_call");
	append_opcode_and_argcount(GDScriptFunction::OPCODE_CALL_SELF_BASE, 1 + p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	CallTarget ct = get_call_target(p_target);
	append(ct.target);
	append(p_arguments.size());
	append(p_function_name);
	ct.cleanup();
}

void GDScriptJitCodeGenerator::write_call_async(const Address &p_target, const Address &p_base, const StringName &p_function_name, const Vector<Address> &p_arguments) {
	print_debug("write_call_async");
	append_opcode_and_argcount(GDScriptFunction::OPCODE_CALL_ASYNC, 2 + p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	append(p_base);
	CallTarget ct = get_call_target(p_target);
	append(ct.target);
	append(p_arguments.size());
	append(p_function_name);
	ct.cleanup();
}

void GDScriptJitCodeGenerator::write_call_gdscript_utility(const Address &p_target, const StringName &p_function, const Vector<Address> &p_arguments) {
	print_debug("write_call_gdscript_utility");
	Vector<ValueId> arg_ptrs;
	for (int i = 0; i < p_arguments.size(); i++) {
		arg_ptrs.push_back(ir.emit_load_ptr(p_arguments[i]));
	}
	GDScriptUtilityFunctions::FunctionPtr gds_function = GDScriptUtilityFunctions::get_function(p_function);
	CallTarget ct = get_call_target(p_target);
	const ValueId target_ptr = ir.emit_load_ptr(ct.target);
	ir.emit_call_gdscript_utility(arg_ptrs, target_ptr, get_gds_utility_pos(gds_function));
	append_opcode_and_argcount(GDScriptFunction::OPCODE_CALL_GDSCRIPT_UTILITY, 1 + p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	append(ct.target);
	append(p_arguments.size());
	append(gds_function);
	ct.cleanup();
#ifdef DEBUG_ENABLED
	add_debug_name(gds_utilities_names, get_gds_utility_pos(gds_function), p_function);
#endif
}

void GDScriptJitCodeGenerator::write_call_utility(const Address &p_target, const StringName &p_function, const Vector<Address> &p_arguments) {
	print_debug("write_call_utility");
	bool is_validated = true;
	if (Variant::is_utility_function_vararg(p_function)) {
		is_validated = false; // Vararg needs runtime checks, can't use validated call.
	} else if (p_arguments.size() == Variant::get_utility_function_argument_count(p_function)) {
		bool all_types_exact = true;
		for (int i = 0; i < p_arguments.size(); i++) {
			if (!IS_BUILTIN_TYPE(p_arguments[i], Variant::get_utility_function_argument_type(p_function, i))) {
				all_types_exact = false;
				break;
			}
		}

		is_validated = all_types_exact;
	}

	if (is_validated) {
		Variant::Type result_type = Variant::has_utility_function_return_value(p_function) ? Variant::get_utility_function_return_type(p_function) : Variant::NIL;
		CallTarget ct = get_call_target(p_target, result_type);
		Variant::Type temp_type = temporaries[ct.target.address].type;
		if (result_type != temp_type) {
			write_type_adjust(ct.target, result_type);
		}
		Vector<ValueId> arg_ptrs;
		for (int i = 0; i < p_arguments.size(); i++) {
			arg_ptrs.push_back(ir.emit_load_ptr(p_arguments[i]));
		}
		const ValueId target_ptr = ir.emit_load_ptr(ct.target);
		ir.emit_call_utility_validated(arg_ptrs, target_ptr, Variant::get_validated_utility_function(p_function));
		append_opcode_and_argcount(GDScriptFunction::OPCODE_CALL_UTILITY_VALIDATED, 1 + p_arguments.size());
		for (int i = 0; i < p_arguments.size(); i++) {
			append(p_arguments[i]);
		}
		append(ct.target);
		append(p_arguments.size());
		append(Variant::get_validated_utility_function(p_function));
		ct.cleanup();
#ifdef DEBUG_ENABLED
		add_debug_name(utilities_names, get_utility_pos(Variant::get_validated_utility_function(p_function)), p_function);
#endif
	} else {
		Vector<ValueId> arg_ptrs;
		for (int i = 0; i < p_arguments.size(); i++) {
			arg_ptrs.push_back(ir.emit_load_ptr(p_arguments[i]));
		}
		CallTarget ct_ir = get_call_target(p_target);
		const ValueId target_ptr = ir.emit_load_ptr(ct_ir.target);
		ir.emit_call_utility(arg_ptrs, target_ptr, get_name_map_pos(p_function));
		ct_ir.cleanup();

		append_opcode_and_argcount(GDScriptFunction::OPCODE_CALL_UTILITY, 1 + p_arguments.size());
		for (int i = 0; i < p_arguments.size(); i++) {
			append(p_arguments[i]);
		}
		CallTarget ct = get_call_target(p_target);
		append(ct.target);
		append(p_arguments.size());
		append(p_function);
		ct.cleanup();
	}
}

void GDScriptJitCodeGenerator::write_call_builtin_type(const Address &p_target, const Address &p_base, Variant::Type p_type, const StringName &p_method, bool p_is_static, const Vector<Address> &p_arguments) {
	print_debug("write_call_builtin_type");
	bool is_validated = false;

	// Check if all types are correct.
	if (Variant::is_builtin_method_vararg(p_type, p_method)) {
		is_validated = false; // Vararg needs runtime checks, can't use validated call.
	} else if (p_arguments.size() == Variant::get_builtin_method_argument_count(p_type, p_method)) {
		bool all_types_exact = true;
		for (int i = 0; i < p_arguments.size(); i++) {
			if (!IS_BUILTIN_TYPE(p_arguments[i], Variant::get_builtin_method_argument_type(p_type, p_method, i))) {
				all_types_exact = false;
				break;
			}
		}

		is_validated = all_types_exact;
	}

	if (!is_validated) {
		// Perform regular call.
		if (p_is_static) {
			Vector<ValueId> arg_ptrs;
			for (int i = 0; i < p_arguments.size(); i++) {
				arg_ptrs.push_back(ir.emit_load_ptr(p_arguments[i]));
			}
			append_opcode_and_argcount(GDScriptFunction::OPCODE_CALL_BUILTIN_STATIC, p_arguments.size() + 1);
			for (int i = 0; i < p_arguments.size(); i++) {
				append(p_arguments[i]);
			}
			CallTarget ct = get_call_target(p_target);
			const ValueId target_ptr = ir.emit_load_ptr(ct.target);
			ir.emit_call_builtin_static(arg_ptrs, target_ptr, p_type, get_name_map_pos(p_method));
			append(ct.target);
			append(p_type);
			append(p_method);
			append(p_arguments.size());
			ct.cleanup();
		} else {
			write_call(p_target, p_base, p_method, p_arguments);
		}
		return;
	}

	Variant::Type result_type = Variant::get_builtin_method_return_type(p_type, p_method);
	CallTarget ct = get_call_target(p_target, result_type);
	Variant::Type temp_type = temporaries[ct.target.address].type;
	if (result_type != temp_type) {
		write_type_adjust(ct.target, result_type);
	}

	append_opcode_and_argcount(GDScriptFunction::OPCODE_CALL_BUILTIN_TYPE_VALIDATED, 2 + p_arguments.size());
	Vector<ValueId> arg_ptrs;
	for (int i = 0; i < p_arguments.size(); i++) {
		arg_ptrs.push_back(ir.emit_load_ptr(p_arguments[i]));
	}
	const ValueId base_ptr = ir.emit_load_ptr(p_base);
	const ValueId target_ptr = ir.emit_load_ptr(ct.target);
	ir.emit_call_builtin_validated(base_ptr, arg_ptrs, target_ptr, Variant::get_validated_builtin_method(p_type, p_method));

	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	append(p_base);
	append(ct.target);
	append(p_arguments.size());
	append(Variant::get_validated_builtin_method(p_type, p_method));
	ct.cleanup();

#ifdef DEBUG_ENABLED
	add_debug_name(builtin_methods_names, get_builtin_method_pos(Variant::get_validated_builtin_method(p_type, p_method)), p_method);
#endif
}

void GDScriptJitCodeGenerator::write_call_builtin_type(const Address &p_target, const Address &p_base, Variant::Type p_type, const StringName &p_method, const Vector<Address> &p_arguments) {
	print_debug("write_call_builtin_type");
	write_call_builtin_type(p_target, p_base, p_type, p_method, false, p_arguments);
}

void GDScriptJitCodeGenerator::write_call_builtin_type_static(const Address &p_target, Variant::Type p_type, const StringName &p_method, const Vector<Address> &p_arguments) {
	print_debug("write_call_builtin_type_static");
	write_call_builtin_type(p_target, Address(), p_type, p_method, true, p_arguments);
}

void GDScriptJitCodeGenerator::write_call_native_static(const Address &p_target, const StringName &p_class, const StringName &p_method, const Vector<Address> &p_arguments) {
	print_debug("write_call_native_static");
	MethodBind *method = ClassDB::get_method(p_class, p_method);

	// Perform regular call.
	append_opcode_and_argcount(GDScriptFunction::OPCODE_CALL_NATIVE_STATIC, p_arguments.size() + 1);
	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	CallTarget ct = get_call_target(p_target);
	append(ct.target);
	append(method);
	append(p_arguments.size());
	ct.cleanup();
	return;
}

void GDScriptJitCodeGenerator::write_call_native_static_validated(const GDScriptCodeGenerator::Address &p_target, MethodBind *p_method, const Vector<GDScriptCodeGenerator::Address> &p_arguments) {
	print_debug("write_call_native_static_validated");
	Variant::Type return_type = Variant::NIL;
	bool has_return = p_method->has_return();

	if (has_return) {
		PropertyInfo return_info = p_method->get_return_info();
		return_type = return_info.type;
	}

	CallTarget ct = get_call_target(p_target, return_type);

	if (has_return) {
		Variant::Type temp_type = temporaries[ct.target.address].type;
		if (temp_type != return_type) {
			write_type_adjust(ct.target, return_type);
		}
	}

	GDScriptFunction::Opcode code = p_method->has_return() ? GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN : GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_NO_RETURN;
	append_opcode_and_argcount(code, 1 + p_arguments.size());

	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	append(ct.target);
	append(p_arguments.size());
	append(p_method);
	ct.cleanup();
}

void GDScriptJitCodeGenerator::write_call_method_bind(const Address &p_target, const Address &p_base, MethodBind *p_method, const Vector<Address> &p_arguments) {
	print_debug("write_call_method_bind");
	CallTarget ct = get_call_target(p_target);

	Vector<ValueId> arg_ptrs;
	for (int i = 0; i < p_arguments.size(); i++) {
		arg_ptrs.push_back(ir.emit_load_ptr(p_arguments[i]));
	}
	const ValueId base_ptr = ir.emit_load_ptr(p_base);
	const ValueId target_ptr = ir.emit_load_ptr(ct.target);
	ir.emit_call_method_bind(base_ptr, arg_ptrs, target_ptr, p_method);

	append_opcode_and_argcount(p_target.mode == Address::NIL ? GDScriptFunction::OPCODE_CALL_METHOD_BIND : GDScriptFunction::OPCODE_CALL_METHOD_BIND_RET, 2 + p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	append(p_base);
	append(ct.target);
	append(p_arguments.size());
	append(p_method);
	ct.cleanup();
}

void GDScriptJitCodeGenerator::write_call_method_bind_validated(const Address &p_target, const Address &p_base, MethodBind *p_method, const Vector<Address> &p_arguments) {
	print_debug("write_call_method_bind_validated");
	Variant::Type return_type = Variant::NIL;
	bool has_return = p_method->has_return();

	if (has_return) {
		PropertyInfo return_info = p_method->get_return_info();
		return_type = return_info.type;
	}

	CallTarget ct = get_call_target(p_target, return_type);

	if (has_return) {
		Variant::Type temp_type = temporaries[ct.target.address].type;
		if (temp_type != return_type) {
			write_type_adjust(ct.target, return_type);
		}
	}

	GDScriptFunction::Opcode code = p_method->has_return() ? GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN : GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_NO_RETURN;
	append_opcode_and_argcount(code, 2 + p_arguments.size());
	if (p_method->has_return()) {
		Vector<ValueId> arg_ptrs;
		for (int i = 0; i < p_arguments.size(); i++) {
			arg_ptrs.push_back(ir.emit_load_ptr(p_arguments[i]));
		}
		const ValueId base_ptr = ir.emit_load_ptr(p_base);
		const ValueId target_ptr = ir.emit_load_ptr(ct.target);
		ir.emit_call_method_bind_validated(base_ptr, arg_ptrs, target_ptr, p_method);
	}

	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	append(p_base);
	append(ct.target);
	append(p_arguments.size());
	append(p_method);
	ct.cleanup();
}

void GDScriptJitCodeGenerator::write_call_self(const Address &p_target, const StringName &p_function_name, const Vector<Address> &p_arguments) {
	print_debug("write_call_self");
	Vector<ValueId> arg_ptrs;
	arg_ptrs.resize(p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		arg_ptrs.write[i] = ir.emit_load_ptr(p_arguments[i]);
	}
	CallTarget ct_ir = get_call_target(p_target);
	const ValueId dst_ptr = ir.emit_load_ptr(ct_ir.target);
	ir.emit_call_self(arg_ptrs, dst_ptr, get_name_map_pos(p_function_name));
	ct_ir.cleanup();

	append_opcode_and_argcount(p_target.mode == Address::NIL ? GDScriptFunction::OPCODE_CALL : GDScriptFunction::OPCODE_CALL_RETURN, 2 + p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	append(GDScriptFunction::ADDR_TYPE_STACK << GDScriptFunction::ADDR_BITS);
	CallTarget ct = get_call_target(p_target);
	append(ct.target);
	append(p_arguments.size());
	append(p_function_name);
	ct.cleanup();
}

void GDScriptJitCodeGenerator::write_call_self_async(const Address &p_target, const StringName &p_function_name, const Vector<Address> &p_arguments) {
	print_debug("write_call_self_async");
	append_opcode_and_argcount(GDScriptFunction::OPCODE_CALL_ASYNC, 2 + p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	append(GDScriptFunction::ADDR_SELF);
	CallTarget ct = get_call_target(p_target);
	append(ct.target);
	append(p_arguments.size());
	append(p_function_name);
	ct.cleanup();
}

void GDScriptJitCodeGenerator::write_call_script_function(const Address &p_target, const Address &p_base, const StringName &p_function_name, const Vector<Address> &p_arguments) {
	print_debug("write_call_script_function");
	append_opcode_and_argcount(p_target.mode == Address::NIL ? GDScriptFunction::OPCODE_CALL : GDScriptFunction::OPCODE_CALL_RETURN, 2 + p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	append(p_base);
	CallTarget ct = get_call_target(p_target);
	append(ct.target);
	append(p_arguments.size());
	append(p_function_name);
	ct.cleanup();
}

void GDScriptJitCodeGenerator::write_lambda(const Address &p_target, GDScriptFunction *p_function, const Vector<Address> &p_captures, bool p_use_self) {
	print_debug("write_lambda");
	append_opcode_and_argcount(p_use_self ? GDScriptFunction::OPCODE_CREATE_SELF_LAMBDA : GDScriptFunction::OPCODE_CREATE_LAMBDA, 1 + p_captures.size());
	for (int i = 0; i < p_captures.size(); i++) {
		append(p_captures[i]);
	}

	CallTarget ct = get_call_target(p_target);
	append(ct.target);
	append(p_captures.size());
	append(p_function);
	ct.cleanup();
}

void GDScriptJitCodeGenerator::write_construct(const Address &p_target, Variant::Type p_type, const Vector<Address> &p_arguments) {
	print_debug("write_construct");
	// Try to find an appropriate constructor.
	bool all_have_type = true;
	Vector<Variant::Type> arg_types;
	for (int i = 0; i < p_arguments.size(); i++) {
		if (!HAS_BUILTIN_TYPE(p_arguments[i])) {
			all_have_type = false;
			break;
		}
		arg_types.push_back(p_arguments[i].type.builtin_type);
	}
	if (all_have_type) {
		int valid_constructor = -1;
		for (int i = 0; i < Variant::get_constructor_count(p_type); i++) {
			if (Variant::get_constructor_argument_count(p_type, i) != p_arguments.size()) {
				continue;
			}
			int types_correct = true;
			for (int j = 0; j < arg_types.size(); j++) {
				if (arg_types[j] != Variant::get_constructor_argument_type(p_type, i, j)) {
					types_correct = false;
					break;
				}
			}
			if (types_correct) {
				valid_constructor = i;
				break;
			}
		}
		if (valid_constructor >= 0) {
			Vector<ValueId> arg_ptrs;
			for (int i = 0; i < p_arguments.size(); i++) {
				arg_ptrs.push_back(ir.emit_load_ptr(p_arguments[i]));
			}
			CallTarget ct_ir = get_call_target(p_target);
			const ValueId target_ptr = ir.emit_load_ptr(ct_ir.target);
			ir.emit_construct_validated(arg_ptrs, target_ptr, Variant::get_validated_constructor(p_type, valid_constructor));
			ct_ir.cleanup();

			append_opcode_and_argcount(GDScriptFunction::OPCODE_CONSTRUCT_VALIDATED, 1 + p_arguments.size());
			for (int i = 0; i < p_arguments.size(); i++) {
				append(p_arguments[i]);
			}
			CallTarget ct = get_call_target(p_target);
			append(ct.target);
			append(p_arguments.size());
			append(Variant::get_validated_constructor(p_type, valid_constructor));
			ct.cleanup();
#ifdef DEBUG_ENABLED
			add_debug_name(constructors_names, get_constructor_pos(Variant::get_validated_constructor(p_type, valid_constructor)), Variant::get_type_name(p_type));
#endif
			return;
		}
	}

	Vector<ValueId> arg_ptrs;
	for (int i = 0; i < p_arguments.size(); i++) {
		arg_ptrs.push_back(ir.emit_load_ptr(p_arguments[i]));
	}
	CallTarget ct_ir = get_call_target(p_target);
	const ValueId target_ptr = ir.emit_load_ptr(ct_ir.target);
	ir.emit_construct(arg_ptrs, target_ptr, p_type);
	ct_ir.cleanup();

	append_opcode_and_argcount(GDScriptFunction::OPCODE_CONSTRUCT, 1 + p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	CallTarget ct = get_call_target(p_target);
	append(ct.target);
	append(p_arguments.size());
	append(p_type);
	ct.cleanup();
}

void GDScriptJitCodeGenerator::write_construct_array(const Address &p_target, const Vector<Address> &p_arguments) {
	print_debug("write_construct_array");
	Vector<ValueId> arg_ptrs;
	arg_ptrs.resize(p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		arg_ptrs.write[i] = ir.emit_load_ptr(p_arguments[i]);
	}
	CallTarget ct_ir = get_call_target(p_target);
	const ValueId dst_ptr = ir.emit_load_ptr(ct_ir.target);
	ir.emit_construct_array(arg_ptrs, dst_ptr);
	ct_ir.cleanup();

	append_opcode_and_argcount(GDScriptFunction::OPCODE_CONSTRUCT_ARRAY, 1 + p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	CallTarget ct = get_call_target(p_target);
	append(ct.target);
	append(p_arguments.size());
	ct.cleanup();
}

void GDScriptJitCodeGenerator::write_construct_typed_array(const Address &p_target, const GDScriptDataType &p_element_type, const Vector<Address> &p_arguments) {
	print_debug("write_construct_typed_array");
	Vector<ValueId> arg_ptrs;
	arg_ptrs.resize(p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		arg_ptrs.write[i] = ir.emit_load_ptr(p_arguments[i]);
	}
	CallTarget ct_ir = get_call_target(p_target);
	const ValueId dst_ptr = ir.emit_load_ptr(ct_ir.target);
	const Address script_type_addr(GDScriptCodeGenerator::Address::CONSTANT, get_constant_pos(p_element_type.script_type), GDScriptDataType());
	const ValueId script_type_ptr = ir.emit_load_ptr(script_type_addr);
	ir.emit_construct_typed_array(arg_ptrs, dst_ptr, script_type_ptr, p_element_type.builtin_type, get_name_map_pos(p_element_type.native_type));
	ct_ir.cleanup();

	append_opcode_and_argcount(GDScriptFunction::OPCODE_CONSTRUCT_TYPED_ARRAY, 2 + p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	CallTarget ct = get_call_target(p_target);
	append(ct.target);
	append(get_constant_pos(p_element_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
	append(p_arguments.size());
	append(p_element_type.builtin_type);
	append(p_element_type.native_type);
	ct.cleanup();
}

void GDScriptJitCodeGenerator::write_construct_dictionary(const Address &p_target, const Vector<Address> &p_arguments) {
	print_debug("write_construct_dictionary");
	Vector<ValueId> arg_ptrs;
	arg_ptrs.resize(p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		arg_ptrs.write[i] = ir.emit_load_ptr(p_arguments[i]);
	}
	CallTarget ct_ir = get_call_target(p_target);
	const ValueId dst_ptr = ir.emit_load_ptr(ct_ir.target);
	ir.emit_construct_dictionary(arg_ptrs, dst_ptr);
	ct_ir.cleanup();

	append_opcode_and_argcount(GDScriptFunction::OPCODE_CONSTRUCT_DICTIONARY, 1 + p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	CallTarget ct = get_call_target(p_target);
	append(ct.target);
	append(p_arguments.size() / 2); // This is number of key-value pairs, so only half of actual arguments.
	ct.cleanup();
}

void GDScriptJitCodeGenerator::write_construct_typed_dictionary(const Address &p_target, const GDScriptDataType &p_key_type, const GDScriptDataType &p_value_type, const Vector<Address> &p_arguments) {
	print_debug("write_construct_typed_dictionary");
	Vector<ValueId> arg_ptrs;
	arg_ptrs.resize(p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		arg_ptrs.write[i] = ir.emit_load_ptr(p_arguments[i]);
	}
	CallTarget ct_ir = get_call_target(p_target);
	const ValueId dst_ptr = ir.emit_load_ptr(ct_ir.target);
	const Address key_script_type_addr(GDScriptCodeGenerator::Address::CONSTANT, get_constant_pos(p_key_type.script_type), GDScriptDataType());
	const Address value_script_type_addr(GDScriptCodeGenerator::Address::CONSTANT, get_constant_pos(p_value_type.script_type), GDScriptDataType());
	const ValueId key_script_type_ptr = ir.emit_load_ptr(key_script_type_addr);
	const ValueId value_script_type_ptr = ir.emit_load_ptr(value_script_type_addr);
	ir.emit_construct_typed_dictionary(arg_ptrs, dst_ptr, key_script_type_ptr, value_script_type_ptr, p_key_type.builtin_type, get_name_map_pos(p_key_type.native_type), p_value_type.builtin_type, get_name_map_pos(p_value_type.native_type));
	ct_ir.cleanup();

	append_opcode_and_argcount(GDScriptFunction::OPCODE_CONSTRUCT_TYPED_DICTIONARY, 3 + p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	CallTarget ct = get_call_target(p_target);
	append(ct.target);
	append(get_constant_pos(p_key_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
	append(get_constant_pos(p_value_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
	append(p_arguments.size() / 2); // This is number of key-value pairs, so only half of actual arguments.
	append(p_key_type.builtin_type);
	append(p_key_type.native_type);
	append(p_value_type.builtin_type);
	append(p_value_type.native_type);
	ct.cleanup();
}

void GDScriptJitCodeGenerator::write_await(const Address &p_target, const Address &p_operand) {
	print_debug("write_await");
	append_opcode(GDScriptFunction::OPCODE_AWAIT);
	append(p_operand);
	append_opcode(GDScriptFunction::OPCODE_AWAIT_RESUME);
	append(p_target);
}

void GDScriptJitCodeGenerator::write_if(const Address &p_condition) {
	print_debug("write_if");
	append_opcode(GDScriptFunction::OPCODE_JUMP_IF_NOT);
	append(p_condition);
	if_jmp_addrs.push_back(opcodes.size());
	append(0); // Jump destination, will be patched.

	const LabelId ir_false_label = ir.new_label();
	const LabelId ir_end_label = ir.new_label();
	const uint32_t ir_condition_type = HAS_BUILTIN_TYPE(p_condition) ? p_condition.type.builtin_type : Variant::NIL;
	if (ir_condition_type == Variant::INT || ir_condition_type == Variant::BOOL) {
		const ValueId ir_condition_value = ir.emit_load(p_condition);
		ir.emit_jump_if_zero(ir_condition_value, ir_false_label);
	} else {
		const ValueId ir_condition_ptr = ir.emit_load_ptr(p_condition);
		const ValueId ir_condition_bool = ir.emit_booleanize(ir_condition_ptr, ir_condition_type);
		ir.emit_jump_if_zero(ir_condition_bool, ir_false_label);
	}
	ir_if_stack.push_back(IRIfLabels{ ir_false_label, ir_end_label, false });
}

void GDScriptJitCodeGenerator::write_else() {
	print_debug("write_else");
	append_opcode(GDScriptFunction::OPCODE_JUMP); // Jump from true if block;
	int else_jmp_addr = opcodes.size();
	append(0); // Jump destination, will be patched.

	patch_jump(if_jmp_addrs.back()->get());
	if_jmp_addrs.pop_back();
	if_jmp_addrs.push_back(else_jmp_addr);

	if (!ir_if_stack.is_empty()) {
		IRIfLabels &ir_if = ir_if_stack.back()->get();
		ir.emit_jump(ir_if.end_label);
		ir.bind_label(ir_if.false_label);
		ir_if.has_else = true;
	}
}

void GDScriptJitCodeGenerator::write_endif() {
	print_debug("write_endif");
	patch_jump(if_jmp_addrs.back()->get());
	if_jmp_addrs.pop_back();

	if (!ir_if_stack.is_empty()) {
		const IRIfLabels &ir_if = ir_if_stack.back()->get();
		if (ir_if.has_else) {
			ir.bind_label(ir_if.end_label);
		} else {
			ir.bind_label(ir_if.false_label);
		}
		ir_if_stack.pop_back();
	}
}

void GDScriptJitCodeGenerator::write_jump_if_shared(const Address &p_value) {
	print_debug("write_jump_if_shared");
	append_opcode(GDScriptFunction::OPCODE_JUMP_IF_SHARED);
	append(p_value);
	if_jmp_addrs.push_back(opcodes.size());
	append(0); // Jump destination, will be patched.
}

void GDScriptJitCodeGenerator::write_end_jump_if_shared() {
	print_debug("write_end_jump_if_shared");
	patch_jump(if_jmp_addrs.back()->get());
	if_jmp_addrs.pop_back();
}

void GDScriptJitCodeGenerator::start_for(const GDScriptDataType &p_iterator_type, const GDScriptDataType &p_list_type, bool p_is_range) {
	print_debug("start_for");
	Address counter(Address::LOCAL_VARIABLE, add_local("@counter_pos", p_iterator_type), p_iterator_type);

	// Store state.
	for_counter_variables.push_back(counter);

	if (p_is_range) {
		GDScriptDataType int_type;
		int_type.kind = GDScriptDataType::BUILTIN;
		int_type.builtin_type = Variant::INT;

		Address range_from(Address::LOCAL_VARIABLE, add_local("@range_from", int_type), int_type);
		Address range_to(Address::LOCAL_VARIABLE, add_local("@range_to", int_type), int_type);
		Address range_step(Address::LOCAL_VARIABLE, add_local("@range_step", int_type), int_type);

		// Store state.
		for_range_from_variables.push_back(range_from);
		for_range_to_variables.push_back(range_to);
		for_range_step_variables.push_back(range_step);
	} else {
		Address container(Address::LOCAL_VARIABLE, add_local("@container_pos", p_list_type), p_list_type);

		// Store state.
		for_container_variables.push_back(container);
	}
}

void GDScriptJitCodeGenerator::write_for_list_assignment(const Address &p_list) {
	print_debug("write_for_list_assignment");
	const Address &container = for_container_variables.back()->get();

	emit_assign_ir_simple(container, p_list);
	// Assign container.
	append_opcode(GDScriptFunction::OPCODE_ASSIGN);
	append(container);
	append(p_list);
}

void GDScriptJitCodeGenerator::write_for_range_assignment(const Address &p_from, const Address &p_to, const Address &p_step) {
	const Address &range_from = for_range_from_variables.back()->get();
	const Address &range_to = for_range_to_variables.back()->get();
	const Address &range_step = for_range_step_variables.back()->get();

	// Assign range args.
	if (range_from.type == p_from.type) {
		for_range_from_variables.back()->get() = p_from;
	} else {
		write_assign_with_conversion(range_from, p_from);
	}
	if (range_to.type == p_to.type) {
		for_range_to_variables.back()->get() = p_to;
	} else {
		write_assign_with_conversion(range_to, p_to);
	}
	if (range_step.type == p_step.type) {
		for_range_step_variables.back()->get() = p_step;
	} else {
		write_assign_with_conversion(range_step, p_step);
	}
}

void GDScriptJitCodeGenerator::write_for(const Address &p_variable, bool p_use_conversion, bool p_is_range) {
	print_debug("write_for");
	const Address &counter = for_counter_variables.back()->get();
	const Address &container = p_is_range ? Address() : for_container_variables.back()->get();
	const Address &range_from = p_is_range ? for_range_from_variables.back()->get() : Address();
	const Address &range_to = p_is_range ? for_range_to_variables.back()->get() : Address();
	const Address &range_step = p_is_range ? for_range_step_variables.back()->get() : Address();

	current_breaks_to_patch.push_back(List<int>());

	Address temp;
	if (p_use_conversion) {
		temp = Address(Address::LOCAL_VARIABLE, add_local("@iterator_temp", GDScriptDataType()));
	}
	const Address &ir_iterator = p_use_conversion ? temp : p_variable;
	bool has_custom_ir_loop = false;

	GDScriptFunction::Opcode begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN;
	GDScriptFunction::Opcode iterate_opcode = GDScriptFunction::OPCODE_ITERATE;
	if (p_is_range) {
		begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_RANGE;
		iterate_opcode = GDScriptFunction::OPCODE_ITERATE_RANGE;

		const LabelId ir_continue_label = ir.new_label();
		const LabelId ir_end_label = ir.new_label();

		ValueId ir_from {};
		int64_t const_from = 0;
		if (try_get_constant_i64(range_from, const_from)) {
			if (const_from == 0) {
				ir_from = ir.emit_zero64();
			} else {
				ir_from = ir.emit_load(range_from);
			}
		} else {
			ir_from = ir.emit_load(range_from);
		}

		const ValueId ir_to = ir.emit_load(range_to);
		const ValueId ir_step = ir.emit_load(range_step);
		const ValueId ir_initial_count = ir.emit_sub64(ir_from, ir_step);
		ir.emit_store_type(counter, Variant::INT);
		ir.emit_store(counter, ir_initial_count);
		ir.emit_store_type(ir_iterator, Variant::INT);
		ir.bind_label(ir_continue_label);

		const ValueId ir_count = ir.emit_load(counter);
		ValueId ir_next_count {};

		int64_t constant_step = 0;
		if (try_get_constant_i64(range_step, constant_step)) {
			ir_next_count = ir.emit_add64(ir_count, constant_step);
			if (constant_step > 0) {
				const ValueId ir_result = ir.emit_lt64(ir_next_count, ir_to);
				ir.emit_jump_if_zero(ir_result, ir_end_label);
			} else if (constant_step < 0) {
				const ValueId ir_delta = ir.emit_gt64(ir_to, ir_next_count);
				ir.emit_jump_if_zero(ir_delta, ir_end_label);
			} else {
				const ValueId ir_delta = ir.emit_sub64(ir_next_count, ir_to);
				const ValueId ir_distance = ir.emit_mul64(ir_delta, ir_step);
				ir.emit_jump_if_ge_zero(ir_distance, ir_end_label);
			}
		} else {
			ir_next_count = ir.emit_add64(ir_count, ir_step);
			const ValueId ir_delta = ir.emit_sub64(ir_next_count, ir_to);
			const ValueId ir_distance = ir.emit_mul64(ir_delta, ir_step);
			ir.emit_jump_if_ge_zero(ir_distance, ir_end_label);
		}

		ir.emit_store(counter, ir_next_count);
		ir.emit_store(ir_iterator, ir_next_count);

		ir_loop_stack.push_back(IRLoopLabels{ true, ir_continue_label, ir_end_label });
		has_custom_ir_loop = true;
	} else if (container.type.has_type()) {
		if (container.type.kind == GDScriptDataType::BUILTIN) {
			switch (container.type.builtin_type) {
				case Variant::INT:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_INT;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_INT;
					{
						const LabelId ir_continue_label = ir.new_label();
						const LabelId ir_end_label = ir.new_label();

						const ValueId ir_size = ir.emit_load(container);
						const ValueId ir_zero = ir.emit_zero64();
						const ValueId ir_initial_count = ir.emit_add64(ir_zero, -1);
						ir.emit_store_type(counter, Variant::INT);
						ir.emit_store(counter, ir_initial_count);
						ir.emit_store_type(ir_iterator, Variant::INT);
						ir.bind_label(ir_continue_label);

						const ValueId ir_count = ir.emit_load(counter);
						const ValueId ir_next_count = ir.emit_add64(ir_count, 1);
						const ValueId ir_continue = ir.emit_lt64(ir_next_count, ir_size);
						ir.emit_jump_if_zero(ir_continue, ir_end_label);
						ir.emit_store(counter, ir_next_count);
						ir.emit_store(ir_iterator, ir_next_count);
						ir_loop_stack.push_back(IRLoopLabels{ true, ir_continue_label, ir_end_label });
						has_custom_ir_loop = true;
					}
					break;
				case Variant::FLOAT:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_FLOAT;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_FLOAT;
					{
						const LabelId ir_continue_label = ir.new_label();
						const LabelId ir_end_label = ir.new_label();
						const Address ir_neg_one_addr(Address::CONSTANT, get_constant_pos(Variant(-1.0)), GDScriptDataType());
						const Address ir_one_addr(Address::CONSTANT, get_constant_pos(Variant(1.0)), GDScriptDataType());

						const ValueId ir_size = ir.emit_loadf64(container);
						const ValueId ir_initial_count = ir.emit_loadf64(ir_neg_one_addr);
						ir.emit_store_type(counter, Variant::FLOAT);
						ir.emit_storef64(counter, ir_initial_count);
						ir.emit_store_type(ir_iterator, Variant::FLOAT);
						ir.bind_label(ir_continue_label);

						const ValueId ir_count = ir.emit_loadf64(counter);
						const ValueId ir_one = ir.emit_loadf64(ir_one_addr);
						const ValueId ir_next_count = ir.emit_addf64(ir_count, ir_one);
						const ValueId ir_continue = ir.emit_ltf64(ir_next_count, ir_size);
						ir.emit_jump_if_zero(ir_continue, ir_end_label);
						ir.emit_storef64(counter, ir_next_count);
						ir.emit_storef64(ir_iterator, ir_next_count);
						ir_loop_stack.push_back(IRLoopLabels{ true, ir_continue_label, ir_end_label });
						has_custom_ir_loop = true;
					}
					break;
				case Variant::VECTOR2:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_VECTOR2;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_VECTOR2;
					break;
				case Variant::VECTOR2I:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_VECTOR2I;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_VECTOR2I;
					break;
				case Variant::VECTOR3:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_VECTOR3;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_VECTOR3;
					break;
				case Variant::VECTOR3I:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_VECTOR3I;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_VECTOR3I;
					break;
				case Variant::STRING:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_STRING;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_STRING;
					break;
				case Variant::DICTIONARY:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_DICTIONARY;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_DICTIONARY;
					break;
				case Variant::ARRAY:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_ARRAY;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_ARRAY;
					break;
				case Variant::PACKED_BYTE_ARRAY:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_BYTE_ARRAY;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_PACKED_BYTE_ARRAY;
					break;
				case Variant::PACKED_INT32_ARRAY:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_INT32_ARRAY;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_PACKED_INT32_ARRAY;
					break;
				case Variant::PACKED_INT64_ARRAY:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_INT64_ARRAY;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_PACKED_INT64_ARRAY;
					break;
				case Variant::PACKED_FLOAT32_ARRAY:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_FLOAT32_ARRAY;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_PACKED_FLOAT32_ARRAY;
					break;
				case Variant::PACKED_FLOAT64_ARRAY:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_FLOAT64_ARRAY;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_PACKED_FLOAT64_ARRAY;
					break;
				case Variant::PACKED_STRING_ARRAY:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_STRING_ARRAY;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_PACKED_STRING_ARRAY;
					break;
				case Variant::PACKED_VECTOR2_ARRAY:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_VECTOR2_ARRAY;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_PACKED_VECTOR2_ARRAY;
					break;
				case Variant::PACKED_VECTOR3_ARRAY:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_VECTOR3_ARRAY;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_PACKED_VECTOR3_ARRAY;
					break;
				case Variant::PACKED_COLOR_ARRAY:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_COLOR_ARRAY;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_PACKED_COLOR_ARRAY;
					break;
				case Variant::PACKED_VECTOR4_ARRAY:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_VECTOR4_ARRAY;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_PACKED_VECTOR4_ARRAY;
					break;
				default:
					break;
			}
		} else {
			begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_OBJECT;
			iterate_opcode = GDScriptFunction::OPCODE_ITERATE_OBJECT;
		}
	}
	if (!p_is_range && !has_custom_ir_loop) {
		const LabelId ir_body_label = ir.new_label();
		const LabelId ir_continue_label = ir.new_label();
		const LabelId ir_end_label = ir.new_label();
		const ValueId ir_container_ptr = ir.emit_load_ptr(container);
		const ValueId ir_counter_ptr = ir.emit_load_ptr(counter);
		const ValueId ir_iterator_ptr = ir.emit_load_ptr(ir_iterator);
		const ValueId ir_has_item = ir.emit_iterate_begin(ir_container_ptr, ir_counter_ptr, ir_iterator_ptr);
		ir.emit_jump_if_zero(ir_has_item, ir_end_label);
		ir.bind_label(ir_body_label);
		ir_loop_stack.push_back(IRLoopLabels{ true, ir_continue_label, ir_end_label, ir_body_label, ir_container_ptr, ir_counter_ptr, ir_iterator_ptr, true });
	}

	// Begin loop.
	append_opcode(begin_opcode);
	append(counter);
	if (p_is_range) {
		append(range_from);
		append(range_to);
		append(range_step);
	} else {
		append(container);
	}
	append(p_use_conversion ? temp : p_variable);
	for_jmp_addrs.push_back(opcodes.size());
	append(0); // End of loop address, will be patched.
	append_opcode(GDScriptFunction::OPCODE_JUMP);
	append(opcodes.size() + (p_is_range ? 7 : 6)); // Skip over 'continue' code.

	// Next iteration.
	int continue_addr = opcodes.size();
	continue_addrs.push_back(continue_addr);
	append_opcode(iterate_opcode);
	append(counter);
	if (p_is_range) {
		append(range_to);
		append(range_step);
	} else {
		append(container);
	}
	append(p_use_conversion ? temp : p_variable);
	for_jmp_addrs.push_back(opcodes.size());
	append(0); // Jump destination, will be patched.

	if (p_use_conversion) {
		write_assign_with_conversion(p_variable, temp);
		if (p_variable.type.can_contain_object()) {
			clear_address(temp); // Can contain `RefCounted`, so clear it.
		}
	}
}

void GDScriptJitCodeGenerator::write_endfor(bool p_is_range) {
	print_debug("write_endfor");
	// Jump back to loop check.
	append_opcode(GDScriptFunction::OPCODE_JUMP);
	append(continue_addrs.back()->get());
	continue_addrs.pop_back();

	if (!ir_loop_stack.is_empty()) {
		const IRLoopLabels ir_loop = ir_loop_stack.back()->get();
		if (ir_loop.supported) {
			ir.emit_jump(ir_loop.continue_label);
			if (ir_loop.needs_iterate_step) {
				ir.bind_label(ir_loop.continue_label);
				const ValueId ir_has_item = ir.emit_iterate(ir_loop.container_ptr, ir_loop.counter_ptr, ir_loop.iterator_ptr);
				ir.emit_jump_if_zero(ir_has_item, ir_loop.break_label);
				ir.emit_jump(ir_loop.body_label);
			}
			ir.bind_label(ir_loop.break_label);
		}
		ir_loop_stack.pop_back();
	}

	// Patch end jumps (two of them).
	for (int i = 0; i < 2; i++) {
		patch_jump(for_jmp_addrs.back()->get());
		for_jmp_addrs.pop_back();
	}

	// Patch break statements.
	for (const int &E : current_breaks_to_patch.back()->get()) {
		patch_jump(E);
	}
	current_breaks_to_patch.pop_back();

	// Pop state.
	for_counter_variables.pop_back();
	if (p_is_range) {
		for_range_from_variables.pop_back();
		for_range_to_variables.pop_back();
		for_range_step_variables.pop_back();
	} else {
		for_container_variables.pop_back();
	}
}

void GDScriptJitCodeGenerator::start_while_condition() {
	print_debug("start_while_condition");
	current_breaks_to_patch.push_back(List<int>());
	continue_addrs.push_back(opcodes.size());
	const LabelId ir_continue_label = ir.new_label();
	const LabelId ir_end_label = ir.new_label();
	ir.bind_label(ir_continue_label);
	ir_loop_stack.push_back(IRLoopLabels{ true, ir_continue_label, ir_end_label });
}

void GDScriptJitCodeGenerator::write_while(const Address &p_condition) {
	print_debug("write_while");
	// Condition check.
	append_opcode(GDScriptFunction::OPCODE_JUMP_IF_NOT);
	append(p_condition);
	while_jmp_addrs.push_back(opcodes.size());
	append(0); // End of loop address, will be patched.

	if (!ir_loop_stack.is_empty() && ir_loop_stack.back()->get().supported) {
		const uint32_t ir_condition_type = HAS_BUILTIN_TYPE(p_condition) ? p_condition.type.builtin_type : Variant::NIL;
		if (ir_condition_type == Variant::INT || ir_condition_type == Variant::BOOL) {
			const ValueId ir_condition_value = ir.emit_load(p_condition);
			ir.emit_jump_if_zero(ir_condition_value, ir_loop_stack.back()->get().break_label);
		} else {
			const ValueId ir_condition_ptr = ir.emit_load_ptr(p_condition);
			const ValueId ir_condition_bool = ir.emit_booleanize(ir_condition_ptr, ir_condition_type);
			ir.emit_jump_if_zero(ir_condition_bool, ir_loop_stack.back()->get().break_label);
		}
	}
}

void GDScriptJitCodeGenerator::write_endwhile() {
	print_debug("write_endwhile");
	// Jump back to loop check.
	append_opcode(GDScriptFunction::OPCODE_JUMP);
	append(continue_addrs.back()->get());
	continue_addrs.pop_back();

	// Patch end jump.
	patch_jump(while_jmp_addrs.back()->get());
	while_jmp_addrs.pop_back();

	// Patch break statements.
	for (const int &E : current_breaks_to_patch.back()->get()) {
		patch_jump(E);
	}
	current_breaks_to_patch.pop_back();
	if (!ir_loop_stack.is_empty()) {
		const IRLoopLabels ir_loop = ir_loop_stack.back()->get();
		if (ir_loop.supported) {
			ir.emit_jump(ir_loop.continue_label);
			ir.bind_label(ir_loop.break_label);
		}
		ir_loop_stack.pop_back();
	}
}

void GDScriptJitCodeGenerator::write_break() {
	print_debug("write_break");
	append_opcode(GDScriptFunction::OPCODE_JUMP);
	current_breaks_to_patch.back()->get().push_back(opcodes.size());
	append(0);

	if (!ir_loop_stack.is_empty() && ir_loop_stack.back()->get().supported) {
		ir.emit_jump(ir_loop_stack.back()->get().break_label);
	}
}

void GDScriptJitCodeGenerator::write_continue() {
	print_debug("write_continue");
	append_opcode(GDScriptFunction::OPCODE_JUMP);
	append(continue_addrs.back()->get());

	if (!ir_loop_stack.is_empty() && ir_loop_stack.back()->get().supported) {
		ir.emit_jump(ir_loop_stack.back()->get().continue_label);
	}
}

void GDScriptJitCodeGenerator::write_breakpoint() {
	print_debug("write_breakpoint");
	append_opcode(GDScriptFunction::OPCODE_BREAKPOINT);
}

void GDScriptJitCodeGenerator::write_newline(int p_line) {
	print_debug("write_newline");
	if (GDScriptLanguage::get_singleton()->should_track_call_stack()) {
		// Add newline for debugger and stack tracking if enabled in the project settings.
		append_opcode(GDScriptFunction::OPCODE_LINE);
		append(p_line);
		current_line = p_line;
	}
}

void GDScriptJitCodeGenerator::write_return(const Address &p_return_value) {
	print_debug("write_return");
	auto emit_variant_return = [&](const Address &p_value) {
		const ValueId source_ptr = ir.emit_load_ptr(p_value);
		ir.emit_return_variant(source_ptr);
		append_opcode(GDScriptFunction::OPCODE_RETURN);
		append(p_value);
	};

	if (!function->return_type.has_type() || p_return_value.type.has_type()) {
		// Either the function is untyped or the return value is also typed.

		// If this is a typed function, then we need to check for potential conversions.
		if (function->return_type.has_type()) {
			if (function->return_type.kind == GDScriptDataType::BUILTIN && function->return_type.builtin_type == Variant::ARRAY && function->return_type.has_container_element_type(0)) {
				// Typed array.
				const GDScriptDataType &element_type = function->return_type.get_container_element_type(0);
				const ValueId source_ptr = ir.emit_load_ptr(p_return_value);
				const Address script_type_addr(GDScriptCodeGenerator::Address::CONSTANT, get_constant_pos(element_type.script_type), GDScriptDataType());
				const ValueId script_type_ptr = ir.emit_load_ptr(script_type_addr);
				ir.emit_return_typed_array(source_ptr, script_type_ptr, element_type.builtin_type, get_name_map_pos(element_type.native_type));
				append_opcode(GDScriptFunction::OPCODE_RETURN_TYPED_ARRAY);
				append(p_return_value);
				append(get_constant_pos(element_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
				append(element_type.builtin_type);
				append(element_type.native_type);
			} else if (function->return_type.kind == GDScriptDataType::BUILTIN && function->return_type.builtin_type == Variant::DICTIONARY &&
					function->return_type.has_container_element_types()) {
				// Typed dictionary.
				const GDScriptDataType &key_type = function->return_type.get_container_element_type_or_variant(0);
				const GDScriptDataType &value_type = function->return_type.get_container_element_type_or_variant(1);
				const ValueId source_ptr = ir.emit_load_ptr(p_return_value);
				const Address key_script_type_addr(GDScriptCodeGenerator::Address::CONSTANT, get_constant_pos(key_type.script_type), GDScriptDataType());
				const Address value_script_type_addr(GDScriptCodeGenerator::Address::CONSTANT, get_constant_pos(value_type.script_type), GDScriptDataType());
				const ValueId key_script_type_ptr = ir.emit_load_ptr(key_script_type_addr);
				const ValueId value_script_type_ptr = ir.emit_load_ptr(value_script_type_addr);
				ir.emit_return_typed_dictionary(source_ptr, key_script_type_ptr, value_script_type_ptr, key_type.builtin_type, get_name_map_pos(key_type.native_type), value_type.builtin_type, get_name_map_pos(value_type.native_type));
				append_opcode(GDScriptFunction::OPCODE_RETURN_TYPED_DICTIONARY);
				append(p_return_value);
				append(get_constant_pos(key_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
				append(get_constant_pos(value_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
				append(key_type.builtin_type);
				append(key_type.native_type);
				append(value_type.builtin_type);
				append(value_type.native_type);
			} else if (function->return_type.kind == GDScriptDataType::BUILTIN && p_return_value.type.kind == GDScriptDataType::BUILTIN && function->return_type.builtin_type != p_return_value.type.builtin_type) {
				// Add conversion.
				const ValueId source_ptr = ir.emit_load_ptr(p_return_value);
				ir.emit_return_typed_builtin(source_ptr, function->return_type.builtin_type);
				append_opcode(GDScriptFunction::OPCODE_RETURN_TYPED_BUILTIN);
				append(p_return_value);
				append(function->return_type.builtin_type);
			} else {
				if (function->return_type.kind == GDScriptDataType::BUILTIN &&
						(function->return_type.builtin_type == Variant::INT || function->return_type.builtin_type == Variant::FLOAT) &&
						p_return_value.type.kind == GDScriptDataType::BUILTIN &&
						p_return_value.type.builtin_type == function->return_type.builtin_type) {
					const auto v1 = function->return_type.builtin_type == Variant::FLOAT
							? ir.emit_loadf64(p_return_value)
							: ir.emit_load(p_return_value);
					ir.emit_return(v1);
					append_opcode(GDScriptFunction::OPCODE_RETURN);
					append(p_return_value);
				} else if (function->return_type.kind == GDScriptDataType::BUILTIN) {
					const ValueId source_ptr = ir.emit_load_ptr(p_return_value);
					ir.emit_return_typed_builtin(source_ptr, function->return_type.builtin_type);
					append_opcode(GDScriptFunction::OPCODE_RETURN_TYPED_BUILTIN);
					append(p_return_value);
					append(function->return_type.builtin_type);
				} else {
					emit_variant_return(p_return_value);
				}
			}
		} else {
			emit_variant_return(p_return_value);
		}
	} else {
		switch (function->return_type.kind) {
			case GDScriptDataType::BUILTIN: {
				if (function->return_type.builtin_type == Variant::ARRAY && function->return_type.has_container_element_type(0)) {
					const GDScriptDataType &element_type = function->return_type.get_container_element_type(0);
					const ValueId source_ptr = ir.emit_load_ptr(p_return_value);
					const Address script_type_addr(GDScriptCodeGenerator::Address::CONSTANT, get_constant_pos(element_type.script_type), GDScriptDataType());
					const ValueId script_type_ptr = ir.emit_load_ptr(script_type_addr);
					ir.emit_return_typed_array(source_ptr, script_type_ptr, element_type.builtin_type, get_name_map_pos(element_type.native_type));
					append_opcode(GDScriptFunction::OPCODE_RETURN_TYPED_ARRAY);
					append(p_return_value);
					append(get_constant_pos(element_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
					append(element_type.builtin_type);
					append(element_type.native_type);
				} else if (function->return_type.builtin_type == Variant::DICTIONARY && function->return_type.has_container_element_types()) {
					const GDScriptDataType &key_type = function->return_type.get_container_element_type_or_variant(0);
					const GDScriptDataType &value_type = function->return_type.get_container_element_type_or_variant(1);
					const ValueId source_ptr = ir.emit_load_ptr(p_return_value);
					const Address key_script_type_addr(GDScriptCodeGenerator::Address::CONSTANT, get_constant_pos(key_type.script_type), GDScriptDataType());
					const Address value_script_type_addr(GDScriptCodeGenerator::Address::CONSTANT, get_constant_pos(value_type.script_type), GDScriptDataType());
					const ValueId key_script_type_ptr = ir.emit_load_ptr(key_script_type_addr);
					const ValueId value_script_type_ptr = ir.emit_load_ptr(value_script_type_addr);
					ir.emit_return_typed_dictionary(source_ptr, key_script_type_ptr, value_script_type_ptr, key_type.builtin_type, get_name_map_pos(key_type.native_type), value_type.builtin_type, get_name_map_pos(value_type.native_type));
					append_opcode(GDScriptFunction::OPCODE_RETURN_TYPED_DICTIONARY);
					append(p_return_value);
					append(get_constant_pos(key_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
					append(get_constant_pos(value_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
					append(key_type.builtin_type);
					append(key_type.native_type);
					append(value_type.builtin_type);
					append(value_type.native_type);
				} else {
					const ValueId source_ptr = ir.emit_load_ptr(p_return_value);
					ir.emit_return_typed_builtin(source_ptr, function->return_type.builtin_type);
					append_opcode(GDScriptFunction::OPCODE_RETURN_TYPED_BUILTIN);
					append(p_return_value);
					append(function->return_type.builtin_type);
				}
			} break;
			case GDScriptDataType::NATIVE: {
				append_opcode(GDScriptFunction::OPCODE_RETURN_TYPED_NATIVE);
				append(p_return_value);
				int class_idx = GDScriptLanguage::get_singleton()->get_global_map()[function->return_type.native_type];
				Variant nc = GDScriptLanguage::get_singleton()->get_global_array()[class_idx];
				class_idx = get_constant_pos(nc) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS);
				append(class_idx);
			} break;
			case GDScriptDataType::GDSCRIPT:
			case GDScriptDataType::SCRIPT: {
				Variant script = function->return_type.script_type;
				int script_idx = get_constant_pos(script) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS);

				append_opcode(GDScriptFunction::OPCODE_RETURN_TYPED_SCRIPT);
				append(p_return_value);
				append(script_idx);
			} break;
			default: {
				ERR_PRINT("Compiler bug: unresolved return.");

				// Shouldn't get here, but fail-safe to a regular return;
				emit_variant_return(p_return_value);
			} break;
		}
	}
}

void GDScriptJitCodeGenerator::write_assert(const Address &p_test, const Address &p_message) {
	print_debug("write_assert");
	append_opcode(GDScriptFunction::OPCODE_ASSERT);
	append(p_test);
	append(p_message);
}

void GDScriptJitCodeGenerator::start_block() {
	print_debug("start_block");
	push_stack_identifiers();
}

void GDScriptJitCodeGenerator::end_block() {
	print_debug("end_block");
	pop_stack_identifiers();
}

void GDScriptJitCodeGenerator::clear_temporaries() {
	print_debug("clear_temporaries");
	for (int slot_idx : temporaries_pending_clear) {
		// The temporary may have been reused as something else since it was added to the list.
		// In that case, there's **no** need to clear it.
		if (temporaries[slot_idx].can_contain_object) {
			clear_address(Address(Address::TEMPORARY, slot_idx)); // Can contain `RefCounted`, so clear it.
		}
	}
	temporaries_pending_clear.clear();
}

void GDScriptJitCodeGenerator::clear_address(const Address &p_address) {
	print_debug("clear_address");
	// Do not check `is_local_dirty()` here! Always clear the address since the codegen doesn't track the compiler.
	// Also, this method is used to initialize local variables of built-in types, since they cannot be `null`.

	if (p_address.type.kind == GDScriptDataType::BUILTIN) {
		switch (p_address.type.builtin_type) {
			case Variant::BOOL:
				write_assign_false(p_address);
				break;
			case Variant::DICTIONARY:
				if (p_address.type.has_container_element_types()) {
					write_construct_typed_dictionary(p_address, p_address.type.get_container_element_type_or_variant(0), p_address.type.get_container_element_type_or_variant(1), Vector<GDScriptCodeGenerator::Address>());
				} else {
					write_construct(p_address, p_address.type.builtin_type, Vector<GDScriptCodeGenerator::Address>());
				}
				break;
			case Variant::ARRAY:
				if (p_address.type.has_container_element_type(0)) {
					write_construct_typed_array(p_address, p_address.type.get_container_element_type(0), Vector<GDScriptCodeGenerator::Address>());
				} else {
					write_construct(p_address, p_address.type.builtin_type, Vector<GDScriptCodeGenerator::Address>());
				}
				break;
			case Variant::NIL:
			case Variant::OBJECT:
				write_assign_null(p_address);
				break;
			default:
				write_construct(p_address, p_address.type.builtin_type, Vector<GDScriptCodeGenerator::Address>());
				break;
		}
	} else {
		write_assign_null(p_address);
	}

	if (p_address.mode == Address::LOCAL_VARIABLE) {
		dirty_locals.erase(p_address.address);
	}
}

// Returns `true` if the local has been reused and not cleaned up with `clear_address()`.
bool GDScriptJitCodeGenerator::is_local_dirty(const Address &p_address) const {
	print_debug("is_local_dirty");
	ERR_FAIL_COND_V(p_address.mode != Address::LOCAL_VARIABLE, false);
	return dirty_locals.has(p_address.address);
}

GDScriptJitCodeGenerator::~GDScriptJitCodeGenerator() {
	print_debug("~GDScriptJitCodeGenerator");
	if (!ended && function != nullptr) {
		memdelete(function);
	}
}
