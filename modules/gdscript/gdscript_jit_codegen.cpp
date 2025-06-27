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

//todo
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

uint32_t GDScriptJitCodeGenerator::add_parameter(const StringName &p_name, bool p_is_optional, const GDScriptDataType &p_type) {
	function->_argument_count++;
	function->argument_types.push_back(p_type);
	if (p_is_optional) {
		function->_default_arg_count++;
	}

	return add_local(p_name, p_type);
}

uint32_t GDScriptJitCodeGenerator::add_local(const StringName &p_name, const GDScriptDataType &p_type) {
	int stack_pos = locals.size() + GDScriptFunction::FIXED_ADDRESSES_MAX;
	locals.push_back(StackSlot(p_type.builtin_type, p_type.can_contain_object()));
	add_stack_identifier(p_name, stack_pos);
	return stack_pos;
}

uint32_t GDScriptJitCodeGenerator::add_local_constant(const StringName &p_name, const Variant &p_constant) {
	int index = add_or_get_constant(p_constant);
	local_constants[p_name] = index;
	return index;
}

uint32_t GDScriptJitCodeGenerator::add_or_get_constant(const Variant &p_constant) {
	return get_constant_pos(p_constant);
}

uint32_t GDScriptJitCodeGenerator::add_or_get_name(const StringName &p_name) {
	return get_name_map_pos(p_name);
}

uint32_t GDScriptJitCodeGenerator::add_temporary(const GDScriptDataType &p_type) {
	Variant::Type temp_type = Variant::NIL;
	if (p_type.has_type && p_type.kind == GDScriptDataType::BUILTIN) {
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
	if (function->_default_arg_count > 0) {
		append(GDScriptFunction::OPCODE_JUMP_TO_DEF_ARGUMENT);
		function->default_arguments.push_back(opcodes.size());
	}
}

void GDScriptJitCodeGenerator::end_parameters() {
	function->default_arguments.reverse();
}

void GDScriptJitCodeGenerator::write_start(GDScript *p_script, const StringName &p_function_name, bool p_static, Variant p_rpc_config, const GDScriptDataType &p_return_type) {
	start_time = OS::get_singleton()->get_ticks_usec();
	function = memnew(GDScriptFunction);

	function->name = p_function_name;
	function->_script = p_script;
	function->source = p_script->get_script_path();

#ifdef DEBUG_ENABLED
	function->func_cname = (String(function->source) + " - " + String(p_function_name)).utf8();
	function->_func_cname = function->func_cname.get_data();
#endif

	function->_static = p_static;
	function->return_type = p_return_type;
	function->rpc_config = p_rpc_config;
	function->_argument_count = 0;

	asmjit::FuncSignature sig;
	sig.setRet(asmjit::TypeId::kVoid);
	sig.addArg(asmjit::TypeId::kIntPtr);
	sig.addArg(asmjit::TypeId::kIntPtr);
	sig.addArg(asmjit::TypeId::kIntPtr);

	asmjit::FuncNode *func_node = cc.addFunc(sig);

	result_ptr = cc.newIntPtr("result_ptr");
	stack_ptr = cc.newIntPtr("stack_ptr");
	members_ptr = cc.newIntPtr("members_ptr");

	func_node->setArg(0, result_ptr);
	func_node->setArg(1, stack_ptr);
	func_node->setArg(2, members_ptr);
}

GDScriptFunction *GDScriptJitCodeGenerator::write_end() {
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
	patch_jit();

	cc.endFunc();

	if (constant_map.size()) {
		asmjit::Section *dataSection;
		JitRuntimeManager::get_singleton()->get_code().newSection(&dataSection, ".data", SIZE_MAX, asmjit::SectionFlags::kNone, 8);
		cc.section(dataSection);
		cc.bind(constants_ptr_label);
		for (const KeyValue<Variant, int> &K : constant_map) {
			cc.embed(&K.key, sizeof(Variant));
		}
	}

	cc.finalize();

	void *func_ptr = nullptr;
	asmjit::Error err = JitRuntimeManager::get_singleton()->get_runtime().add(&func_ptr, &JitRuntimeManager::get_singleton()->get_code());
	if (err) {
		print_error(asmjit::DebugUtils::errorAsString(err));
	}

	print_line(stringLogger.data());

	uint64_t end_time = OS::get_singleton()->get_ticks_usec();
	uint64_t elapsed_time = end_time - start_time;
	print_line("JIT compilation of function '" + String(function->name) + "' completed in " + String::num_int64(elapsed_time) + " us");

	function->jit_function = func_ptr;

	ended = true;
	return function;
}

#ifdef DEBUG_ENABLED
void GDScriptJitCodeGenerator::set_signature(const String &p_signature) {
	function->profile.signature = p_signature;
}
#endif

void GDScriptJitCodeGenerator::set_initial_line(int p_line) {
	function->_initial_line = p_line;
}

#define HAS_BUILTIN_TYPE(m_var) \
	(m_var.type.has_type && m_var.type.kind == GDScriptDataType::BUILTIN)

#define IS_BUILTIN_TYPE(m_var, m_type) \
	(m_var.type.has_type && m_var.type.kind == GDScriptDataType::BUILTIN && m_var.type.builtin_type == m_type && m_type != Variant::NIL)

void GDScriptJitCodeGenerator::write_type_adjust(const Address &p_target, Variant::Type p_new_type) {
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
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_VECTOR3);
			break;
		case Variant::VECTOR4I:
			append_opcode(GDScriptFunction::OPCODE_TYPE_ADJUST_VECTOR3I);
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
	if (HAS_BUILTIN_TYPE(p_left_operand)) {
		// Gather specific operator.
		Variant::ValidatedOperatorEvaluator op_func = Variant::get_validated_operator_evaluator(p_operator, p_left_operand.type.builtin_type, Variant::NIL);

		Gp left_ptr = get_variant_ptr(p_left_operand);
		Gp right_ptr = get_variant_ptr(Address());
		Gp op_ptr = get_variant_ptr(p_target);

		asmjit::InvokeNode *op_invoke;
		cc.invoke(&op_invoke, op_func, asmjit::FuncSignature::build<void, const Variant *, const Variant *, Variant *>());
		op_invoke->setArg(0, left_ptr);
		op_invoke->setArg(1, right_ptr);
		op_invoke->setArg(2, op_ptr);
		print_line("GDScriptJitCodeGenerator::write_unary_operator");

		append_opcode(GDScriptFunction::OPCODE_OPERATOR_VALIDATED);
		append(p_left_operand);
		append(Address());
		append(p_target);
		append(op_func);
#ifdef DEBUG_ENABLED
		add_debug_name(operator_names, get_operation_pos(op_func), Variant::get_operator_name(p_operator));
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
	bool valid = HAS_BUILTIN_TYPE(p_left_operand) && HAS_BUILTIN_TYPE(p_right_operand);

	// Avoid validated evaluator for modulo and division when operands are int or integer vector, since there's no check for division by zero.
	if (valid && (p_operator == Variant::OP_DIVIDE || p_operator == Variant::OP_MODULE)) {
		switch (p_left_operand.type.builtin_type) {
			case Variant::INT:
				valid = p_right_operand.type.builtin_type != Variant::INT;
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
		if (p_left_operand.type.builtin_type == Variant::VECTOR2 || p_right_operand.type.builtin_type == Variant::VECTOR2) {
			handle_vector2_operation(p_operator, p_left_operand, p_right_operand, p_target);
		} else if (p_left_operand.type.builtin_type == Variant::FLOAT || p_right_operand.type.builtin_type == Variant::FLOAT) {
			Vec left_val = cc.newXmmSd();
			Vec right_val = cc.newXmmSd();

			if (p_left_operand.type.builtin_type == Variant::INT && p_right_operand.type.builtin_type == Variant::FLOAT) {
				cc.cvtsi2sd(left_val, get_variant_mem(p_left_operand, OFFSET_INT));
				create_patch(p_left_operand, 1, OFFSET_INT);
				mov_from_variant_mem(right_val, p_right_operand, OFFSET_FLOAT);
			} else if (p_left_operand.type.builtin_type == Variant::FLOAT && p_right_operand.type.builtin_type == Variant::INT) {
				mov_from_variant_mem(left_val, p_left_operand, OFFSET_FLOAT);
				cc.cvtsi2sd(right_val, get_variant_mem(p_right_operand, OFFSET_INT));
				create_patch(p_right_operand, 1, OFFSET_INT);
			} else {
				mov_from_variant_mem(left_val, p_left_operand, OFFSET_FLOAT);
				mov_from_variant_mem(right_val, p_right_operand, OFFSET_FLOAT);
			}

			switch (p_operator) {
				case Variant::OP_ADD: {
					cc.addsd(left_val, right_val);
					mov_to_variant_mem(p_target, left_val, OFFSET_FLOAT);
				} break;
				case Variant::OP_SUBTRACT: {
					cc.subsd(left_val, right_val);
					mov_to_variant_mem(p_target, left_val, OFFSET_FLOAT);
				} break;
				case Variant::OP_MULTIPLY: {
					cc.mulsd(left_val, right_val);
					mov_to_variant_mem(p_target, left_val, OFFSET_FLOAT);
				} break;
				case Variant::OP_DIVIDE: {
					cc.divsd(left_val, right_val);
					mov_to_variant_mem(p_target, left_val, OFFSET_FLOAT);
				} break;
				case Variant::OP_EQUAL:
					gen_compare_float(left_val, right_val, p_target, Arch::CondCode::kE);
					break;
				case Variant::OP_NOT_EQUAL:
					gen_compare_float(left_val, right_val, p_target, Arch::CondCode::kNE);
					break;
				case Variant::OP_LESS:
					gen_compare_float(left_val, right_val, p_target, Arch::CondCode::kB);
					break;
				case Variant::OP_LESS_EQUAL:
					gen_compare_float(left_val, right_val, p_target, Arch::CondCode::kBE);
					break;
				case Variant::OP_GREATER:
					gen_compare_float(left_val, right_val, p_target, Arch::CondCode::kA);
					break;
				case Variant::OP_GREATER_EQUAL:
					gen_compare_float(left_val, right_val, p_target, Arch::CondCode::kAE);
					break;
				default: {
					print_error("Unsupported float operation ");
					return;
				}
			}
		} else if (p_left_operand.type.builtin_type == Variant::INT && p_right_operand.type.builtin_type == Variant::INT) {
			handle_int_operation(p_operator, p_left_operand, p_right_operand, p_target);
		} else {
			Gp left_ptr = get_variant_ptr(p_left_operand);
			Gp right_ptr = get_variant_ptr(p_right_operand);
			Gp op_ptr = get_variant_ptr(p_target);

			asmjit::InvokeNode *op_invoke;
			cc.invoke(&op_invoke, op_func, asmjit::FuncSignature::build<void, const Variant *, const Variant *, Variant *>());
			op_invoke->setArg(0, left_ptr);
			op_invoke->setArg(1, right_ptr);
			op_invoke->setArg(2, op_ptr);
		}

		print_line("GDScriptJitCodeGenerator::write_binary_operator");

		append_opcode(GDScriptFunction::OPCODE_OPERATOR_VALIDATED);
		append(p_left_operand);
		append(p_right_operand);
		append(p_target);
		append(op_func);
#ifdef DEBUG_ENABLED
		add_debug_name(operator_names, get_operation_pos(op_func), Variant::get_operator_name(p_operator));
#endif
		return;
	}

	Gp left_ptr = get_variant_ptr(p_left_operand);
	Gp right_ptr = get_variant_ptr(p_right_operand);
	Gp dst_ptr = get_variant_ptr(p_target);

	Gp operator_ptr = cc.newIntPtr("operator_ptr");
	cc.lea(operator_ptr, stackManager.alloc<Variant::Operator>());
	cc.mov(asmjit::x86::dword_ptr(operator_ptr), p_operator);

	Gp bool_ptr = cc.newIntPtr("bool_ptr");
	cc.lea(bool_ptr, stackManager.alloc<bool>());
	cc.mov(asmjit::x86::byte_ptr(bool_ptr), 1);

	asmjit::InvokeNode *evaluate_invoke;
	cc.invoke(&evaluate_invoke, static_cast<void (*)(const Variant::Operator &, const Variant &, const Variant &, Variant &, bool &)>(&Variant::evaluate),
			asmjit::FuncSignature::build<void, const Variant::Operator &, const Variant &, const Variant &, Variant &, bool &>());
	evaluate_invoke->setArg(0, operator_ptr);
	evaluate_invoke->setArg(1, left_ptr);
	evaluate_invoke->setArg(2, right_ptr);
	evaluate_invoke->setArg(3, dst_ptr);
	evaluate_invoke->setArg(4, bool_ptr);

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
	print_line("GDScriptJitCodeGenerator::write_and_left_operand");
	append_opcode(GDScriptFunction::OPCODE_JUMP_IF_NOT);
	append(p_left_operand);
	logic_op_jump_pos1.push_back(opcodes.size());
	append(0); // Jump target, will be patched.
}

void GDScriptJitCodeGenerator::write_and_right_operand(const Address &p_right_operand) {
	print_line("GDScriptJitCodeGenerator::write_and_right_operand");
	append_opcode(GDScriptFunction::OPCODE_JUMP_IF_NOT);
	append(p_right_operand);
	logic_op_jump_pos2.push_back(opcodes.size());
	append(0); // Jump target, will be patched.
}

void GDScriptJitCodeGenerator::write_end_and(const Address &p_target) {
	print_line("GDScriptJitCodeGenerator::write_end_and");
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
	print_line("GDScriptJitCodeGenerator::write_or_left_operand");
	append_opcode(GDScriptFunction::OPCODE_JUMP_IF);
	append(p_left_operand);
	logic_op_jump_pos1.push_back(opcodes.size());
	append(0); // Jump target, will be patched.
}

void GDScriptJitCodeGenerator::write_or_right_operand(const Address &p_right_operand) {
	print_line("GDScriptJitCodeGenerator::write_or_right_operand");
	append_opcode(GDScriptFunction::OPCODE_JUMP_IF);
	append(p_right_operand);
	logic_op_jump_pos2.push_back(opcodes.size());
	append(0); // Jump target, will be patched.
}

void GDScriptJitCodeGenerator::write_end_or(const Address &p_target) {
	print_line("GDScriptJitCodeGenerator::write_end_or");
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
	ternary_result.push_back(p_target);
}

void GDScriptJitCodeGenerator::write_ternary_condition(const Address &p_condition) {
	print_line("GDScriptJitCodeGenerator::write_ternary_condition");
	append_opcode(GDScriptFunction::OPCODE_JUMP_IF_NOT);
	append(p_condition);
	ternary_jump_fail_pos.push_back(opcodes.size());
	append(0); // Jump target, will be patched.
}

void GDScriptJitCodeGenerator::write_ternary_true_expr(const Address &p_expr) {
	print_line("GDScriptJitCodeGenerator::write_ternary_true_expr");
	//assign(p_expr, ternary_result.back()->get());
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
	print_line("GDScriptJitCodeGenerator::write_ternary_false_expr");
	//assign(p_expr, ternary_result.back()->get());
	append_opcode(GDScriptFunction::OPCODE_ASSIGN);
	append(ternary_result.back()->get());
	append(p_expr);
}

void GDScriptJitCodeGenerator::write_end_ternary() {
	print_line("GDScriptJitCodeGenerator::write_end_ternary");
	patch_jump(ternary_jump_skip_pos.back()->get());
	ternary_jump_skip_pos.pop_back();
	ternary_result.pop_back();
}

void GDScriptJitCodeGenerator::write_set(const Address &p_target, const Address &p_index, const Address &p_source) {
	if (HAS_BUILTIN_TYPE(p_target)) {
		if (IS_BUILTIN_TYPE(p_index, Variant::INT) && Variant::get_member_validated_indexed_setter(p_target.type.builtin_type) &&
				IS_BUILTIN_TYPE(p_source, Variant::get_indexed_element_type(p_target.type.builtin_type))) {
			// Use indexed setter instead.
			Variant::ValidatedIndexedSetter setter = Variant::get_member_validated_indexed_setter(p_target.type.builtin_type);

			Gp base_ptr = get_variant_ptr(p_target);
			Gp src_ptr = get_variant_ptr(p_source);
			Gp index_val = cc.newInt64("index_val");

			mov_from_variant_mem(index_val, p_index, OFFSET_INT);

			Mem bool_mem = stackManager.alloc<bool>();
			Gp bool_ptr = cc.newIntPtr("bool_ptr");
			cc.lea(bool_ptr, bool_mem);
			cc.mov(asmjit::x86::byte_ptr(bool_ptr), 1);

			asmjit::InvokeNode *setter_invoke;
			cc.invoke(&setter_invoke, setter, asmjit::FuncSignature::build<void, Variant *, int64_t, const Variant *, bool *>());
			setter_invoke->setArg(0, base_ptr);
			setter_invoke->setArg(1, src_ptr);
			setter_invoke->setArg(2, index_val);
			setter_invoke->setArg(3, bool_ptr);

			append_opcode(GDScriptFunction::OPCODE_SET_INDEXED_VALIDATED);
			append(p_target);
			append(p_index);
			append(p_source);
			append(setter);
			return;
		} else if (Variant::get_member_validated_keyed_setter(p_target.type.builtin_type)) {
			Variant::ValidatedKeyedSetter setter = Variant::get_member_validated_keyed_setter(p_target.type.builtin_type);
			append_opcode(GDScriptFunction::OPCODE_SET_KEYED_VALIDATED);
			append(p_target);
			append(p_index);
			append(p_source);
			append(setter);
			return;
		}
	}

	Gp base_ptr = get_variant_ptr(p_target);
	Gp src_ptr = get_variant_ptr(p_index);
	Gp index_val = get_variant_ptr(p_source);

	Mem bool_mem = stackManager.alloc<bool>();
	Gp bool_ptr = cc.newIntPtr("bool_ptr");
	cc.lea(bool_ptr, bool_mem);
	cc.mov(asmjit::x86::byte_ptr(bool_ptr), 1);

	asmjit::InvokeNode *set_invoke;
	cc.invoke(&set_invoke, &set_keyed,
			asmjit::FuncSignature::build<void, Variant *, const Variant *, const Variant *, bool *>());
	set_invoke->setArg(0, base_ptr);
	set_invoke->setArg(1, src_ptr);
	set_invoke->setArg(2, index_val);
	set_invoke->setArg(3, bool_ptr);

	append_opcode(GDScriptFunction::OPCODE_SET_KEYED);
	append(p_target);
	append(p_index);
	append(p_source);
}

void GDScriptJitCodeGenerator::write_get(const Address &p_target, const Address &p_index, const Address &p_source) {
	if (HAS_BUILTIN_TYPE(p_source)) {
		if (IS_BUILTIN_TYPE(p_index, Variant::INT) && Variant::get_member_validated_indexed_getter(p_source.type.builtin_type)) {
			// Use indexed getter instead.
			Variant::ValidatedIndexedGetter getter = Variant::get_member_validated_indexed_getter(p_source.type.builtin_type);

			Gp base_ptr = get_variant_ptr(p_source);
			Gp dst_ptr = get_variant_ptr(p_target);

			Gp index_val = cc.newInt64("index_val");
			mov_from_variant_mem(index_val, p_index, OFFSET_INT);

			Mem bool_mem = stackManager.alloc<bool>();
			Gp bool_ptr = cc.newIntPtr("bool_ptr");
			cc.lea(bool_ptr, bool_mem);
			cc.mov(asmjit::x86::byte_ptr(bool_ptr), 0);

			asmjit::InvokeNode *getter_invoke;
			cc.invoke(&getter_invoke, getter,
					asmjit::FuncSignature::build<void, const Variant *, int64_t, Variant *, bool *>());
			getter_invoke->setArg(0, base_ptr);
			getter_invoke->setArg(1, index_val);
			getter_invoke->setArg(2, dst_ptr);
			getter_invoke->setArg(3, bool_ptr);

			append_opcode(GDScriptFunction::OPCODE_GET_INDEXED_VALIDATED);
			append(p_source);
			append(p_index);
			append(p_target);
			append(getter);
			return;
		} else if (Variant::get_member_validated_keyed_getter(p_source.type.builtin_type)) {
			Variant::ValidatedKeyedGetter getter = Variant::get_member_validated_keyed_getter(p_source.type.builtin_type);
			append_opcode(GDScriptFunction::OPCODE_GET_KEYED_VALIDATED);
			append(p_source);
			append(p_index);
			append(p_target);
			append(getter);
			return;
		}
	}
	append_opcode(GDScriptFunction::OPCODE_GET_KEYED);
	append(p_source);
	append(p_index);
	append(p_target);
}

void GDScriptJitCodeGenerator::write_set_named(const Address &p_target, const StringName &p_name, const Address &p_source) {
	if (HAS_BUILTIN_TYPE(p_target) && Variant::get_member_validated_setter(p_target.type.builtin_type, p_name) &&
			IS_BUILTIN_TYPE(p_source, Variant::get_member_type(p_target.type.builtin_type, p_name))) {
		Variant::ValidatedSetter setter = Variant::get_member_validated_setter(p_target.type.builtin_type, p_name);

		if (p_source.type.builtin_type == Variant::FLOAT && p_target.type.builtin_type == Variant::VECTOR2) {
			if (p_name == "x") {
				Vec left_x = cc.newXmm("x");
				mov_from_variant_mem(left_x, p_source, OFFSET_FLOAT);
				cc.cvtsd2ss(left_x, left_x);
				cc.movss(get_variant_mem(p_target, OFFSET_VECTOR2_X), left_x);
				create_patch(p_target, 0, OFFSET_VECTOR2_X);
				return;
			} else if (p_name == "y") {
				Vec left_y = cc.newXmm("y");
				mov_from_variant_mem(left_y, p_source, OFFSET_FLOAT);
				cc.cvtsd2ss(left_y, left_y);
				cc.movss(get_variant_mem(p_target, OFFSET_VECTOR2_Y), left_y);
				create_patch(p_target, 0, OFFSET_VECTOR2_Y);
				return;
			}
		}

		Gp source_ptr = get_variant_ptr(p_source);
		Gp target_ptr = get_variant_ptr(p_target);

		asmjit::InvokeNode *setter_invoke;
		cc.invoke(&setter_invoke, setter,
				asmjit::FuncSignature::build<void, Variant *, const Variant *>());
		setter_invoke->setArg(0, target_ptr);
		setter_invoke->setArg(1, source_ptr);

		append_opcode(GDScriptFunction::OPCODE_SET_NAMED_VALIDATED);
		append(p_target);
		append(p_source);
		append(setter);
#ifdef DEBUG_ENABLED
		add_debug_name(setter_names, get_setter_pos(setter), p_name);
#endif
		return;
	}

	Gp base_ptr = get_variant_ptr(p_target);
	Gp source_ptr = get_variant_ptr(p_source);

	Mem bool_mem = stackManager.alloc<bool>();
	Gp bool_ptr = cc.newIntPtr("bool_ptr");
	cc.lea(bool_ptr, bool_mem);
	cc.mov(asmjit::x86::byte_ptr(bool_ptr), 1);

	asmjit::InvokeNode *set_invoke;
	cc.invoke(&set_invoke,
			static_cast<void (*)(Variant *, const StringName &, const Variant &, bool &)>([](Variant *base, const StringName &name, const Variant &value, bool &valid) {
				base->set_named(name, value, valid);
			}),
			asmjit::FuncSignature::build<void, Variant *, const StringName &, const Variant &, bool &>());
	set_invoke->setArg(0, base_ptr);
	set_invoke->setArg(2, source_ptr);
	set_invoke->setArg(3, bool_ptr);

	NamePatch name_patch;
	name_patch.arg_index = 1;
	name_patch.invoke_node = set_invoke;
	name_patch.name_index = get_name_map_pos(p_name);
	name_patches.push_back(name_patch);

	append_opcode(GDScriptFunction::OPCODE_SET_NAMED);
	append(p_target);
	append(p_source);
	append(p_name);
}

void GDScriptJitCodeGenerator::write_get_named(const Address &p_target, const StringName &p_name, const Address &p_source) {
	if (HAS_BUILTIN_TYPE(p_source) && Variant::get_member_validated_getter(p_source.type.builtin_type, p_name)) {
		Variant::ValidatedGetter getter = Variant::get_member_validated_getter(p_source.type.builtin_type, p_name);

		if (p_source.type.builtin_type == Variant::VECTOR2 && p_target.type.builtin_type == Variant::FLOAT) {
			if (p_name == "x") {
				Vec left_x = cc.newXmm("x");
				cc.movss(left_x, get_variant_mem(p_source, OFFSET_VECTOR2_X));
				create_patch(p_source, 1, OFFSET_VECTOR2_X);
				//mov_from_variant_mem(left_x, p_source, OFFSET_VECTOR2_X);
				cc.cvtss2sd(left_x, left_x);
				mov_to_variant_mem(p_target, left_x, OFFSET_FLOAT);

				return;
			} else if (p_name == "y") {
				Vec left_y = cc.newXmm("y");
				cc.movss(left_y, get_variant_mem(p_source, OFFSET_VECTOR2_Y));
				create_patch(p_source, 1, OFFSET_VECTOR2_Y);
				//mov_from_variant_mem(left_y, p_source, OFFSET_VECTOR2_Y);
				cc.cvtss2sd(left_y, left_y);
				mov_to_variant_mem(p_target, left_y, OFFSET_FLOAT);

				return;
			}
		}

		Gp source_ptr = get_variant_ptr(p_source);
		Gp target_ptr = get_variant_ptr(p_target);

		asmjit::InvokeNode *getter_invoke;
		cc.invoke(&getter_invoke, getter,
				asmjit::FuncSignature::build<void, const Variant *, Variant *>());
		getter_invoke->setArg(0, source_ptr);
		getter_invoke->setArg(1, target_ptr);

		append_opcode(GDScriptFunction::OPCODE_GET_NAMED_VALIDATED);
		append(p_source);
		append(p_target);
		append(getter);
#ifdef DEBUG_ENABLED
		add_debug_name(getter_names, get_getter_pos(getter), p_name);
#endif
		return;
	}

	Gp source_ptr = get_variant_ptr(p_source);
	Gp target_ptr = get_variant_ptr(p_target);

	Mem bool_mem = stackManager.alloc<bool>();
	Gp bool_ptr = cc.newIntPtr("bool_ptr");
	cc.lea(bool_ptr, bool_mem);
	cc.mov(asmjit::x86::byte_ptr(bool_ptr), 1);

	asmjit::InvokeNode *get_invoke;
	cc.invoke(&get_invoke,
			static_cast<void (*)(const Variant *, const StringName &, Variant *, bool &)>([](const Variant *base, const StringName &name, Variant *result, bool &valid) {
				*result = base->get_named(name, valid);
			}),
			asmjit::FuncSignature::build<void, const Variant *, const StringName &, Variant *, bool &>());
	get_invoke->setArg(0, source_ptr);
	get_invoke->setArg(2, target_ptr);
	get_invoke->setArg(3, bool_ptr);

	NamePatch name_patch;
	name_patch.arg_index = 1;
	name_patch.invoke_node = get_invoke;
	name_patch.name_index = get_name_map_pos(p_name);
	name_patches.push_back(name_patch);

	append_opcode(GDScriptFunction::OPCODE_GET_NAMED);
	append(p_source);
	append(p_target);
	append(p_name);
}

void GDScriptJitCodeGenerator::write_set_member(const Address &p_value, const StringName &p_name) {
	append_opcode(GDScriptFunction::OPCODE_SET_MEMBER);
	append(p_value);
	append(p_name);
}

void GDScriptJitCodeGenerator::write_get_member(const Address &p_target, const StringName &p_name) {
	append_opcode(GDScriptFunction::OPCODE_GET_MEMBER);
	append(p_target);
	append(p_name);
}

void GDScriptJitCodeGenerator::write_set_static_variable(const Address &p_value, const Address &p_class, int p_index) {
	append_opcode(GDScriptFunction::OPCODE_SET_STATIC_VARIABLE);
	append(p_value);
	append(p_class);
	append(p_index);
}

void GDScriptJitCodeGenerator::write_get_static_variable(const Address &p_target, const Address &p_class, int p_index) {
	append_opcode(GDScriptFunction::OPCODE_GET_STATIC_VARIABLE);
	append(p_target);
	append(p_class);
	append(p_index);
}

void GDScriptJitCodeGenerator::write_assign_with_conversion(const Address &p_target, const Address &p_source) {
	switch (p_target.type.kind) {
		case GDScriptDataType::BUILTIN: {
			if (p_target.type.builtin_type == Variant::ARRAY && p_target.type.has_container_element_type(0)) {
				const GDScriptDataType &element_type = p_target.type.get_container_element_type(0);
				append_opcode(GDScriptFunction::OPCODE_ASSIGN_TYPED_ARRAY);
				append(p_target);
				append(p_source);
				append(get_constant_pos(element_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
				append(element_type.builtin_type);
				append(element_type.native_type);
			} else if (p_target.type.builtin_type == Variant::DICTIONARY && p_target.type.has_container_element_types()) {
				const GDScriptDataType &key_type = p_target.type.get_container_element_type_or_variant(0);
				const GDScriptDataType &value_type = p_target.type.get_container_element_type_or_variant(1);
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
			append_opcode(GDScriptFunction::OPCODE_ASSIGN_TYPED_NATIVE);
			append(p_target);
			append(p_source);
			append(class_idx);
		} break;
		case GDScriptDataType::SCRIPT:
		case GDScriptDataType::GDSCRIPT: {
			Variant script = p_target.type.script_type;
			int idx = get_constant_pos(script) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS);

			append_opcode(GDScriptFunction::OPCODE_ASSIGN_TYPED_SCRIPT);
			append(p_target);
			append(p_source);
			append(idx);
		} break;
		default: {
			ERR_PRINT("Compiler bug: unresolved assign.");
			assign(p_source, p_target);

			// Shouldn't get here, but fail-safe to a regular assignment
			append_opcode(GDScriptFunction::OPCODE_ASSIGN);
			append(p_target);
			append(p_source);
		}
	}
}

void GDScriptJitCodeGenerator::write_assign(const Address &p_target, const Address &p_source) {
	if (p_target.type.kind == GDScriptDataType::BUILTIN && p_target.type.builtin_type == Variant::ARRAY && p_target.type.has_container_element_type(0)) {
		const GDScriptDataType &element_type = p_target.type.get_container_element_type(0);
		append_opcode(GDScriptFunction::OPCODE_ASSIGN_TYPED_ARRAY);
		append(p_target);
		append(p_source);
		append(get_constant_pos(element_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
		append(element_type.builtin_type);
		append(element_type.native_type);
	} else if (p_target.type.kind == GDScriptDataType::BUILTIN && p_target.type.builtin_type == Variant::DICTIONARY && p_target.type.has_container_element_types()) {
		const GDScriptDataType &key_type = p_target.type.get_container_element_type_or_variant(0);
		const GDScriptDataType &value_type = p_target.type.get_container_element_type_or_variant(1);
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
		append_opcode(GDScriptFunction::OPCODE_ASSIGN_TYPED_BUILTIN);
		append(p_target);
		append(p_source);
		append(p_target.type.builtin_type);
	} else {
		assign(p_source, p_target);
		append_opcode(GDScriptFunction::OPCODE_ASSIGN);
		append(p_target);
		append(p_source);
	}
}

void GDScriptJitCodeGenerator::write_assign_null(const Address &p_target) {
	assign_null(p_target);
	append_opcode(GDScriptFunction::OPCODE_ASSIGN_NULL);
	append(p_target);
}

void GDScriptJitCodeGenerator::write_assign_true(const Address &p_target) {
	assign_bool(p_target, true);
	append_opcode(GDScriptFunction::OPCODE_ASSIGN_TRUE);
	append(p_target);
}

void GDScriptJitCodeGenerator::write_assign_false(const Address &p_target) {
	assign_bool(p_target, false);
	append_opcode(GDScriptFunction::OPCODE_ASSIGN_FALSE);
	append(p_target);
}

void GDScriptJitCodeGenerator::write_assign_default_parameter(const Address &p_dst, const Address &p_src, bool p_use_conversion) {
	if (p_use_conversion) {
		write_assign_with_conversion(p_dst, p_src);
	} else {
		write_assign(p_dst, p_src);
	}
	function->default_arguments.push_back(opcodes.size());
}

void GDScriptJitCodeGenerator::write_store_global(const Address &p_dst, int p_global_index) {
	append_opcode(GDScriptFunction::OPCODE_STORE_GLOBAL);
	append(p_dst);
	append(p_global_index);
}

void GDScriptJitCodeGenerator::write_store_named_global(const Address &p_dst, const StringName &p_global) {
	append_opcode(GDScriptFunction::OPCODE_STORE_NAMED_GLOBAL);
	append(p_dst);
	append(p_global);
}

void GDScriptJitCodeGenerator::write_cast(const Address &p_target, const Address &p_source, const GDScriptDataType &p_type) {
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

			Gp src_ptr = get_variant_ptr(p_source);
			Gp dst_ptr = get_variant_ptr(p_target);
			Gp script_ptr = get_variant_ptr(Address(Address::AddressMode::CONSTANT, get_constant_pos(script)));

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
	if (p_target.mode == Address::NIL) {
		GDScriptDataType type;
		if (p_type != Variant::NIL) {
			type.has_type = true;
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
	append_opcode_and_argcount(GDScriptFunction::OPCODE_CALL_GDSCRIPT_UTILITY, 1 + p_arguments.size());
	GDScriptUtilityFunctions::FunctionPtr gds_function = GDScriptUtilityFunctions::get_function(p_function);
	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	CallTarget ct = get_call_target(p_target);
	append(ct.target);
	append(p_arguments.size());
	append(gds_function);
	ct.cleanup();
#ifdef DEBUG_ENABLED
	add_debug_name(gds_utilities_names, get_gds_utility_pos(gds_function), p_function);
#endif
}

void GDScriptJitCodeGenerator::write_call_utility(const Address &p_target, const StringName &p_function, const Vector<Address> &p_arguments) {
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

		Gp args_array = prepare_args_array(p_arguments);
		Gp dst_ptr = get_variant_ptr(ct.target);

		asmjit::InvokeNode *utility_invoke;
		cc.invoke(&utility_invoke, Variant::get_validated_utility_function(p_function), asmjit::FuncSignature::build<void, Variant *, const Variant **, int>());
		utility_invoke->setArg(0, dst_ptr);
		utility_invoke->setArg(1, args_array);
		utility_invoke->setArg(2, p_arguments.size());

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
		append_opcode_and_argcount(GDScriptFunction::OPCODE_CALL_UTILITY, 1 + p_arguments.size());
		for (int i = 0; i < p_arguments.size(); i++) {
			append(p_arguments[i]);
		}
		CallTarget ct = get_call_target(p_target);

		Gp args_array = prepare_args_array(p_arguments);
		Gp dst_ptr = get_variant_ptr(ct.target);

		Gp call_error_ptr = get_call_error();

		asmjit::InvokeNode *utility_invoke;
		cc.invoke(&utility_invoke, &Variant::call_utility_function, asmjit::FuncSignature::build<void, StringName &, Variant *, const Variant **, int, Callable::CallError &>());
		utility_invoke->setArg(1, dst_ptr);
		utility_invoke->setArg(2, args_array);
		utility_invoke->setArg(3, p_arguments.size());
		utility_invoke->setArg(4, call_error_ptr);

		NamePatch name_patch;
		name_patch.arg_index = 0;
		name_patch.invoke_node = utility_invoke;
		name_patch.name_index = get_name_map_pos(p_function);
		name_patches.push_back(name_patch);

		append(ct.target);
		append(p_arguments.size());
		append(p_function);
		ct.cleanup();
	}
}

void GDScriptJitCodeGenerator::write_call_builtin_type(const Address &p_target, const Address &p_base, Variant::Type p_type, const StringName &p_method, bool p_is_static, const Vector<Address> &p_arguments) {
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
			append_opcode_and_argcount(GDScriptFunction::OPCODE_CALL_BUILTIN_STATIC, p_arguments.size() + 1);
			for (int i = 0; i < p_arguments.size(); i++) {
				append(p_arguments[i]);
			}
			CallTarget ct = get_call_target(p_target);
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

	Gp base_ptr = get_variant_ptr(p_base);
	Gp dst_ptr = get_variant_ptr(ct.target);
	Gp args_array = prepare_args_array(p_arguments);

	asmjit::InvokeNode *call_invoke;
	cc.invoke(&call_invoke, Variant::get_validated_builtin_method(p_type, p_method),
			asmjit::FuncSignature::build<void, Variant *, const Variant **, int, Variant *>());
	call_invoke->setArg(0, base_ptr);
	call_invoke->setArg(1, args_array);
	call_invoke->setArg(2, p_arguments.size());
	call_invoke->setArg(3, dst_ptr);

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
	write_call_builtin_type(p_target, p_base, p_type, p_method, false, p_arguments);
}

void GDScriptJitCodeGenerator::write_call_builtin_type_static(const Address &p_target, Variant::Type p_type, const StringName &p_method, const Vector<Address> &p_arguments) {
	write_call_builtin_type(p_target, Address(), p_type, p_method, true, p_arguments);
}

void GDScriptJitCodeGenerator::write_call_native_static(const Address &p_target, const StringName &p_class, const StringName &p_method, const Vector<Address> &p_arguments) {
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
	append_opcode_and_argcount(p_target.mode == Address::NIL ? GDScriptFunction::OPCODE_CALL_METHOD_BIND : GDScriptFunction::OPCODE_CALL_METHOD_BIND_RET, 2 + p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	CallTarget ct = get_call_target(p_target);

	Gp base_ptr = get_variant_ptr(p_base);
	Gp dst_ptr = get_variant_ptr(ct.target);
	Gp call_error_ptr = get_call_error();

	Gp base_obj = cc.newIntPtr("base_obj");
	cc.mov(base_obj, Arch::ptr(base_ptr, offsetof(Variant, _data) + offsetof(Variant::ObjData, obj)));

	Gp args_array = prepare_args_array(p_arguments);
	asmjit::InvokeNode *call_invoke;

	if (p_target.mode == Address::NIL) {
		cc.invoke(&call_invoke,
				static_cast<void (*)(MethodBind *, Object *, const Variant **, int, Callable::CallError &)>(
						[](MethodBind *method_p, Object *obj, const Variant **args, int argcount, Callable::CallError &err) -> void {
							method_p->call(obj, args, argcount, err);
						}),
				asmjit::FuncSignature::build<void, MethodBind *, Object *, const Variant **, int, Callable::CallError &>());
	} else {
		cc.invoke(&call_invoke,
				static_cast<void (*)(MethodBind *, Object *, const Variant **, int, Callable::CallError &, Variant *dst)>(
						[](MethodBind *method_p, Object *obj, const Variant **args, int argcount, Callable::CallError &err, Variant *dst) -> void {
							Variant temp_ret = method_p->call(obj, args, argcount, err);
							*dst = temp_ret;
						}),
				asmjit::FuncSignature::build<void, MethodBind *, Object *, const Variant **, int, Callable::CallError &, Variant *>());

		call_invoke->setArg(5, dst_ptr);
	}

	call_invoke->setArg(0, p_method);
	call_invoke->setArg(1, base_obj);
	call_invoke->setArg(2, args_array);
	call_invoke->setArg(3, p_arguments.size());
	call_invoke->setArg(4, call_error_ptr);

	append(p_base);
	append(ct.target);
	append(p_arguments.size());
	append(p_method);
	ct.cleanup();
}

void GDScriptJitCodeGenerator::write_call_method_bind_validated(const Address &p_target, const Address &p_base, MethodBind *p_method, const Vector<Address> &p_arguments) {
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

	Gp base_ptr = get_variant_ptr(p_base);
	Gp dst_ptr = get_variant_ptr(ct.target);

	Gp base_obj = cc.newIntPtr("base_obj");
	cc.mov(base_obj, Arch::ptr(base_ptr, offsetof(Variant, _data) + offsetof(Variant::ObjData, obj)));

	Gp args_array = prepare_args_array(p_arguments);

	asmjit::InvokeNode *method_invoke;
	if (code == GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN) {
		cc.invoke(&method_invoke,
				static_cast<void (*)(MethodBind *, Object *, const Variant **, Variant *)>(
						[](MethodBind *method_p, Object *obj, const Variant **args, Variant *ret) {
							method_p->validated_call(obj, args, ret);
						}),
				asmjit::FuncSignature::build<void, MethodBind *, Object *, const Variant **, Variant *>());
	} else {
		cc.invoke(&method_invoke,
				static_cast<void (*)(MethodBind *, Object *, const Variant **, Variant *)>(
						[](MethodBind *method_p, Object *obj, const Variant **args, Variant *ret) {
							VariantInternal::initialize(ret, Variant::NIL);
							method_p->validated_call(obj, args, nullptr);
						}),
				asmjit::FuncSignature::build<void, MethodBind *, Object *, const Variant **, Variant *>());
	}
	method_invoke->setArg(0, p_method);
	method_invoke->setArg(1, base_obj);
	method_invoke->setArg(2, args_array);
	method_invoke->setArg(3, dst_ptr);

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
	append_opcode_and_argcount(p_target.mode == Address::NIL ? GDScriptFunction::OPCODE_CALL : GDScriptFunction::OPCODE_CALL_RETURN, 2 + p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	append(GDScriptFunction::ADDR_TYPE_STACK << GDScriptFunction::ADDR_BITS);
	CallTarget ct = get_call_target(p_target);

	Gp base_ptr = get_variant_ptr(Address(Address::AddressMode::SELF));
	Gp dst_ptr = get_variant_ptr(ct.target);

	Gp args_array = prepare_args_array(p_arguments);
	Gp call_error_ptr = get_call_error();

	asmjit::InvokeNode *call_invoke;
	cc.invoke(&call_invoke, &call_variant_method,
			asmjit::FuncSignature::build<void, const Variant &, const StringName &, const Variant **, int, Variant &, Callable::CallError &>());

	call_invoke->setArg(0, base_ptr);
	call_invoke->setArg(2, args_array);
	call_invoke->setArg(3, p_arguments.size());
	call_invoke->setArg(4, dst_ptr);
	call_invoke->setArg(5, call_error_ptr);

	NamePatch name_patch;
	name_patch.arg_index = 1;
	name_patch.invoke_node = call_invoke;
	name_patch.name_index = get_name_map_pos(p_function_name);
	name_patches.push_back(name_patch);

	append(ct.target);
	append(p_arguments.size());
	append(p_function_name);
	ct.cleanup();
}

void GDScriptJitCodeGenerator::write_call_self_async(const Address &p_target, const StringName &p_function_name, const Vector<Address> &p_arguments) {
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
	append_opcode_and_argcount(GDScriptFunction::OPCODE_CONSTRUCT_ARRAY, 1 + p_arguments.size());
	for (int i = 0; i < p_arguments.size(); i++) {
		append(p_arguments[i]);
	}
	CallTarget ct = get_call_target(p_target);

	Gp args_array = prepare_args_array(p_arguments);
	Gp dst_ptr = get_variant_ptr(ct.target);

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
	construct_invoke->setArg(2, p_arguments.size());

	append(ct.target);
	append(p_arguments.size());
	ct.cleanup();
}

void GDScriptJitCodeGenerator::write_construct_typed_array(const Address &p_target, const GDScriptDataType &p_element_type, const Vector<Address> &p_arguments) {
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
	append_opcode(GDScriptFunction::OPCODE_AWAIT);
	append(p_operand);
	append_opcode(GDScriptFunction::OPCODE_AWAIT_RESUME);
	append(p_target);
}

void GDScriptJitCodeGenerator::write_if(const Address &p_condition) {
	print_line("if");
	IfContext if_context;
	if_context.if_false_label = cc.newLabel();
	if_context.end_label = cc.newLabel();
	if_contexts.push_back(if_context);

	switch (p_condition.type.builtin_type) {
		case Variant::INT: {
			Gp temp = cc.newInt64();
			mov_from_variant_mem(temp, p_condition, OFFSET_INT);
			cc.test(temp, temp);
		} break;
		case Variant::BOOL: {
			Gp temp = cc.newInt8();
			mov_from_variant_mem(temp, p_condition, OFFSET_BOOL);
			cc.test(temp, temp);
		} break;
		default: {
			Gp condition_ptr = get_variant_ptr(p_condition);
			Gp bool_result = cc.newInt8("bool_result");
			asmjit::InvokeNode *booleanize_invoke;
			cc.invoke(&booleanize_invoke,
					static_cast<bool (*)(const Variant *)>([](const Variant *v) -> bool {
						return v->booleanize();
					}),
					asmjit::FuncSignature::build<bool, const Variant *>());
			booleanize_invoke->setArg(0, condition_ptr);
			booleanize_invoke->setRet(0, bool_result);

			cc.test(bool_result, bool_result);
		}
	}
	cc.jz(if_context.if_false_label);

	append_opcode(GDScriptFunction::OPCODE_JUMP_IF_NOT);
	append(p_condition);
	if_jmp_addrs.push_back(opcodes.size());
	append(0); // Jump destination, will be patched.
}

void GDScriptJitCodeGenerator::write_else() {
	print_line("else");
	IfContext &current_if = if_contexts.back()->get();
	current_if.has_else = true;

	cc.jmp(current_if.end_label);

	cc.bind(current_if.if_false_label);

	append_opcode(GDScriptFunction::OPCODE_JUMP); // Jump from true if block;
	int else_jmp_addr = opcodes.size();
	append(0); // Jump destination, will be patched.

	patch_jump(if_jmp_addrs.back()->get());
	if_jmp_addrs.pop_back();
	if_jmp_addrs.push_back(else_jmp_addr);
}

void GDScriptJitCodeGenerator::write_endif() {
	print_line("endif");
	IfContext current_if = if_contexts.back()->get();
	if_contexts.pop_back();

	if (current_if.has_else) {
		cc.bind(current_if.end_label);
	} else {
		cc.bind(current_if.if_false_label);
		//cc.bind(current_if.end_label);
	}

	patch_jump(if_jmp_addrs.back()->get());
	if_jmp_addrs.pop_back();
}

void GDScriptJitCodeGenerator::write_jump_if_shared(const Address &p_value) {
	append_opcode(GDScriptFunction::OPCODE_JUMP_IF_SHARED);
	append(p_value);
	if_jmp_addrs.push_back(opcodes.size());
	append(0); // Jump destination, will be patched.
}

void GDScriptJitCodeGenerator::write_end_jump_if_shared() {
	patch_jump(if_jmp_addrs.back()->get());
	if_jmp_addrs.pop_back();
}

void GDScriptJitCodeGenerator::start_for(const GDScriptDataType &p_iterator_type, const GDScriptDataType &p_list_type, bool p_is_range) {
	Address counter(Address::LOCAL_VARIABLE, add_local("@counter_pos", p_iterator_type), p_iterator_type);

	// Store state.
	for_counter_variables.push_back(counter);

	if (p_is_range) {
		GDScriptDataType int_type;
		int_type.has_type = true;
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
	const Address &container = for_container_variables.back()->get();
	assign(p_list, container);
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
		write_assign(range_from, p_from);
	} else {
		write_assign_with_conversion(range_from, p_from);
	}
	if (range_to.type == p_to.type) {
		write_assign(range_to, p_to);
	} else {
		write_assign_with_conversion(range_to, p_to);
	}
	if (range_step.type == p_step.type) {
		write_assign(range_step, p_step);
	} else {
		write_assign_with_conversion(range_step, p_step);
	}
}

void GDScriptJitCodeGenerator::write_for(const Address &p_variable, bool p_use_conversion, bool p_is_range) {
	const Address &counter = for_counter_variables.back()->get();
	const Address &container = p_is_range ? Address() : for_container_variables.back()->get();
	const Address &range_from = p_is_range ? for_range_from_variables.back()->get() : Address();
	const Address &range_to = p_is_range ? for_range_to_variables.back()->get() : Address();
	const Address &range_step = p_is_range ? for_range_step_variables.back()->get() : Address();

	current_breaks_to_patch.push_back(List<int>());

	GDScriptFunction::Opcode begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN;
	GDScriptFunction::Opcode iterate_opcode = GDScriptFunction::OPCODE_ITERATE;
	Address temp;
	if (p_use_conversion) {
		temp = Address(Address::LOCAL_VARIABLE, add_local("@iterator_temp", GDScriptDataType()));
	}

	if (p_is_range) {
		begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_RANGE;
		iterate_opcode = GDScriptFunction::OPCODE_ITERATE_RANGE;

		iterate_range(range_from, range_to, range_step, counter, p_use_conversion, temp, p_variable);

	} else if (container.type.has_type) {
		if (container.type.kind == GDScriptDataType::BUILTIN) {
			switch (container.type.builtin_type) {
				case Variant::INT:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_INT;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_INT;
					break;
				case Variant::FLOAT:
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_FLOAT;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_FLOAT;
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
				case Variant::ARRAY: {
					begin_opcode = GDScriptFunction::OPCODE_ITERATE_BEGIN_ARRAY;
					iterate_opcode = GDScriptFunction::OPCODE_ITERATE_ARRAY;

					asmjit::Label body = cc.newLabel();

					LoopContext loop_context;
					loop_context.loop = cc.newLabel();
					loop_context.exit = cc.newLabel();
					for_jmp_labels.push_back(loop_context);

					Gp container_ptr = get_variant_ptr(container);
					Gp counter_ptr = get_variant_ptr(counter);
					Gp iterator_ptr = get_variant_ptr(p_use_conversion ? temp : p_variable);

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
					cc.jz(loop_context.exit);

					asmjit::InvokeNode *get_first_invoke;
					cc.invoke(&get_first_invoke,
							static_cast<void (*)(const Array *, Variant *)>([](const Array *arr, Variant *dst) -> void {
								*dst = arr->operator[](0);
							}),
							asmjit::FuncSignature::build<void, const Array *, Variant *>());
					get_first_invoke->setArg(0, array_ptr);
					get_first_invoke->setArg(1, iterator_ptr);

					cc.jmp(body);
					cc.bind(loop_context.loop);

					Gp idx = cc.newInt64("index");
					cc.mov(idx, asmjit::x86::qword_ptr(counter_ptr, OFFSET_INT));
					cc.add(idx, 1);
					cc.mov(asmjit::x86::qword_ptr(counter_ptr, OFFSET_INT), idx);

					asmjit::InvokeNode *size_update_invoke;
					cc.invoke(&size_update_invoke,
							static_cast<int (*)(const Array *)>([](const Array *arr) -> int {
								return arr->size();
							}),
							asmjit::FuncSignature::build<int, const Array *>());
					size_update_invoke->setArg(0, array_ptr);
					size_update_invoke->setRet(0, array_size);

					cc.cmp(idx.r32(), array_size);
					cc.jae(loop_context.exit);

					asmjit::InvokeNode *get_invoke;
					cc.invoke(&get_invoke,
							static_cast<void (*)(const Array *, int, Variant *)>([](const Array *arr, int index, Variant *dst) -> void {
								*dst = arr->operator[](index);
							}),
							asmjit::FuncSignature::build<void, const Array *, int, Variant *>());
					get_invoke->setArg(0, array_ptr);
					get_invoke->setArg(1, idx);
					get_invoke->setArg(2, iterator_ptr);

					cc.bind(body);
				} break;
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

void GDScriptJitCodeGenerator::iterate_range(const Address &range_from, const Address &range_to, const Address &range_step, const Address &counter, bool p_use_conversion, Address &temp, const Address &p_variable) {
	asmjit::Label body = cc.newLabel();

	LoopContext loop_context;
	loop_context.loop = cc.newLabel();
	loop_context.exit = cc.newLabel();
	for_jmp_labels.push_back(loop_context);

	Gp from = cc.newInt64("from");
	Gp to = cc.newInt64("to");
	Gp step = cc.newInt64("step");

	mov_from_variant_mem(from, range_from, OFFSET_INT);
	mov_from_variant_mem(to, range_to, OFFSET_INT);
	mov_from_variant_mem(step, range_step, OFFSET_INT);

	mov_to_variant_type_mem(counter, Variant::INT);
	mov_to_variant_mem(counter, from, OFFSET_INT);

	Gp condition = cc.newInt64("condition");
	cc.mov(condition, to);
	cc.sub(condition, from);
	cc.imul(condition, step);

	cc.cmp(condition, 0);
	cc.jle(loop_context.exit);

	mov_to_variant_type_mem(p_use_conversion ? temp : p_variable, Variant::INT);
	mov_to_variant_mem(p_use_conversion ? temp : p_variable, from, OFFSET_INT);

	cc.jmp(body);

	// ITERATE
	cc.bind(loop_context.loop);

	Gp count = cc.newInt64("count");
	mov_from_variant_mem(count, counter, OFFSET_INT);
	cc.add(count, step);
	mov_to_variant_mem(counter, count, OFFSET_INT);

	cc.mov(condition, count);
	cc.sub(condition, to);
	cc.imul(condition, step);

	cc.test(condition, condition);
	cc.jge(loop_context.exit);

	mov_to_variant_mem(p_use_conversion ? temp : p_variable, count, OFFSET_INT);
	cc.bind(body);
}

void GDScriptJitCodeGenerator::write_endfor(bool p_is_range) {
	auto loop = for_jmp_labels.back()->get();

	cc.jmp(loop.loop);
	cc.bind(loop.exit);
	for_jmp_labels.pop_back();

	// Jump back to loop check.
	append_opcode(GDScriptFunction::OPCODE_JUMP);
	append(continue_addrs.back()->get());
	continue_addrs.pop_back();

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
	current_breaks_to_patch.push_back(List<int>());
	continue_addrs.push_back(opcodes.size());
}

void GDScriptJitCodeGenerator::write_while(const Address &p_condition) {
	// Condition check.
	print_line("GDScriptJitCodeGenerator::write_while");
	append_opcode(GDScriptFunction::OPCODE_JUMP_IF_NOT);
	append(p_condition);
	while_jmp_addrs.push_back(opcodes.size());
	append(0); // End of loop address, will be patched.
}

void GDScriptJitCodeGenerator::write_endwhile() {
	// Jump back to loop check.
	print_line("GDScriptJitCodeGenerator::write_endwhile");
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
}

void GDScriptJitCodeGenerator::write_break() {
	print_line("GDScriptJitCodeGenerator::write_break");
	auto &loop = for_jmp_labels.back()->get();

	cc.jmp(loop.exit);
	append_opcode(GDScriptFunction::OPCODE_JUMP);
	current_breaks_to_patch.back()->get().push_back(opcodes.size());
	append(0);
}

void GDScriptJitCodeGenerator::write_continue() {
	print_line("GDScriptJitCodeGenerator::write_continue");
	auto &loop = for_jmp_labels.back()->get();

	cc.jmp(loop.loop);
	append_opcode(GDScriptFunction::OPCODE_JUMP);
	append(continue_addrs.back()->get());
}

void GDScriptJitCodeGenerator::write_breakpoint() {
	append_opcode(GDScriptFunction::OPCODE_BREAKPOINT);
}

void GDScriptJitCodeGenerator::write_newline(int p_line) {
	if (GDScriptLanguage::get_singleton()->should_track_call_stack()) {
		// Add newline for debugger and stack tracking if enabled in the project settings.
		append_opcode(GDScriptFunction::OPCODE_LINE);
		append(p_line);
		current_line = p_line;
	}
}

void GDScriptJitCodeGenerator::write_return(const Address &p_return_value) {
	if (!function->return_type.has_type || p_return_value.type.has_type) {
		// Either the function is untyped or the return value is also typed.

		// If this is a typed function, then we need to check for potential conversions.
		if (function->return_type.has_type) {
			if (function->return_type.kind == GDScriptDataType::BUILTIN && function->return_type.builtin_type == Variant::ARRAY && function->return_type.has_container_element_type(0)) {
				// Typed array.
				const GDScriptDataType &element_type = function->return_type.get_container_element_type(0);
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
				append_opcode(GDScriptFunction::OPCODE_RETURN_TYPED_DICTIONARY);
				append(p_return_value);
				append(get_constant_pos(key_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
				append(get_constant_pos(value_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
				append(key_type.builtin_type);
				append(key_type.native_type);
				append(value_type.builtin_type);
				append(value_type.native_type);
			} else if (function->return_type.kind == GDScriptDataType::BUILTIN && p_return_value.type.kind == GDScriptDataType::BUILTIN && function->return_type.builtin_type != p_return_value.type.builtin_type) {
				Gp src_ptr = get_variant_ptr(p_return_value);
				Gp args_array = cc.newIntPtr("cast_args_array");
				cc.lea(args_array, stackManager.allocArg(1));
				cc.mov(Arch::ptr(args_array, 0), src_ptr);

				Gp call_error_ptr = get_call_error();
				cc.mov(asmjit::x86::dword_ptr(result_ptr, 0), (int)function->return_type.builtin_type);

				asmjit::InvokeNode *construct_invoke;
				cc.invoke(&construct_invoke, &Variant::construct,
						asmjit::FuncSignature::build<void, Variant::Type, Variant &, const Variant **, int, Callable::CallError &>());
				construct_invoke->setArg(0, function->return_type.builtin_type);
				construct_invoke->setArg(1, result_ptr);
				construct_invoke->setArg(2, args_array);
				construct_invoke->setArg(3, 1);
				construct_invoke->setArg(4, call_error_ptr);
				cc.ret();

				append_opcode(GDScriptFunction::OPCODE_RETURN_TYPED_BUILTIN);
				append(p_return_value);
				append(function->return_type.builtin_type);
			} else if (function->return_type.kind == GDScriptDataType::BUILTIN && p_return_value.type.kind == GDScriptDataType::BUILTIN && p_return_value.type.builtin_type == Variant::INT) {
				Gp tmp = cc.newInt64();
				mov_from_variant_mem(tmp, p_return_value, OFFSET_INT);
				cc.mov(Arch::dword_ptr(result_ptr), (int)Variant::INT);
				cc.mov(Arch::qword_ptr(result_ptr, OFFSET_INT), tmp);
				cc.ret();
			} else {
				Gp src_ptr = get_variant_ptr(p_return_value);
				copy_variant(result_ptr, src_ptr);
				cc.ret();

				append_opcode(GDScriptFunction::OPCODE_RETURN);
				append(p_return_value);
			}
		} else {
			Gp src_ptr = get_variant_ptr(p_return_value);
			copy_variant(result_ptr, src_ptr);
			cc.ret();

			append_opcode(GDScriptFunction::OPCODE_RETURN);
			append(p_return_value);
		}
	} else {
		switch (function->return_type.kind) {
			case GDScriptDataType::BUILTIN: {
				if (function->return_type.builtin_type == Variant::ARRAY && function->return_type.has_container_element_type(0)) {
					const GDScriptDataType &element_type = function->return_type.get_container_element_type(0);
					append_opcode(GDScriptFunction::OPCODE_RETURN_TYPED_ARRAY);
					append(p_return_value);
					append(get_constant_pos(element_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
					append(element_type.builtin_type);
					append(element_type.native_type);
				} else if (function->return_type.builtin_type == Variant::DICTIONARY && function->return_type.has_container_element_types()) {
					const GDScriptDataType &key_type = function->return_type.get_container_element_type_or_variant(0);
					const GDScriptDataType &value_type = function->return_type.get_container_element_type_or_variant(1);
					append_opcode(GDScriptFunction::OPCODE_RETURN_TYPED_DICTIONARY);
					append(p_return_value);
					append(get_constant_pos(key_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
					append(get_constant_pos(value_type.script_type) | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS));
					append(key_type.builtin_type);
					append(key_type.native_type);
					append(value_type.builtin_type);
					append(value_type.native_type);
				} else {
					Gp src_ptr = get_variant_ptr(p_return_value);
					Gp args_array = cc.newIntPtr("cast_args_array");
					cc.lea(args_array, stackManager.allocArg(1));
					cc.mov(Arch::ptr(args_array, 0), src_ptr);

					Gp call_error_ptr = get_call_error();
					cc.mov(asmjit::x86::dword_ptr(result_ptr, 0), (int)function->return_type.builtin_type);

					asmjit::InvokeNode *construct_invoke;
					cc.invoke(&construct_invoke, &Variant::construct,
							asmjit::FuncSignature::build<void, Variant::Type, Variant &, const Variant **, int, Callable::CallError &>());
					construct_invoke->setArg(0, function->return_type.builtin_type);
					construct_invoke->setArg(1, result_ptr);
					construct_invoke->setArg(2, args_array);
					construct_invoke->setArg(3, 1);
					construct_invoke->setArg(4, call_error_ptr);
					cc.ret();
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
				append_opcode(GDScriptFunction::OPCODE_RETURN);
				append(p_return_value);
			} break;
		}
	}
}

Gp GDScriptJitCodeGenerator::get_call_error() {
	Gp call_error_ptr = cc.newIntPtr("call_error_ptr");
	cc.lea(call_error_ptr, stackManager.alloc<Callable::CallError>());
	cc.mov(asmjit::x86::dword_ptr(call_error_ptr), (int)Callable::CallError::CALL_OK);

	return call_error_ptr;
}

void GDScriptJitCodeGenerator::write_assert(const Address &p_test, const Address &p_message) {
	append_opcode(GDScriptFunction::OPCODE_ASSERT);
	append(p_test);
	append(p_message);
}

void GDScriptJitCodeGenerator::start_block() {
	push_stack_identifiers();
}

void GDScriptJitCodeGenerator::end_block() {
	pop_stack_identifiers();
}

void GDScriptJitCodeGenerator::clear_temporaries() {
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
	// Do not check `is_local_dirty()` here! Always clear the address since the codegen doesn't track the compiler.
	// Also, this method is used to initialize local variables of built-in types, since they cannot be `null`.

	if (p_address.type.has_type && p_address.type.kind == GDScriptDataType::BUILTIN) {
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

bool GDScriptJitCodeGenerator::is_local_dirty(const Address &p_address) const {
	ERR_FAIL_COND_V(p_address.mode != Address::LOCAL_VARIABLE, false);
	return dirty_locals.has(p_address.address);
}

GDScriptJitCodeGenerator::~GDScriptJitCodeGenerator() {
	if (!ended && function != nullptr) {
		memdelete(function);
	}
	JitRuntimeManager::get_singleton()->get_code().reinit();
}

GDScriptJitCodeGenerator::GDScriptJitCodeGenerator() :
		cc(&JitRuntimeManager::get_singleton()->get_code()), stackManager(&cc) {
	JitRuntimeManager::get_singleton()->get_code().setLogger(&stringLogger);
}

void GDScriptJitCodeGenerator::print_address(const Address &p_address, const String &p_label) {
	String prefix = p_label.is_empty() ? "" : p_label + ": ";

	switch (p_address.mode) {
		case Address::SELF:
			print_line(prefix, "SELF");
			break;

		case Address::CLASS:
			print_line(prefix, "CLASS");
			break;

		case Address::MEMBER:
			print_line(prefix, "MEMBER[", p_address.address, "] type=",
					p_address.type.has_type ? Variant::get_type_name(p_address.type.builtin_type) : "untyped");
			break;

		case Address::CONSTANT:
			print_line(prefix, "CONSTANT[", p_address.address, "] type=",
					p_address.type.has_type ? Variant::get_type_name(p_address.type.builtin_type) : "untyped");
			if (constant_map.size() > p_address.address) {
				for (const auto &pair : constant_map) {
					if ((uint32_t)pair.value == p_address.address) {
						print_line("  value=", pair.key);
						break;
					}
				}
			}
			break;

		case Address::LOCAL_VARIABLE:
			print_line(prefix, "LOCAL_VAR[", p_address.address, "] type=",
					p_address.type.has_type ? Variant::get_type_name(p_address.type.builtin_type) : "untyped");
			for (const auto &pair : stack_identifiers) {
				if ((uint32_t)pair.value == p_address.address) {
					print_line("  name=", pair.key);
					break;
				}
			}
			break;

		case Address::FUNCTION_PARAMETER:
			print_line(prefix, "PARAM[", p_address.address, "] type=",
					p_address.type.has_type ? Variant::get_type_name(p_address.type.builtin_type) : "untyped");
			for (const auto &pair : stack_identifiers) {
				if ((uint32_t)pair.value == p_address.address) {
					print_line("  name=", pair.key);
					break;
				}
			}
			break;

		case Address::TEMPORARY: {
			print_line(prefix, "TEMP[", p_address.address, "] type=", p_address.type.has_type ? Variant::get_type_name(p_address.type.builtin_type) : "untyped");
			if (p_address.address < temporaries.size()) {
				const StackSlot &slot = temporaries[p_address.address];
				print_line("  slot_type=", Variant::get_type_name(slot.type),
						" can_contain_object=", slot.can_contain_object);
			}

		} break;

		case Address::NIL:
			print_line(prefix, "NIL");
			break;

		default:
			print_line(prefix, "UNKNOWN[", (int)p_address.mode, "][", p_address.address, "]");
			break;
	}

	if (p_address.type.has_type) {
		String type_info = "Type: ";
		switch (p_address.type.kind) {
			case GDScriptDataType::BUILTIN:
				type_info += "BUILTIN(" + Variant::get_type_name(p_address.type.builtin_type) + ")";
				break;
			case GDScriptDataType::NATIVE:
				type_info += "NATIVE(" + p_address.type.native_type + ")";
				break;
			case GDScriptDataType::SCRIPT:
				type_info += "SCRIPT";
				break;
			case GDScriptDataType::GDSCRIPT:
				type_info += "GDSCRIPT";
				break;
			default:
				type_info += "UNKNOWN_KIND";
				break;
		}

		if (p_address.type.can_contain_object()) {
			type_info += " (can_contain_object)";
		}

		print_line("  ", type_info);
	}

	int bytecode_addr = address_of(p_address);
	if (bytecode_addr != -1) {
		print_line("  bytecode_address=0x", String::num_int64(bytecode_addr, 16));

		int addr_type = (bytecode_addr & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS;
		int addr_index = bytecode_addr & GDScriptFunction::ADDR_MASK;
		String addr_type_name;

		switch (addr_type) {
			case GDScriptFunction::ADDR_TYPE_MEMBER:
				addr_type_name = "MEMBER";
				break;
			case GDScriptFunction::ADDR_TYPE_CONSTANT:
				addr_type_name = "CONSTANT";
				break;
			case GDScriptFunction::ADDR_TYPE_STACK:
				addr_type_name = "STACK";
				break;
			default:
				addr_type_name = "UNKNOWN";
				break;
		}

		print_line("  decoded: type=", addr_type_name, " index=", addr_index);
	}
}

void GDScriptJitCodeGenerator::decode_address(const Address &p_address, int &address_type, int &address_index) {
	switch (p_address.mode) {
		case Address::MEMBER:
			address_type = GDScriptFunction::ADDR_TYPE_MEMBER;
			address_index = p_address.address;
			break;
		case Address::CONSTANT:
			address_type = GDScriptFunction::ADDR_TYPE_CONSTANT;
			address_index = p_address.address;
			break;
		case Address::LOCAL_VARIABLE:
		case Address::FUNCTION_PARAMETER: {
			address_type = GDScriptFunction::ADDR_TYPE_STACK;
			address_index = p_address.address;
		} break;
		case Address::TEMPORARY: {
			address_type = GDScriptFunction::ADDR_TYPE_STACK;
			address_index = p_address.address;
		} break;

		default: {
			ERR_FAIL_MSG("Invalid address mode in GDScriptJitCodeGenerator::decode_address");
		}
	}
}

Gp GDScriptJitCodeGenerator::get_variant_ptr(const Address &p_address) {
	Gp variant_ptr = cc.newIntPtr();

	switch (p_address.mode) {
		case Address::SELF: {
			cc.lea(variant_ptr, Arch::ptr(stack_ptr, GDScriptFunction::ADDR_SELF * sizeof(Variant)));
		} break;

		case Address::CLASS: {
			cc.lea(variant_ptr, Arch::ptr(stack_ptr, GDScriptFunction::ADDR_CLASS * sizeof(Variant)));
		} break;

		case Address::MEMBER: {
			cc.lea(variant_ptr, Arch::ptr(members_ptr, p_address.address * sizeof(Variant)));
		} break;

		case Address::CONSTANT: {
			cc.lea(variant_ptr, Arch::ptr(constants_ptr_label, p_address.address * sizeof(Variant)));
		} break;

		case Address::LOCAL_VARIABLE:
		case Address::FUNCTION_PARAMETER: {
			cc.lea(variant_ptr, Arch::ptr(stack_ptr, p_address.address * sizeof(Variant)));
		} break;

		case Address::TEMPORARY: {
			cc.lea(variant_ptr, Arch::ptr(stack_ptr));
			asmjit::BaseNode *node = cc.cursor();

			MemoryPatch patch;
			patch.node = node;
			patch.operand_index = 1;
			patch.temp_address = p_address.address;
			patch.patch_type = MemoryPatch::VARIANT_PTR;

			memory_patches.push_back(patch);
		} break;

		case Address::NIL: {
			cc.lea(variant_ptr, Arch::ptr(stack_ptr, GDScriptFunction::ADDR_NIL * sizeof(Variant)));
		} break;
	}

	return variant_ptr;
}

template <typename RegT>
void GDScriptJitCodeGenerator::mov_from_variant_mem(const RegT &dst, const Address &p_address, int offset) {
	static_assert(std::is_base_of<Arch::Reg, RegT>::value, "RegT must derive from Reg");
	Mem mem = get_variant_mem(p_address, offset);

	if constexpr (std::is_same<RegT, Gp>::value) {
		cc.mov(dst, mem);
	} else if constexpr (std::is_same<RegT, Vec>::value) {
		cc.movsd(dst, mem);
	} else {
		static_assert(false, "Unsupported register type");
	}

	if (p_address.mode == Address::TEMPORARY) {
		asmjit::BaseNode *node = cc.cursor();
		MemoryPatch patch;
		patch.node = node;
		patch.operand_index = 1;
		patch.temp_address = p_address.address;
		patch.additional_offset = offset;
		patch.patch_type = MemoryPatch::VARIANT_MEM;
		memory_patches.push_back(patch);
	}
}

template <typename RegT>
void GDScriptJitCodeGenerator::mov_to_variant_mem(const Address &p_address, const RegT &src, int offset) {
	static_assert(std::is_base_of<Arch::Reg, RegT>::value, "RegT must derive from Reg");
	Mem mem = get_variant_mem(p_address, offset);

	if constexpr (std::is_same<RegT, Gp>::value) {
		cc.mov(mem, src);
	} else if constexpr (std::is_same<RegT, Vec>::value) {
		cc.movsd(mem, src);
	} else {
		static_assert(false, "Unsupported register type");
	}

	if (p_address.mode == Address::TEMPORARY) {
		asmjit::BaseNode *node = cc.cursor();
		MemoryPatch patch;
		patch.node = node;
		patch.operand_index = 0;
		patch.temp_address = p_address.address;
		patch.additional_offset = offset;
		patch.patch_type = MemoryPatch::VARIANT_MEM;
		memory_patches.push_back(patch);
	}
}

//todo combine all
void GDScriptJitCodeGenerator::mov_from_variant_type_mem(const Gp &dst, const Address &p_address, int offset) {
	Mem mem = get_variant_type_mem(p_address, offset);
	cc.mov(dst, mem);

	if (p_address.mode == Address::TEMPORARY) {
		asmjit::BaseNode *node = cc.cursor();
		MemoryPatch patch;
		patch.node = node;
		patch.operand_index = 1;
		patch.temp_address = p_address.address;
		patch.additional_offset = offset;
		patch.patch_type = MemoryPatch::VARIANT_TYPE_MEM;
		memory_patches.push_back(patch);
	}
}

void GDScriptJitCodeGenerator::mov_to_variant_type_mem(const Address &p_address, int type_value, int offset) {
	Mem mem = get_variant_type_mem(p_address, offset);
	cc.mov(mem, type_value);

	if (p_address.mode == Address::TEMPORARY) {
		asmjit::BaseNode *node = cc.cursor();
		MemoryPatch patch;
		patch.node = node;
		patch.operand_index = 0;
		patch.temp_address = p_address.address;
		patch.additional_offset = offset;
		patch.patch_type = MemoryPatch::VARIANT_TYPE_MEM;
		memory_patches.push_back(patch);
	}
}

void GDScriptJitCodeGenerator::create_patch(const Address &p_address, int operand_index, int offset) {
	if (p_address.mode == Address::TEMPORARY) {
		asmjit::BaseNode *node = cc.cursor();
		MemoryPatch patch;
		patch.node = node;
		patch.operand_index = operand_index;
		patch.temp_address = p_address.address;
		patch.additional_offset = offset;
		patch.patch_type = MemoryPatch::VARIANT_TYPE_MEM;
		memory_patches.push_back(patch);
	}
}

void GDScriptJitCodeGenerator::patch_jit() {
	int base_offset = (GDScriptFunction::FIXED_ADDRESSES_MAX + max_locals) * sizeof(Variant);

	for (const MemoryPatch &patch : memory_patches) {
		asmjit::InstNode *inst_node = patch.node->as<asmjit::InstNode>();

		if (inst_node) {
			asmjit::Operand &operand = inst_node->op(patch.operand_index);

			if (operand.isMem()) {
				Mem &mem = operand.as<Mem>();

				int final_disp = base_offset + (patch.temp_address * sizeof(Variant)) + patch.additional_offset;

				mem.setOffset(final_disp);

				// if (patch.patch_type == MemoryPatch::VARIANT_TYPE_MEM) {
				// 	mem.setSize(4); // dword
				// } else {
				// 	mem.setSize(8); // qword
				// }
			}
		}
	}

	for (const NamePatch &patch : name_patches) {
		patch.invoke_node->setArg(patch.arg_index, &function->_global_names_ptr[patch.name_index]);
	}

	memory_patches.clear();
	name_patches.clear();
}

//todo
Mem GDScriptJitCodeGenerator::get_variant_mem(const Address &p_address, int offset) {
	switch (p_address.mode) {
		case Address::SELF: {
			int disp = GDScriptFunction::ADDR_SELF * sizeof(Variant) + offset;
			return Arch::qword_ptr(stack_ptr, disp);
		}

		case Address::CLASS: {
			int disp = GDScriptFunction::ADDR_CLASS * sizeof(Variant) + offset;
			return Arch::qword_ptr(stack_ptr, disp);
		}

		case Address::MEMBER: {
			int disp = p_address.address * sizeof(Variant) + offset;
			return Arch::qword_ptr(members_ptr, disp);
		}

		case Address::CONSTANT: {
			int disp = p_address.address * sizeof(Variant) + offset;
			return Arch::qword_ptr(constants_ptr_label, disp);
		}

		case Address::LOCAL_VARIABLE:
		case Address::FUNCTION_PARAMETER: {
			int disp = p_address.address * sizeof(Variant) + offset;
			return Arch::qword_ptr(stack_ptr, disp);
		}

		case Address::TEMPORARY: {
			return Arch::qword_ptr(stack_ptr);
		}

		case Address::NIL: {
			int disp = GDScriptFunction::ADDR_NIL * sizeof(Variant) + offset;
			return Arch::qword_ptr(stack_ptr, disp);
		}

		default: {
			return Arch::qword_ptr(stack_ptr);
		}
	}
}

//todo
Mem GDScriptJitCodeGenerator::get_variant_type_mem(const Address &p_address, int offset) {
	switch (p_address.mode) {
		case Address::SELF: {
			int disp = GDScriptFunction::ADDR_SELF * sizeof(Variant) + offset;
			return Arch::dword_ptr(stack_ptr, disp);
		}

		case Address::CLASS: {
			int disp = GDScriptFunction::ADDR_CLASS * sizeof(Variant) + offset;
			return Arch::dword_ptr(stack_ptr, disp);
		}

		case Address::MEMBER: {
			int disp = p_address.address * sizeof(Variant) + offset;
			return Arch::dword_ptr(members_ptr, disp);
		}

		case Address::CONSTANT: {
			int disp = p_address.address * sizeof(Variant) + offset;
			return Arch::dword_ptr(constants_ptr_label, disp);
		}

		case Address::LOCAL_VARIABLE:
		case Address::FUNCTION_PARAMETER: {
			int disp = p_address.address * sizeof(Variant) + offset;
			return Arch::dword_ptr(stack_ptr, disp);
		}

		case Address::TEMPORARY: {
			return Arch::dword_ptr(stack_ptr);
		}

		case Address::NIL: {
			int disp = GDScriptFunction::ADDR_NIL * sizeof(Variant) + offset;
			return Arch::dword_ptr(stack_ptr, disp);
		}

		default: {
			return Arch::dword_ptr(stack_ptr);
		}
	}
}

void GDScriptJitCodeGenerator::copy_variant(Gp &dst_ptr, Gp &src_ptr) {
	asmjit::InvokeNode *copy_invoke;
	cc.invoke(&copy_invoke,
			static_cast<void (*)(Variant *, const Variant *)>([](Variant *dst, const Variant *src) {
				*dst = *src;
			}),
			asmjit::FuncSignature::build<void, Variant *, const Variant *>());
	copy_invoke->setArg(0, dst_ptr);
	copy_invoke->setArg(1, src_ptr);
}

//todo
void GDScriptJitCodeGenerator::assign(const Address &src, const Address &dst) {
	if (src.type.kind == GDScriptDataType::BUILTIN && dst.type.kind == GDScriptDataType::BUILTIN) {
		switch (src.type.builtin_type) {
			case Variant::INT: {
				Gp tmp = cc.newInt64();
				mov_from_variant_mem(tmp, src, OFFSET_INT);
				mov_to_variant_type_mem(dst, (int)Variant::INT);
				mov_to_variant_mem(dst, tmp, OFFSET_INT);
			} break;
			case Variant::BOOL: {
				Gp tmp = cc.newInt64();
				mov_from_variant_mem(tmp, src, OFFSET_BOOL);
				mov_to_variant_type_mem(dst, (int)Variant::BOOL);
				mov_to_variant_mem(dst, tmp, OFFSET_BOOL);
			} break;
			case Variant::FLOAT: {
				Vec tmp = cc.newXmm();
				mov_from_variant_mem(tmp, src, OFFSET_FLOAT);
				mov_to_variant_type_mem(dst, (int)Variant::FLOAT);
				mov_to_variant_mem(dst, tmp, OFFSET_FLOAT);
			} break;
			case Variant::VECTOR2: {
				Vec x = cc.newXmm();
				Vec y = cc.newXmm();
				mov_from_variant_mem(x, src, OFFSET_VECTOR2_X);
				mov_from_variant_mem(y, src, OFFSET_VECTOR2_Y);
				mov_to_variant_type_mem(dst, (int)Variant::VECTOR2);
				mov_to_variant_mem(dst, x, OFFSET_VECTOR2_X);
				mov_to_variant_mem(dst, y, OFFSET_VECTOR2_Y);
			} break;
			default: {
				Gp src_ptr = get_variant_ptr(src);
				Gp dst_ptr = get_variant_ptr(dst);

				copy_variant(dst_ptr, src_ptr);
			} break;
		};
	} else {
		Gp src_ptr = get_variant_ptr(src);
		Gp dst_ptr = get_variant_ptr(dst);

		copy_variant(dst_ptr, src_ptr);
	}
}

void GDScriptJitCodeGenerator::assign_bool(const Address &dst, bool value) {
	if (dst.type.kind == GDScriptDataType::BUILTIN && !Variant::needs_deinit[dst.type.builtin_type]) {
		Gp tmp = cc.newInt64();
		if (value) {
			cc.mov(tmp, 1);
		} else {
			cc.xor_(tmp, tmp);
		}
		mov_to_variant_mem(dst, tmp, OFFSET_BOOL); //todo
	} else {
		Gp dst_ptr = get_variant_ptr(dst);

		asmjit::InvokeNode *copy_invoke;
		cc.invoke(&copy_invoke,
				static_cast<void (*)(Variant *)>([](Variant *i_dst) {
					*i_dst = true;
				}),
				asmjit::FuncSignature::build<void, Variant *>());
		copy_invoke->setArg(0, dst_ptr);
	}
}

void GDScriptJitCodeGenerator::assign_null(const Address &dst) {
	Gp dst_ptr = get_variant_ptr(dst);

	asmjit::InvokeNode *copy_invoke;
	cc.invoke(&copy_invoke,
			static_cast<void (*)(Variant *)>([](Variant *i_dst) {
				*i_dst = Variant();
			}),
			asmjit::FuncSignature::build<void, Variant *>());
	copy_invoke->setArg(0, dst_ptr);
}

//todo all op
void GDScriptJitCodeGenerator::handle_int_operation(Variant::Operator p_operator, const Address &p_left, const Address &p_right, const Address &p_result) {
	Gp left = cc.newInt64();
	Mem right = get_variant_mem(p_right, OFFSET_INT);

	mov_from_variant_mem(left, p_left, OFFSET_INT);

	switch (p_operator) {
		case Variant::OP_ADD:
			cc.add(left, right);
			create_patch(p_right, 1, OFFSET_INT);
			break;
		case Variant::OP_SUBTRACT:
			cc.sub(left, right);
			create_patch(p_right, 1, OFFSET_INT);
			break;
		case Variant::OP_MULTIPLY:
			cc.imul(left, right);
			create_patch(p_right, 1, OFFSET_INT);
			break;
		case Variant::OP_EQUAL:
			gen_compare_int(left, right, p_right, Arch::CondCode::kEqual);
			break;
		case Variant::OP_NOT_EQUAL:
			gen_compare_int(left, right, p_right, Arch::CondCode::kNotEqual);
			break;
		case Variant::OP_LESS:
			gen_compare_int(left, right, p_right, Arch::CondCode::kL);
			break;
		case Variant::OP_LESS_EQUAL:
			gen_compare_int(left, right, p_right, Arch::CondCode::kLE);
			break;
		case Variant::OP_GREATER:
			gen_compare_int(left, right, p_right, Arch::CondCode::kG);
			break;
		case Variant::OP_GREATER_EQUAL:
			gen_compare_int(left, right, p_right, Arch::CondCode::kGE);
			break;
		default: {
			print_error("Unsupported int operation");
			return;
		}
	}

	auto return_type = (p_operator == Variant::OP_EQUAL || p_operator == Variant::OP_NOT_EQUAL ||
							   p_operator == Variant::OP_LESS || p_operator == Variant::OP_LESS_EQUAL ||
							   p_operator == Variant::OP_GREATER || p_operator == Variant::OP_GREATER_EQUAL ||
							   p_operator == Variant::OP_AND || p_operator == Variant::OP_OR ||
							   p_operator == Variant::OP_XOR || p_operator == Variant::OP_NOT ||
							   p_operator == Variant::OP_IN)
			? Variant::BOOL
			: Variant::INT;
	if (p_result.type.builtin_type != return_type || p_result.type.kind == GDScriptDataType::UNINITIALIZED) {
		mov_to_variant_type_mem(p_result, return_type); //?
	}
	mov_to_variant_mem(p_result, left, OFFSET_INT);
}

void GDScriptJitCodeGenerator::gen_compare_int(Gp &lhs, Mem &rhs, const Address &p_right, Arch::CondCode code) {
	cc.cmp(lhs, rhs);
	create_patch(p_right, 1, OFFSET_INT);
	cc.set(code, lhs.r8());
	cc.movzx(lhs, lhs.r8());
}

void GDScriptJitCodeGenerator::gen_compare_float(Vec &lhs, Vec &rhs, const Address &result_addr, Arch::CondCode code) {
	cc.comisd(lhs, rhs);
	cc.set(code, get_variant_mem(result_addr, OFFSET_BOOL));
	create_patch(result_addr, 0, OFFSET_BOOL);
}

Gp GDScriptJitCodeGenerator::prepare_args_array(const Vector<Address> &p_args) {
	Gp args_array = cc.newIntPtr("args_array");

	if (p_args.size() > 0) {
		Mem args_stack = stackManager.allocArg(p_args.size());
		cc.lea(args_array, args_stack);

		for (int i = 0; i < p_args.size(); i++) {
			const Address &arg_addr = p_args[i];

			Gp arg_ptr = get_variant_ptr(arg_addr);

			cc.mov(Arch::ptr(args_array, i * PTR_SIZE), arg_ptr);
		}
	} else {
		cc.xor_(args_array, args_array);
	}

	return args_array;
}

void GDScriptJitCodeGenerator::handle_vector2_operation(Variant::Operator p_operator, const Address &p_left, const Address &p_right, const Address &p_result) {
	Vec left_x = cc.newXmmSs("left_x");
	Vec left_y = cc.newXmmSs("left_y");
	Vec right_x = cc.newXmmSs("right_x");
	Vec right_y = cc.newXmmSs("right_y");

	if (p_left.type.builtin_type == Variant::VECTOR2) {
		cc.movss(left_x, get_variant_type_mem(p_left, OFFSET_VECTOR2_X));
		create_patch(p_left, 1, OFFSET_VECTOR2_X);
		cc.movss(left_y, get_variant_type_mem(p_left, OFFSET_VECTOR2_Y));
		create_patch(p_left, 1, OFFSET_VECTOR2_Y);
	} else if (p_left.type.builtin_type == Variant::FLOAT) {
		cc.cvtsd2ss(left_x, get_variant_mem(p_left, OFFSET_FLOAT));
		create_patch(p_left, 1, OFFSET_FLOAT);
		cc.movss(left_y, left_x);
	} else if (p_left.type.builtin_type == Variant::INT) {
		cc.cvtsi2ss(left_x, get_variant_mem(p_left, OFFSET_INT));
		create_patch(p_left, 1, OFFSET_INT);
		cc.movss(left_y, left_x);
	}

	if (p_right.type.builtin_type == Variant::VECTOR2) {
		cc.movss(right_x, get_variant_type_mem(p_right, OFFSET_VECTOR2_X));
		create_patch(p_right, 1, OFFSET_VECTOR2_X);
		cc.movss(right_y, get_variant_type_mem(p_right, OFFSET_VECTOR2_Y));
		create_patch(p_right, 1, OFFSET_VECTOR2_Y);
	} else if (p_right.type.builtin_type == Variant::FLOAT) {
		cc.cvtsd2ss(right_x, get_variant_mem(p_right, OFFSET_FLOAT));
		create_patch(p_right, 1, OFFSET_FLOAT);
		cc.movss(right_y, right_x);
	} else if (p_right.type.builtin_type == Variant::INT) {
		cc.cvtsi2ss(right_x, get_variant_mem(p_right, OFFSET_INT));
		create_patch(p_right, 1, OFFSET_INT);
		cc.movss(right_y, right_x);
	}

	switch (p_operator) {
		case Variant::OP_ADD: {
			cc.addss(left_x, right_x);
			cc.addss(left_y, right_y);
			break;
		}
		case Variant::OP_SUBTRACT: {
			cc.subss(left_x, right_x);
			cc.subss(left_y, right_y);
			break;
		}
		case Variant::OP_MULTIPLY: {
			cc.mulss(left_x, right_x);
			cc.mulss(left_y, right_y);
			break;
		}
		case Variant::OP_DIVIDE: {
			cc.divss(left_x, right_x);
			cc.divss(left_y, right_y);
			break;
		}
		default: {
			print_line("Unsupported Vector2 operation");
			return;
		}
	}

	mov_to_variant_type_mem(p_result, Variant::VECTOR2);
	cc.movss(get_variant_type_mem(p_result, OFFSET_VECTOR2_X), left_x);
	create_patch(p_result, 0, OFFSET_VECTOR2_X);
	cc.movss(get_variant_type_mem(p_result, OFFSET_VECTOR2_Y), left_y);
	create_patch(p_result, 0, OFFSET_VECTOR2_Y);
}