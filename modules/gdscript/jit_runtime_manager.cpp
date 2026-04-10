#include "jit_runtime_manager.h"

#include "gdscript.h"
#include "gdscript_function.h"
#include "jit_ir_optimizer.h"
#include "core/variant/variant_internal.h"

using namespace asmjit::ujit;

JitRuntimeManager *JitRuntimeManager::singleton = nullptr;

static String _ir_value_name(const ValueId &p_v) {
	return vformat("v%d", p_v.id);
}

static String _ir_block_name(uint32_t p_block_id) {
	return vformat("b%d", p_block_id);
}

static String _ir_label_name(uint64_t p_label_id) {
	return vformat("l%d", p_label_id);
}

static const char *_ir_cond_name(IRCond p_cond) {
	switch (p_cond) {
		case IRCond::EQ: return "eq";
		case IRCond::NE: return "ne";
		case IRCond::LT: return "lt";
		case IRCond::LE: return "le";
		case IRCond::GT: return "gt";
		case IRCond::GE: return "ge";
	}
	return "unknown";
}

static uint32_t _jit_variant_booleanize(const Variant *p_variant) {
	return p_variant->booleanize() ? 1 : 0;
}

static void _jit_variant_assign(Variant *p_dst, const Variant *p_src) {
	*p_dst = *p_src;
}

static void _jit_variant_assign_typed_array(Variant *p_dst, const Variant *p_src, const Variant *p_script_type, int p_builtin_type, const StringName *p_native_type) {
	if (p_src->get_type() != Variant::ARRAY) {
		return;
	}
	const Array *array = VariantInternal::get_array(p_src);
	if (array->get_typed_builtin() != uint32_t(p_builtin_type) || array->get_typed_class_name() != *p_native_type || array->get_typed_script() != *p_script_type) {
		return;
	}
	*p_dst = *p_src;
}

static void _jit_variant_assign_typed_dictionary(Variant *p_dst, const Variant *p_src, const Variant *p_key_script_type, int p_key_builtin_type, const StringName *p_key_native_type, const Variant *p_value_script_type, int p_value_builtin_type, const StringName *p_value_native_type) {
	if (p_src->get_type() != Variant::DICTIONARY) {
		return;
	}
	const Dictionary *dictionary = VariantInternal::get_dictionary(p_src);
	if (dictionary->get_typed_key_builtin() != uint32_t(p_key_builtin_type) || dictionary->get_typed_key_class_name() != *p_key_native_type || dictionary->get_typed_key_script() != *p_key_script_type ||
			dictionary->get_typed_value_builtin() != uint32_t(p_value_builtin_type) || dictionary->get_typed_value_class_name() != *p_value_native_type || dictionary->get_typed_value_script() != *p_value_script_type) {
		return;
	}
	*p_dst = *p_src;
}

static void _jit_variant_type_adjust(Variant *p_variant, int p_type) {
	switch (Variant::Type(p_type)) {
		case Variant::BOOL: VariantTypeAdjust<bool>::adjust(p_variant); break;
		case Variant::INT: VariantTypeAdjust<int64_t>::adjust(p_variant); break;
		case Variant::FLOAT: VariantTypeAdjust<double>::adjust(p_variant); break;
		case Variant::STRING: VariantTypeAdjust<String>::adjust(p_variant); break;
		case Variant::VECTOR2: VariantTypeAdjust<Vector2>::adjust(p_variant); break;
		case Variant::VECTOR2I: VariantTypeAdjust<Vector2i>::adjust(p_variant); break;
		case Variant::RECT2: VariantTypeAdjust<Rect2>::adjust(p_variant); break;
		case Variant::RECT2I: VariantTypeAdjust<Rect2i>::adjust(p_variant); break;
		case Variant::VECTOR3: VariantTypeAdjust<Vector3>::adjust(p_variant); break;
		case Variant::VECTOR3I: VariantTypeAdjust<Vector3i>::adjust(p_variant); break;
		case Variant::TRANSFORM2D: VariantTypeAdjust<Transform2D>::adjust(p_variant); break;
		case Variant::VECTOR4: VariantTypeAdjust<Vector4>::adjust(p_variant); break;
		case Variant::VECTOR4I: VariantTypeAdjust<Vector4i>::adjust(p_variant); break;
		case Variant::PLANE: VariantTypeAdjust<Plane>::adjust(p_variant); break;
		case Variant::QUATERNION: VariantTypeAdjust<Quaternion>::adjust(p_variant); break;
		case Variant::AABB: VariantTypeAdjust<AABB>::adjust(p_variant); break;
		case Variant::BASIS: VariantTypeAdjust<Basis>::adjust(p_variant); break;
		case Variant::TRANSFORM3D: VariantTypeAdjust<Transform3D>::adjust(p_variant); break;
		case Variant::PROJECTION: VariantTypeAdjust<Projection>::adjust(p_variant); break;
		case Variant::COLOR: VariantTypeAdjust<Color>::adjust(p_variant); break;
		case Variant::STRING_NAME: VariantTypeAdjust<StringName>::adjust(p_variant); break;
		case Variant::NODE_PATH: VariantTypeAdjust<NodePath>::adjust(p_variant); break;
		case Variant::RID: VariantTypeAdjust<RID>::adjust(p_variant); break;
		case Variant::OBJECT: VariantTypeAdjust<Object *>::adjust(p_variant); break;
		case Variant::CALLABLE: VariantTypeAdjust<Callable>::adjust(p_variant); break;
		case Variant::SIGNAL: VariantTypeAdjust<Signal>::adjust(p_variant); break;
		case Variant::DICTIONARY: VariantTypeAdjust<Dictionary>::adjust(p_variant); break;
		case Variant::ARRAY: VariantTypeAdjust<Array>::adjust(p_variant); break;
		case Variant::PACKED_BYTE_ARRAY: VariantTypeAdjust<PackedByteArray>::adjust(p_variant); break;
		case Variant::PACKED_INT32_ARRAY: VariantTypeAdjust<PackedInt32Array>::adjust(p_variant); break;
		case Variant::PACKED_INT64_ARRAY: VariantTypeAdjust<PackedInt64Array>::adjust(p_variant); break;
		case Variant::PACKED_FLOAT32_ARRAY: VariantTypeAdjust<PackedFloat32Array>::adjust(p_variant); break;
		case Variant::PACKED_FLOAT64_ARRAY: VariantTypeAdjust<PackedFloat64Array>::adjust(p_variant); break;
		case Variant::PACKED_STRING_ARRAY: VariantTypeAdjust<PackedStringArray>::adjust(p_variant); break;
		case Variant::PACKED_VECTOR2_ARRAY: VariantTypeAdjust<PackedVector2Array>::adjust(p_variant); break;
		case Variant::PACKED_VECTOR3_ARRAY: VariantTypeAdjust<PackedVector3Array>::adjust(p_variant); break;
		case Variant::PACKED_COLOR_ARRAY: VariantTypeAdjust<PackedColorArray>::adjust(p_variant); break;
		case Variant::PACKED_VECTOR4_ARRAY: VariantTypeAdjust<PackedVector4Array>::adjust(p_variant); break;
		case Variant::NIL:
		case Variant::VARIANT_MAX:
			break;
	}
}

static void _jit_variant_return(Variant *p_result, const Variant *p_src) {
	*p_result = *p_src;
}

static void _jit_variant_return_typed_builtin(Variant *p_result, const Variant *p_src, int p_type) {
	const Variant::Type ret_type = Variant::Type(p_type);
	if (p_src->get_type() != ret_type) {
		Callable::CallError ce;
		if (Variant::can_convert_strict(p_src->get_type(), ret_type)) {
			const Variant *args[1] = { p_src };
			Variant::construct(ret_type, *p_result, args, 1, ce);
		} else {
			Variant::construct(ret_type, *p_result, nullptr, 0, ce);
#ifdef DEBUG_ENABLED
			ERR_FAIL_MSG("Trying to return value of type '" + Variant::get_type_name(p_src->get_type()) +
					"' from a function whose return type is '" + Variant::get_type_name(ret_type) + "'.");
#endif
		}
	} else {
		*p_result = *p_src;
	}
}

static void _jit_variant_get_named(const Variant *p_src, Variant *p_dst, const StringName *p_name) {
	bool valid = false;
	*p_dst = p_src->get_named(*p_name, valid);
}

static void _jit_variant_set_named(Variant *p_dst, const Variant *p_src, const StringName *p_name) {
	bool valid = false;
	p_dst->set_named(*p_name, *p_src, valid);
}

static void _jit_variant_get_member(const Variant *p_self, Variant *p_dst, const StringName *p_name) {
	Object *owner = *p_self;
	if (owner) {
		ClassDB::get_property(owner, *p_name, *p_dst);
	}
}

static void _jit_variant_set_member(const Variant *p_self, const Variant *p_src, const StringName *p_name) {
	Object *owner = *p_self;
	if (owner) {
		bool valid = false;
		ClassDB::set_property(owner, *p_name, *p_src, &valid);
	}
}

static void _jit_variant_get_keyed(const Variant *p_src, const Variant *p_key, Variant *p_dst) {
	bool valid = false;
	*p_dst = p_src->get(*p_key, &valid);
}

static void _jit_variant_set_keyed(Variant *p_dst, const Variant *p_key, const Variant *p_src) {
	bool valid = false;
	p_dst->set(*p_key, *p_src, &valid);
}

static void _jit_variant_get_indexed_validated(const Variant *p_src, const int64_t int_index, Variant *p_dst, uint64_t p_getter) {
	Variant::ValidatedIndexedGetter getter = reinterpret_cast<Variant::ValidatedIndexedGetter>(p_getter);
	bool oob = false;
	getter(p_src, int_index, p_dst, &oob);
}

static void _jit_variant_set_indexed_validated(Variant *p_dst, const int64_t int_index, const Variant *p_src, uint64_t p_setter) {
	Variant::ValidatedIndexedSetter setter = reinterpret_cast<Variant::ValidatedIndexedSetter>(p_setter);
	bool oob = false;
	setter(p_dst, int_index, p_src, &oob);
}

static void _jit_variant_get_keyed_validated(const Variant *p_src, const Variant *p_key, Variant *p_dst, uint64_t p_getter) {
	Variant::ValidatedKeyedGetter getter = reinterpret_cast<Variant::ValidatedKeyedGetter>(p_getter);
	bool valid = false;
	getter(p_src, p_key, p_dst, &valid);
}

static void _jit_variant_set_keyed_validated(Variant *p_dst, const Variant *p_key, const Variant *p_src, uint64_t p_setter) {
	Variant::ValidatedKeyedSetter setter = reinterpret_cast<Variant::ValidatedKeyedSetter>(p_setter);
	bool valid = false;
	setter(p_dst, p_key, p_src, &valid);
}

static void _jit_variant_operator(uint64_t p_operator, const Variant *p_left, const Variant *p_right, Variant *p_dst) {
	bool valid = false;
	Variant::evaluate(Variant::Operator(p_operator), *p_left, *p_right, *p_dst, valid);
}

static uint32_t _jit_variant_iterate_begin(const Variant *p_container, Variant *p_counter, Variant *p_iterator) {
	*p_counter = Variant();

	bool valid = false;
	if (!p_container->iter_init(*p_counter, valid)) {
		return 0;
	}

	*p_iterator = p_container->iter_get(*p_counter, valid);
	return valid ? 1u : 0u;
}

static uint32_t _jit_variant_iterate(const Variant *p_container, Variant *p_counter, Variant *p_iterator) {
	bool valid = false;
	if (!p_container->iter_next(*p_counter, valid)) {
		return 0;
	}

	*p_iterator = p_container->iter_get(*p_counter, valid);
	return valid ? 1u : 0u;
}

static void _jit_call_utility_validated(uint64_t p_function, Variant *p_dst, uint64_t p_args, int p_argcount) {
	Variant::ValidatedUtilityFunction function = reinterpret_cast<Variant::ValidatedUtilityFunction>(p_function);
	const Variant **args = reinterpret_cast<const Variant **>(p_args);
	function(p_dst, args, p_argcount);
}

static void _jit_call_gdscript_utility(uint64_t p_function, Variant *p_dst, uint64_t p_args, int p_argcount) {
	GDScriptUtilityFunctions::FunctionPtr function = reinterpret_cast<GDScriptUtilityFunctions::FunctionPtr>(p_function);
	const Variant **args = reinterpret_cast<const Variant **>(p_args);
	Callable::CallError err;
	function(p_dst, args, p_argcount, err);
}

static void _jit_call_builtin_static(const StringName *p_method_name, int p_builtin_type, Variant *p_dst, uint64_t p_args, int p_argcount) {
	const Variant **args = reinterpret_cast<const Variant **>(p_args);
	Callable::CallError err;
	Variant::call_static(Variant::Type(p_builtin_type), *p_method_name, args, p_argcount, *p_dst, err);
}

static void _jit_call_method_bind(uint64_t p_method, const Variant *p_base, Variant *p_dst, uint64_t p_args, int p_argcount) {
	MethodBind *method = reinterpret_cast<MethodBind *>(p_method);
	const Variant **args = reinterpret_cast<const Variant **>(p_args);
#ifdef DEBUG_ENABLED
	bool freed = false;
	Object *base_obj = p_base->get_validated_object_with_check(freed);
	if (freed || !base_obj) {
		VariantInternal::initialize(p_dst, Variant::NIL);
		return;
	}
#else
	Object *base_obj = *VariantInternal::get_object(p_base);
#endif
	Callable::CallError err;
	*p_dst = method->call(base_obj, args, p_argcount, err);
}

static void _jit_call_builtin_validated(uint64_t p_method, Variant *p_base, Variant *p_dst, uint64_t p_args, int p_argcount) {
	Variant::ValidatedBuiltInMethod method = reinterpret_cast<Variant::ValidatedBuiltInMethod>(p_method);
	const Variant **args = reinterpret_cast<const Variant **>(p_args);
	method(p_base, args, p_argcount, p_dst);
}

static void _jit_call_method_bind_validated(uint64_t p_method, const Variant *p_base, Variant *p_dst, uint64_t p_args, int p_argcount) {
	MethodBind *method = reinterpret_cast<MethodBind *>(p_method);
	Object *base_obj = *p_base;
	const Variant **args = reinterpret_cast<const Variant **>(p_args);
	method->validated_call(base_obj, args, p_dst);
}

static void _jit_construct_typed_array(Variant *p_dst, uint64_t p_args, int p_argcount, const Variant *p_script_type, int p_builtin_type, const StringName *p_native_type) {
	const Variant **args = reinterpret_cast<const Variant **>(p_args);
	Array array;
	array.set_typed(Variant::Type(p_builtin_type), *p_native_type, *p_script_type);
	array.resize(p_argcount);
	for (int i = 0; i < p_argcount; i++) {
		array.set(i, *args[i]);
	}
	*p_dst = Variant();
	*p_dst = array;
}

static void _jit_variant_call(Variant *p_base, const StringName *p_method_name, Variant *p_dst, uint64_t p_args, int p_argcount) {
	const Variant **args = reinterpret_cast<const Variant **>(p_args);
	Variant ret;
	Callable::CallError err;
	p_base->callp(*p_method_name, args, p_argcount, ret, err);
	*p_dst = ret;
}

static void _jit_variant_call_utility(const StringName *p_function_name, Variant *p_dst, uint64_t p_args, int p_argcount) {
	const Variant **args = reinterpret_cast<const Variant **>(p_args);
	Callable::CallError err;
	Variant::call_utility_function(*p_function_name, p_dst, args, p_argcount, err);
}

static void _jit_variant_construct(Variant *p_dst, uint64_t p_args, int p_argcount, int p_type) {
	const Variant **args = reinterpret_cast<const Variant **>(p_args);
	Callable::CallError err;
	Variant::construct(Variant::Type(p_type), *p_dst, args, p_argcount, err);
}

static void _jit_variant_construct_validated(Variant *p_dst, uint64_t p_args, Variant::ValidatedConstructor p_constructor) {
	const Variant **args = reinterpret_cast<const Variant **>(p_args);
	p_constructor(p_dst, args);
}

static void _jit_construct_array(Variant *p_dst, uint64_t p_args, int p_argcount) {
	const Variant **args = reinterpret_cast<const Variant **>(p_args);
	Array array;
	array.resize(p_argcount);
	for (int i = 0; i < p_argcount; i++) {
		array[i] = *args[i];
	}
	*p_dst = Variant();
	*p_dst = array;
}

static void _jit_construct_dictionary(Variant *p_dst, uint64_t p_args, int p_argcount) {
	const Variant **args = reinterpret_cast<const Variant **>(p_args);
	Dictionary dict;
	dict.reserve(p_argcount);
	for (int i = 0; i < p_argcount; i++) {
		dict[*args[i * 2 + 0]] = *args[i * 2 + 1];
	}
	*p_dst = Variant();
	*p_dst = dict;
}

static void _jit_construct_typed_dictionary(Variant *p_dst, uint64_t p_args, int p_argcount, const Variant *p_key_script_type, int p_key_builtin_type, const StringName *p_key_native_type, const Variant *p_value_script_type, int p_value_builtin_type, const StringName *p_value_native_type) {
	const Variant **args = reinterpret_cast<const Variant **>(p_args);
	Dictionary dict;
	dict.set_typed(Variant::Type(p_key_builtin_type), *p_key_native_type, *p_key_script_type, Variant::Type(p_value_builtin_type), *p_value_native_type, *p_value_script_type);
	dict.reserve(p_argcount);
	for (int i = 0; i < p_argcount; i++) {
		dict.set(*args[i * 2 + 0], *args[i * 2 + 1]);
	}
	*p_dst = Variant();
	*p_dst = dict;
}

static String _ir_mem_name(const GDScriptCodeGenerator::Address &p_address, int p_max_locals) {
	auto decoded = JitRuntimeManager::decode_address_index(p_address.address);

	int index = decoded.address_index;
	String kind = "stack";

	switch (p_address.mode) {
		case GDScriptCodeGenerator::Address::CONSTANT:
			kind = "const";
			break;
		case GDScriptCodeGenerator::Address::MEMBER:
			kind = "member";
			break;
		case GDScriptCodeGenerator::Address::TEMPORARY:
			kind = "stack";
			index = decoded.address_index + p_max_locals + 3;
			break;
		case GDScriptCodeGenerator::Address::SELF:
			kind = "stack";
			index = GDScriptFunction::ADDR_STACK_SELF;
			break;
		case GDScriptCodeGenerator::Address::CLASS:
			kind = "stack";
			index = GDScriptFunction::ADDR_STACK_CLASS;
			break;
		case GDScriptCodeGenerator::Address::NIL:
			kind = "stack";
			index = GDScriptFunction::ADDR_STACK_NIL;
			break;
		case GDScriptCodeGenerator::Address::LOCAL_VARIABLE:
		case GDScriptCodeGenerator::Address::FUNCTION_PARAMETER:
		default:
			kind = "stack";
			break;
	}

	return vformat("%s[%d]", kind, index);
}

static String _ir_inst_to_string(const IRInst &p_inst, const int p_max_locals) {
	const char *op_name = IROpNames[static_cast<int>(p_inst.op)];
	String out = op_name;

	switch (p_inst.op) {
	case IROp::LoadParam:
	case IROp::LoadF64:
	case IROp::LoadPtr:
		out += vformat(" dst=%s mem=%s", _ir_value_name(p_inst.dst), _ir_mem_name(p_inst.mem_loc, p_max_locals));
		break;
	case IROp::LoadDefArg:
		out += vformat(" dst=%s", _ir_value_name(p_inst.dst));
		break;
		case IROp::ZeroI64:
			out += vformat(" dst=%s", _ir_value_name(p_inst.dst));
			break;
		case IROp::LoadRealMemberF64:
			if (p_inst.args.is_empty()) {
				out += vformat(" dst=%s mem=%s offset=%d", _ir_value_name(p_inst.dst), _ir_mem_name(p_inst.mem_loc, p_max_locals), p_inst.imm);
			} else {
				out += vformat(" dst=%s base_ptr=%s offset=%d", _ir_value_name(p_inst.dst), _ir_value_name(p_inst.args[0]), p_inst.imm);
			}
			break;
		case IROp::StoreRealMemberF64:
			if (p_inst.args.size() == 1) {
				out += vformat(" mem=%s offset=%d src=%s", _ir_mem_name(p_inst.mem_loc, p_max_locals), p_inst.imm, _ir_value_name(p_inst.args[0]));
			} else {
				out += vformat(" base_ptr=%s offset=%d src=%s", _ir_value_name(p_inst.args[0]), p_inst.imm, _ir_value_name(p_inst.args[1]));
			}
			break;
		case IROp::Construct:
			out += vformat(" dst_ptr=%s argc=%d type=%d",
					_ir_value_name(p_inst.args[p_inst.args.size() - 1]),
					p_inst.args.size() - 1,
					p_inst.imm);
			break;
		case IROp::ConstructValidated:
			out += vformat(" dst_ptr=%s argc=%d constructor=%d",
					_ir_value_name(p_inst.args[p_inst.args.size() - 1]),
					p_inst.args.size() - 1,
					p_inst.imm);
			break;
		case IROp::ConstructArray:
			out += vformat(" dst_ptr=%s argc=%d",
					_ir_value_name(p_inst.args[p_inst.args.size() - 1]),
					p_inst.args.size() - 1);
			break;
		case IROp::ConstructDictionary:
			out += vformat(" dst_ptr=%s argc=%d",
					_ir_value_name(p_inst.args[p_inst.args.size() - 1]),
					(p_inst.args.size() - 1) / 2);
			break;
		case IROp::ConstructTypedDictionary:
			out += vformat(" dst_ptr=%s argc=%d key_script_type_ptr=%s value_script_type_ptr=%s key_builtin=%d key_native=%d value_builtin=%d value_native=%d",
					_ir_value_name(p_inst.args[p_inst.args.size() - 3]),
					(p_inst.args.size() - 3) / 2,
					_ir_value_name(p_inst.args[p_inst.args.size() - 2]),
					_ir_value_name(p_inst.args[p_inst.args.size() - 1]),
					int32_t(p_inst.imm & 0xFFFFFFFFu),
					int32_t(p_inst.aux & 0xFFFFFFFFu),
					int32_t((p_inst.imm >> 32) & 0xFFFFFFFFu),
					int32_t((p_inst.aux >> 32) & 0xFFFFFFFFu));
			break;
		case IROp::AddI64:
		case IROp::AddF64:
		case IROp::NegI64:
		case IROp::NegF64:
		case IROp::MulI64:
		case IROp::MulF64:
		case IROp::SubI64:
		case IROp::SubF64:
		case IROp::DivI64:
		case IROp::EqI64:
		case IROp::EqF64:
		case IROp::NeI64:
		case IROp::NeF64:
		case IROp::LtI64:
		case IROp::LtF64:
		case IROp::LeI64:
		case IROp::LeF64:
		case IROp::GtI64:
		case IROp::GtF64:
		case IROp::GeI64:
		case IROp::GeF64:
			if (p_inst.op == IROp::AddI64 && p_inst.args.size() == 1) {
				out += vformat(" dst=%s a=%s imm=%d", _ir_value_name(p_inst.dst), _ir_value_name(p_inst.args[0]), int64_t(p_inst.imm));
			} else if (p_inst.args.size() == 1) {
				out += vformat(" dst=%s a=%s", _ir_value_name(p_inst.dst), _ir_value_name(p_inst.args[0]));
			} else {
				out += vformat(" dst=%s a=%s b=%s", _ir_value_name(p_inst.dst), _ir_value_name(p_inst.args[0]), _ir_value_name(p_inst.args[1]));
			}
			break;
		case IROp::Assign:
			out += vformat(" dst_ptr=%s src_ptr=%s", _ir_value_name(p_inst.args[0]), _ir_value_name(p_inst.args[1]));
			if (p_inst.aux != Variant::NIL) {
				out += vformat(" type=%d", p_inst.aux);
			}
			break;
		case IROp::AssignNull:
		case IROp::AssignTrue:
		case IROp::AssignFalse:
			out += vformat(" dst_ptr=%s", _ir_value_name(p_inst.args[0]));
			break;
		case IROp::AssignTypedBuiltin:
			out += vformat(" dst_ptr=%s src_ptr=%s builtin=%d",
					_ir_value_name(p_inst.args[0]),
					_ir_value_name(p_inst.args[1]),
					int32_t(p_inst.imm));
			break;
		case IROp::AssignTypedNative:
		case IROp::AssignTypedScript:
			out += vformat(" dst_ptr=%s src_ptr=%s type_ptr=%s",
					_ir_value_name(p_inst.args[0]),
					_ir_value_name(p_inst.args[1]),
					_ir_value_name(p_inst.args[2]));
			break;
		case IROp::AssignTypedArray:
			out += vformat(" dst_ptr=%s src_ptr=%s script_type_ptr=%s builtin=%d native=%d",
					_ir_value_name(p_inst.args[0]),
					_ir_value_name(p_inst.args[1]),
					_ir_value_name(p_inst.args[2]),
					int32_t(p_inst.imm),
					int32_t(p_inst.aux));
			break;
		case IROp::AssignTypedDictionary:
			out += vformat(" dst_ptr=%s src_ptr=%s key_script_type_ptr=%s value_script_type_ptr=%s key_builtin=%d key_native=%d value_builtin=%d value_native=%d",
					_ir_value_name(p_inst.args[0]),
					_ir_value_name(p_inst.args[1]),
					_ir_value_name(p_inst.args[2]),
					_ir_value_name(p_inst.args[3]),
					int32_t(p_inst.imm & 0xFFFFFFFFu),
					int32_t(p_inst.aux & 0xFFFFFFFFu),
					int32_t((p_inst.imm >> 32) & 0xFFFFFFFFu),
					int32_t((p_inst.aux >> 32) & 0xFFFFFFFFu));
			break;
		case IROp::TypeAdjust:
			out += vformat(" dst_ptr=%s type=%d",
					_ir_value_name(p_inst.args[0]),
					int32_t(p_inst.imm));
			break;
		case IROp::GetNamed:
			out += vformat(" src_ptr=%s dst_ptr=%s name=%d", _ir_value_name(p_inst.args[0]), _ir_value_name(p_inst.args[1]), p_inst.imm);
			break;
		case IROp::SetNamed:
			out += vformat(" dst_ptr=%s src_ptr=%s name=%d", _ir_value_name(p_inst.args[0]), _ir_value_name(p_inst.args[1]), p_inst.imm);
			break;
		case IROp::GetNamedValidated:
			out += vformat(" src_ptr=%s dst_ptr=%s", _ir_value_name(p_inst.args[0]), _ir_value_name(p_inst.args[1]));
			out += " getter=" + itos((int64_t)p_inst.imm);
			break;
				case IROp::SetNamedValidated:
					out += vformat(" dst_ptr=%s src_ptr=%s", _ir_value_name(p_inst.args[0]), _ir_value_name(p_inst.args[1]));
					out += " setter=" + itos((int64_t)p_inst.imm);
					break;
		case IROp::GetMember:
			out += vformat(" dst_ptr=%s name=%d", _ir_value_name(p_inst.args[0]), p_inst.imm);
			break;
		case IROp::SetMember:
			out += vformat(" src_ptr=%s name=%d", _ir_value_name(p_inst.args[0]), p_inst.imm);
			break;
		case IROp::GetKeyed:
			out += vformat(" src_ptr=%s key_ptr=%s dst_ptr=%s", _ir_value_name(p_inst.args[0]), _ir_value_name(p_inst.args[1]), _ir_value_name(p_inst.args[2]));
			break;
		case IROp::SetKeyed:
			out += vformat(" dst_ptr=%s key_ptr=%s src_ptr=%s", _ir_value_name(p_inst.args[0]), _ir_value_name(p_inst.args[1]), _ir_value_name(p_inst.args[2]));
			break;
		case IROp::GetIndexedValidated:
			out += vformat(" src_ptr=%s index_ptr=%s dst_ptr=%s", _ir_value_name(p_inst.args[0]), _ir_value_name(p_inst.args[1]), _ir_value_name(p_inst.args[2]));
			out += " getter=" + itos((int64_t)p_inst.imm);
			break;
		case IROp::SetIndexedValidated:
			out += vformat(" dst_ptr=%s index_ptr=%s src_ptr=%s", _ir_value_name(p_inst.args[0]), _ir_value_name(p_inst.args[1]), _ir_value_name(p_inst.args[2]));
			out += " setter=" + itos((int64_t)p_inst.imm);
			break;
		case IROp::GetKeyedValidated:
			out += vformat(" src_ptr=%s key_ptr=%s dst_ptr=%s", _ir_value_name(p_inst.args[0]), _ir_value_name(p_inst.args[1]), _ir_value_name(p_inst.args[2]));
			out += " getter=" + itos((int64_t)p_inst.imm);
			break;
		case IROp::SetKeyedValidated:
			out += vformat(" dst_ptr=%s key_ptr=%s src_ptr=%s", _ir_value_name(p_inst.args[0]), _ir_value_name(p_inst.args[1]), _ir_value_name(p_inst.args[2]));
			out += " setter=" + itos((int64_t)p_inst.imm);
			break;
		case IROp::CallBuiltinValidated:
			out += vformat(" base_ptr=%s dst_ptr=%s argc=%d", _ir_value_name(p_inst.args[p_inst.args.size() - 2]), _ir_value_name(p_inst.args[p_inst.args.size() - 1]), p_inst.args.size() - 2);
			out += " method=" + itos((int64_t)p_inst.imm);
			break;
		case IROp::CallMethodBind:
			out += vformat(" base_ptr=%s dst_ptr=%s argc=%d", _ir_value_name(p_inst.args[p_inst.args.size() - 2]), _ir_value_name(p_inst.args[p_inst.args.size() - 1]), p_inst.args.size() - 2);
			out += " method=" + itos((int64_t)p_inst.imm);
			break;
		case IROp::CallMethodBindValidated:
			out += vformat(" base_ptr=%s dst_ptr=%s argc=%d", _ir_value_name(p_inst.args[p_inst.args.size() - 2]), _ir_value_name(p_inst.args[p_inst.args.size() - 1]), p_inst.args.size() - 2);
			out += " method=" + itos((int64_t)p_inst.imm);
			break;
		case IROp::ConstructTypedArray:
			out += vformat(" dst_ptr=%s argc=%d script_type_ptr=%s builtin=%d native=%d",
					_ir_value_name(p_inst.args[p_inst.args.size() - 2]),
					p_inst.args.size() - 2,
					_ir_value_name(p_inst.args[p_inst.args.size() - 1]),
					p_inst.imm,
					p_inst.aux);
			break;
		case IROp::Call:
			out += vformat(" base_ptr=%s dst_ptr=%s argc=%d method=%d",
					_ir_value_name(p_inst.args[p_inst.args.size() - 2]),
					_ir_value_name(p_inst.args[p_inst.args.size() - 1]),
					p_inst.args.size() - 2,
					p_inst.imm);
			break;
		case IROp::CallSelf:
			out += vformat(" dst_ptr=%s argc=%d method=%d",
					_ir_value_name(p_inst.args[p_inst.args.size() - 1]),
					p_inst.args.size() - 1,
					p_inst.imm);
			break;
		case IROp::CallUtility:
			out += vformat(" dst_ptr=%s argc=%d function=%d",
					_ir_value_name(p_inst.args[p_inst.args.size() - 1]),
					p_inst.args.size() - 1,
					p_inst.imm);
			break;
		case IROp::CallGDScriptUtility:
			out += vformat(" dst_ptr=%s argc=%d function=%d",
					_ir_value_name(p_inst.args[p_inst.args.size() - 1]),
					p_inst.args.size() - 1,
					p_inst.imm);
			break;
		case IROp::CallBuiltinStatic:
			out += vformat(" dst_ptr=%s argc=%d type=%d method=%d",
					_ir_value_name(p_inst.args[p_inst.args.size() - 1]),
					p_inst.args.size() - 1,
					p_inst.aux,
					p_inst.imm);
			break;
		case IROp::CallUtilityValidated:
			out += vformat(" dst_ptr=%s argc=%d", _ir_value_name(p_inst.args[p_inst.args.size() - 1]), p_inst.args.size() - 1);
			out += " function=" + itos((int64_t)p_inst.imm);
			break;
		case IROp::CallBinOp:
			out += vformat(" a=%s b=%s dst=%s", _ir_value_name(p_inst.args[0]), _ir_value_name(p_inst.args[1]), _ir_value_name(p_inst.args[2]));
			break;
		case IROp::CallOperator:
			out += vformat(" op=%d a=%s b=%s dst=%s", p_inst.imm, _ir_value_name(p_inst.args[0]), _ir_value_name(p_inst.args[1]), _ir_value_name(p_inst.args[2]));
			break;
		case IROp::IterateBegin:
			out += vformat(" dst=%s container_ptr=%s counter_ptr=%s iterator_ptr=%s",
					_ir_value_name(p_inst.dst),
					_ir_value_name(p_inst.args[0]),
					_ir_value_name(p_inst.args[1]),
					_ir_value_name(p_inst.args[2]));
			break;
		case IROp::Iterate:
			out += vformat(" dst=%s container_ptr=%s counter_ptr=%s iterator_ptr=%s",
					_ir_value_name(p_inst.dst),
					_ir_value_name(p_inst.args[0]),
					_ir_value_name(p_inst.args[1]),
					_ir_value_name(p_inst.args[2]));
			break;
		case IROp::Booleanize:
			out += vformat(" dst=%s ptr=%s", _ir_value_name(p_inst.dst), _ir_value_name(p_inst.args[0]));
			if (p_inst.imm != Variant::NIL) {
				out += vformat(" type=%d", p_inst.imm);
			}
			break;
		case IROp::Jump:
			out += vformat(" target=%s", _ir_label_name(p_inst.imm));
			break;
		case IROp::JumpCc:
			out += vformat(" cc=%s", _ir_cond_name(IRCond(p_inst.aux)));
			if (p_inst.args.size() == 1) {
				out += vformat(" value=%s target=%s",
						_ir_value_name(p_inst.args[0]),
						_ir_label_name(p_inst.imm));
			} else {
				out += vformat(" a=%s b=%s target=%s",
						_ir_value_name(p_inst.args[0]),
						_ir_value_name(p_inst.args[1]),
						_ir_label_name(p_inst.imm));
			}
			break;
		case IROp::Ret:
			out += vformat(" value=%s", _ir_value_name(p_inst.args[0]));
			break;
		case IROp::RetVariant:
			out += vformat(" src_ptr=%s", _ir_value_name(p_inst.args[0]));
			break;
		case IROp::RetTypedBuiltin:
			out += vformat(" src_ptr=%s type=%d", _ir_value_name(p_inst.args[0]), int32_t(p_inst.imm));
			break;
		case IROp::RetTypedArray:
			out += vformat(" src_ptr=%s script_type_ptr=%s builtin=%d native=%d",
					_ir_value_name(p_inst.args[0]),
					_ir_value_name(p_inst.args[1]),
					int32_t(p_inst.imm),
					int32_t(p_inst.aux));
			break;
		case IROp::RetTypedDictionary:
			out += vformat(" src_ptr=%s key_script_type_ptr=%s value_script_type_ptr=%s key_builtin=%d key_native=%d value_builtin=%d value_native=%d",
					_ir_value_name(p_inst.args[0]),
					_ir_value_name(p_inst.args[1]),
					_ir_value_name(p_inst.args[2]),
					int32_t(p_inst.imm & 0xFFFFFFFFu),
					int32_t(p_inst.aux & 0xFFFFFFFFu),
					int32_t((p_inst.imm >> 32) & 0xFFFFFFFFu),
					int32_t((p_inst.aux >> 32) & 0xFFFFFFFFu));
			break;
		case IROp::StoreI64:
		case IROp::StoreF64:
			out += vformat(" src=%s mem=%s", _ir_value_name(p_inst.args[0]), _ir_mem_name(p_inst.mem_loc, p_max_locals));
			break;
		case IROp::StoreType:
			out += vformat(" type=%d mem=%s", p_inst.imm, _ir_mem_name(p_inst.mem_loc, p_max_locals));
			break;
	}

	return out;
}

static void _ir_debug_print_blocks(const char *p_title, const Vector<IRBlock> &p_blocks, int p_max_locals) {
#if defined(DEBUG_ENABLED) && !defined(TESTS_ENABLED)
	print_line(p_title);
	for (const IRBlock &block : p_blocks) {
		String block_header = vformat("Block %s", _ir_block_name(block.id));
		if (block.has_label) {
			block_header += vformat(" (%s)", _ir_label_name(block.label.id));
		}
		print_line(block_header);
		for (const IRInst &inst : block.code) {
			print_line("  " + _ir_inst_to_string(inst, p_max_locals));
		}
	}
#endif
}

JitRuntimeManager::JitRuntimeManager() : pc(&cc, runtime.cpu_features(), runtime.cpu_hints()),
		logger(stdout) {
}

JitRuntimeManager *JitRuntimeManager::get_singleton() {
	if (!singleton) {
		create_singleton();
	}
	return singleton;
}

void JitRuntimeManager::create_singleton() {
	if (!singleton) {
		singleton = memnew(JitRuntimeManager);
		singleton->code.init(singleton->runtime.environment(), singleton->runtime.cpu_features());
		singleton->code.attach(&singleton->cc);
#if defined(DEBUG_ENABLED) && !defined(TESTS_ENABLED)
		singleton->code.set_logger(&singleton->logger);
		singleton->cc.add_diagnostic_options(asmjit::DiagnosticOptions::kRAAnnotate);
#endif
	}
}

void JitRuntimeManager::destroy_singleton() {
	if (singleton) {
		memdelete(singleton);
	}
}

void JitRuntimeManager::reset() {
	code.reinit();
}

void JitRuntimeManager::compile(IRBuilder ir, GDScriptFunction *func, int max_locals) {
	function = func;
	asmjit::FuncNode *func_node = pc.add_func(asmjit::FuncSignature::build<void, Variant *, Variant *, Variant *, Variant *, uint64_t>());

	result_ptr = pc.new_gpz("result_ptr");
	stack_ptr = pc.new_gpz("stack_ptr");
	members_ptr = pc.new_gpz("members_ptr");
	constants_ptr = pc.new_gpz("constants_ptr");
	defarg_ptr = pc.new_gpz("defarg_ptr");

	func_node->set_arg(0, result_ptr);
	func_node->set_arg(1, stack_ptr);
	func_node->set_arg(2, members_ptr);
	func_node->set_arg(3, constants_ptr);
	func_node->set_arg(4, defarg_ptr);

	const IROptimizationResult opt_result = optimize_ir(ir);
	const Vector<IRBlock> &opt_blocks = opt_result.optimized_blocks;

	_ir_debug_print_blocks("Raw IR:", opt_result.raw_blocks, max_locals);
	_ir_debug_print_blocks("Optimized IR:", opt_blocks, max_locals);

	const HashMap<uint32_t, IRValueKind> &value_kind_map = opt_result.value_kind_map;

	HashMap<uint32_t, Gp> gp_map;
	HashMap<uint32_t, Vec> vec_map;
	HashMap<uint32_t, asmjit::Label> lmap;

	auto G = [&](const ValueId v) -> const Gp & {
		if (const auto it = gp_map.find(v.id); it != gp_map.end()) {
			return it->value;
		}

		const Gp r = cc.new_gp64();
		gp_map[v.id] = r;
		return gp_map[v.id];
	};

	auto F = [&](const ValueId v) -> const Vec & {
		if (const auto it = vec_map.find(v.id); it != vec_map.end()) {
			return it->value;
		}

		const Vec r = pc.new_vec128_f64x1();
		vec_map[v.id] = r;
		return vec_map[v.id];
	};

	auto L = [&](uint32_t label_id) -> asmjit::Label {
		if (const auto it = lmap.find(label_id); it != lmap.end()) {
			return it->value;
		}

		const asmjit::Label label = pc.new_label();
		lmap[label_id] = label;
		return label;
	};

	auto addrOf = [&](const GDScriptCodeGenerator::Address &p_address, const int offset = 0) -> Mem {
		const auto [address_type, address_index] = decode_address_index(p_address.address);
		constexpr int variant_size = sizeof(Variant);

		if (p_address.mode == GDScriptCodeGenerator::Address::SELF) {
			return mem_ptr(stack_ptr, GDScriptFunction::ADDR_STACK_SELF * variant_size + offset);
		}
		else if (p_address.mode == GDScriptCodeGenerator::Address::CLASS) {
			return mem_ptr(stack_ptr, GDScriptFunction::ADDR_STACK_CLASS * variant_size + offset);
		}
		else if (p_address.mode == GDScriptCodeGenerator::Address::NIL) {
			return mem_ptr(stack_ptr, GDScriptFunction::ADDR_STACK_NIL * variant_size + offset);
		}
		else if (p_address.mode == GDScriptCodeGenerator::Address::TEMPORARY) {
			return mem_ptr(stack_ptr, (address_index + max_locals + 3) * variant_size + offset);
		}
		else if (p_address.mode == GDScriptCodeGenerator::Address::CONSTANT) {
			return mem_ptr(constants_ptr, address_index * variant_size + offset);
		}
		else if (p_address.mode == GDScriptCodeGenerator::Address::MEMBER) {
			return mem_ptr(members_ptr, address_index * variant_size + offset);
		}

		return mem_ptr(stack_ptr, p_address.address * variant_size + offset);
	};

	auto instructionArgsPtr = [&]() -> Gp {
		constexpr int variant_size = sizeof(Variant);
		Gp args_ptr = pc.new_gp64();
		pc.add(args_ptr, stack_ptr, function->_stack_size * variant_size);
		return args_ptr;
	};

	for (const IRBlock &block : opt_blocks) {
		if (block.has_label) {
			pc.bind(L(block.label.id));
		}

		for (const IRInst &inst : block.code) {
			switch (inst.op) {
				case IROp::LoadParam:
					pc.load_i64(G(inst.dst), addrOf(inst.mem_loc, 8));
					break;

				case IROp::LoadDefArg:
					pc.mov(G(inst.dst), defarg_ptr);
					break;

				case IROp::LoadF64:
					pc.v_loadu64_f64(F(inst.dst), addrOf(inst.mem_loc, 8));
					break;

				case IROp::LoadRealMemberF64:
#ifdef REAL_T_IS_DOUBLE
					if (inst.args.is_empty()) {
						pc.v_loadu64_f64(F(inst.dst), addrOf(inst.mem_loc, 8 + int32_t(inst.imm)));
					} else {
						pc.v_loadu64_f64(F(inst.dst), mem_ptr(G(inst.args[0]), 8 + int32_t(inst.imm)));
					}
#else
				{
					const Vec scalar_f32 = pc.new_vec128_f32x1();
					if (inst.args.is_empty()) {
						pc.v_loadu32_f32(scalar_f32, addrOf(inst.mem_loc, 8 + int32_t(inst.imm)));
					} else {
						pc.v_loadu32_f32(scalar_f32, mem_ptr(G(inst.args[0]), 8 + int32_t(inst.imm)));
					}
					pc.s_cvt_f32_to_f64(F(inst.dst), scalar_f32);
				}
#endif
					break;

				case IROp::StoreRealMemberF64:
#ifdef REAL_T_IS_DOUBLE
					if (inst.args.size() == 1) {
						pc.v_storeu64_f64(addrOf(inst.mem_loc, 8 + int32_t(inst.imm)), F(inst.args[0]));
					} else {
						pc.v_storeu64_f64(mem_ptr(G(inst.args[0]), 8 + int32_t(inst.imm)), F(inst.args[1]));
					}
#else
				{
					const Vec scalar_f32 = pc.new_vec128_f32x1();
					pc.s_cvt_f64_to_f32(scalar_f32, F(inst.args[inst.args.size() - 1]));
					if (inst.args.size() == 1) {
						pc.v_storeu32_f32(addrOf(inst.mem_loc, 8 + int32_t(inst.imm)), scalar_f32);
					} else {
						pc.v_storeu32_f32(mem_ptr(G(inst.args[0]), 8 + int32_t(inst.imm)), scalar_f32);
					}
				}
#endif
					break;

				case IROp::Construct: {
					const int argc = inst.args.size() - 1;
					Gp args_ptr = instructionArgsPtr();
					Gp type = pc.new_gp64();
					pc.mov(type, inst.imm);
					for (int i = 0; i < argc; i++) {
						pc.store_u64(mem_ptr(args_ptr, i * int(sizeof(Variant *))), G(inst.args[i]));
					}
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_construct, asmjit::FuncSignature::build<void, Variant *, uint64_t, int, int>());
					invoke_node->set_arg(0, G(inst.args[argc]));
					invoke_node->set_arg(1, args_ptr);
					invoke_node->set_arg(2, argc);
					invoke_node->set_arg(3, type.r32());
				} break;

				case IROp::ConstructValidated: {
					const int argc = inst.args.size() - 1;
					Gp args_ptr = instructionArgsPtr();
					Gp constructor = pc.new_gp64();
					pc.mov(constructor, inst.imm);
					for (int i = 0; i < argc; i++) {
						pc.store_u64(mem_ptr(args_ptr, i * int(sizeof(Variant *))), G(inst.args[i]));
					}
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_construct_validated, asmjit::FuncSignature::build<void, Variant *, uint64_t, Variant::ValidatedConstructor>());
					invoke_node->set_arg(0, G(inst.args[argc]));
					invoke_node->set_arg(1, args_ptr);
					invoke_node->set_arg(2, constructor);
				} break;

				case IROp::ConstructArray: {
					const int argc = inst.args.size() - 1;
					Gp args_ptr = instructionArgsPtr();
					for (int i = 0; i < argc; i++) {
						pc.store_u64(mem_ptr(args_ptr, i * int(sizeof(Variant *))), G(inst.args[i]));
					}
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_construct_array, asmjit::FuncSignature::build<void, Variant *, uint64_t, int>());
					invoke_node->set_arg(0, G(inst.args[argc]));
					invoke_node->set_arg(1, args_ptr);
					invoke_node->set_arg(2, argc);
				} break;

				case IROp::ConstructDictionary: {
					const int argc = (inst.args.size() - 1) / 2;
					Gp args_ptr = instructionArgsPtr();
					for (int i = 0; i < argc * 2; i++) {
						pc.store_u64(mem_ptr(args_ptr, i * int(sizeof(Variant *))), G(inst.args[i]));
					}
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_construct_dictionary, asmjit::FuncSignature::build<void, Variant *, uint64_t, int>());
					invoke_node->set_arg(0, G(inst.args[argc * 2]));
					invoke_node->set_arg(1, args_ptr);
					invoke_node->set_arg(2, argc);
				} break;

				case IROp::LoadPtr:
					pc.lea(G(inst.dst), addrOf(inst.mem_loc));
					break;

				case IROp::ZeroI64:
					pc.mov(G(inst.dst), 0);
					break;

				case IROp::AddI64:
					if (inst.args.size() == 1) {
						pc.add(G(inst.dst), G(inst.args[0]), asmjit::Imm(inst.imm));
					} else {
						pc.add(G(inst.dst), G(inst.args[0]), G(inst.args[1]));
					}
					break;

				case IROp::AddF64:
					pc.s_add_f64(F(inst.dst), F(inst.args[0]), F(inst.args[1]));
					break;

				case IROp::NegI64:
					pc.neg(G(inst.dst), G(inst.args[0]));
					break;

				case IROp::NegF64:
					pc.s_neg_f64(F(inst.dst), F(inst.args[0]));
					break;

				case IROp::MulI64:
					pc.mul(G(inst.dst), G(inst.args[0]), G(inst.args[1]));
					break;

				case IROp::MulF64:
					pc.s_mul_f64(F(inst.dst), F(inst.args[0]), F(inst.args[1]));
					break;

				case IROp::SubI64:
					pc.sub(G(inst.dst), G(inst.args[0]), G(inst.args[1]));
					break;

				case IROp::SubF64:
					pc.s_sub_f64(F(inst.dst), F(inst.args[0]), F(inst.args[1]));
					break;

				case IROp::DivI64:  // not exist
#ifdef ASMJIT_UJIT_AARCH64
					cc.sdiv(G(inst.dst), G(inst.args[0]), G(inst.args[1]));
#else
					cc.idiv(G(inst.dst), G(inst.args[0]), G(inst.args[1]));
#endif
					break;

				case IROp::EqI64: {
					Gp temp = pc.new_gp64();
					pc.xor_(G(inst.dst), G(inst.dst), G(inst.dst));
					pc.mov(temp, 1);
					pc.cmov(G(inst.dst), temp, cmp_eq(G(inst.args[0]), G(inst.args[1])));
				} break;

				case IROp::EqF64:
				{
					const Vec mask = pc.new_vec128_f64x1();
					const Gp bits = pc.new_gp64();
					pc.s_cmp_eq_f64(mask, F(inst.args[0]), F(inst.args[1]));
					pc.s_extract_u64(bits, mask, 0);
					pc.and_(G(inst.dst), bits, asmjit::Imm(1));
				} break;

				case IROp::NeI64: {
					Gp temp = pc.new_gp64();
					pc.xor_(G(inst.dst), G(inst.dst), G(inst.dst));
					pc.mov(temp, 1);
					pc.cmov(G(inst.dst), temp, cmp_ne(G(inst.args[0]), G(inst.args[1])));
				} break;

				case IROp::NeF64:
				{
					const Vec mask = pc.new_vec128_f64x1();
					const Gp bits = pc.new_gp64();
					pc.s_cmp_ne_f64(mask, F(inst.args[0]), F(inst.args[1]));
					pc.s_extract_u64(bits, mask, 0);
					pc.and_(G(inst.dst), bits, asmjit::Imm(1));
				} break;

				case IROp::LtI64: {
					Gp temp = pc.new_gp64();
					pc.xor_(G(inst.dst), G(inst.dst), G(inst.dst));
					pc.mov(temp, 1);
					pc.cmov(G(inst.dst), temp, scmp_lt(G(inst.args[0]), G(inst.args[1])));
				} break;

				case IROp::LtF64:
				{
					const Vec mask = pc.new_vec128_f64x1();
					const Gp bits = pc.new_gp64();
					pc.s_cmp_lt_f64(mask, F(inst.args[0]), F(inst.args[1]));
					pc.s_extract_u64(bits, mask, 0);
					pc.and_(G(inst.dst), bits, asmjit::Imm(1));
				} break;

				case IROp::LeI64: {
					Gp temp = pc.new_gp64();
					pc.xor_(G(inst.dst), G(inst.dst), G(inst.dst));
					pc.mov(temp, 1);
					pc.cmov(G(inst.dst), temp, scmp_le(G(inst.args[0]), G(inst.args[1])));
				} break;

				case IROp::LeF64:
				{
					const Vec mask = pc.new_vec128_f64x1();
					const Gp bits = pc.new_gp64();
					pc.s_cmp_le_f64(mask, F(inst.args[0]), F(inst.args[1]));
					pc.s_extract_u64(bits, mask, 0);
					pc.and_(G(inst.dst), bits, asmjit::Imm(1));
				} break;

				case IROp::GtI64: {
					Gp temp = pc.new_gp64();
					pc.xor_(G(inst.dst), G(inst.dst), G(inst.dst));
					pc.mov(temp, 1);
					pc.cmov(G(inst.dst), temp, scmp_gt(G(inst.args[0]), G(inst.args[1])));
				} break;

				case IROp::GtF64:
				{
					const Vec mask = pc.new_vec128_f64x1();
					const Gp bits = pc.new_gp64();
					pc.s_cmp_gt_f64(mask, F(inst.args[0]), F(inst.args[1]));
					pc.s_extract_u64(bits, mask, 0);
					pc.and_(G(inst.dst), bits, asmjit::Imm(1));
				} break;

				case IROp::GeI64: {
					Gp temp = pc.new_gp64();
					pc.xor_(G(inst.dst), G(inst.dst), G(inst.dst));
					pc.mov(temp, 1);
					pc.cmov(G(inst.dst), temp, scmp_ge(G(inst.args[0]), G(inst.args[1])));
				} break;

				case IROp::GeF64:
				{
					const Vec mask = pc.new_vec128_f64x1();
					const Gp bits = pc.new_gp64();
					pc.s_cmp_ge_f64(mask, F(inst.args[0]), F(inst.args[1]));
					pc.s_extract_u64(bits, mask, 0);
					pc.and_(G(inst.dst), bits, asmjit::Imm(1));
				} break;

				case IROp::Assign: {
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_assign, asmjit::FuncSignature::build<void, Variant *, const Variant *>());
					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, G(inst.args[1]));
					break;
				}

				case IROp::AssignNull: {
					asmjit::InvokeNode *invoke_node = emit_invoke(+[](Variant *dst) {
						*dst = Variant();
					}, asmjit::FuncSignature::build<void, Variant *>());
					invoke_node->set_arg(0, G(inst.args[0]));
					break;
				}

				case IROp::AssignTrue: {
					asmjit::InvokeNode *invoke_node = emit_invoke(+[](Variant *dst) {
						*dst = true;
					}, asmjit::FuncSignature::build<void, Variant *>());
					invoke_node->set_arg(0, G(inst.args[0]));
					break;
				}

				case IROp::AssignFalse: {
					asmjit::InvokeNode *invoke_node = emit_invoke(+[](Variant *dst) {
						*dst = false;
					}, asmjit::FuncSignature::build<void, Variant *>());
					invoke_node->set_arg(0, G(inst.args[0]));
					break;
				}

				case IROp::AssignTypedBuiltin: {
					Gp builtin_type = pc.new_gp64();
					pc.mov(builtin_type, uint32_t(inst.imm));
					asmjit::InvokeNode *invoke_node = emit_invoke(+[](Variant *dst, const Variant *src, int builtin_type_arg) {
						const Variant::Type builtin_type = Variant::Type(builtin_type_arg);
						if (src->get_type() != builtin_type) {
#ifdef DEBUG_ENABLED
							if (Variant::can_convert_strict(src->get_type(), builtin_type)) {
#endif
								Callable::CallError ce;
								Variant::construct(builtin_type, *dst, const_cast<const Variant **>(&src), 1, ce);
							} else {
#ifdef DEBUG_ENABLED
								ERR_FAIL_MSG("Trying to assign value of type '" + Variant::get_type_name(src->get_type()) +
										"' to a variable of type '" + Variant::get_type_name(builtin_type) + "'.");
							}
						} else {
#endif
							*dst = *src;
						}
					}, asmjit::FuncSignature::build<void, Variant *, const Variant *, int>());
					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, G(inst.args[1]));
					invoke_node->set_arg(2, builtin_type.r32());
					break;
				}

				case IROp::AssignTypedNative: {
					asmjit::InvokeNode *invoke_node = emit_invoke(+[](Variant *dst, const Variant *src, const Variant *type) {
#ifdef DEBUG_ENABLED
						GDScriptNativeClass *nc = Object::cast_to<GDScriptNativeClass>(type->operator Object *());
						ERR_FAIL_NULL(nc);
						if (src->get_type() != Variant::OBJECT && src->get_type() != Variant::NIL) {
							ERR_FAIL_MSG("Trying to assign value of type '" + Variant::get_type_name(src->get_type()) +
									"' to a variable of type '" + nc->get_name() + "'.");
						}

						if (src->get_type() == Variant::OBJECT) {
							bool was_freed = false;
							Object *src_obj = src->get_validated_object_with_check(was_freed);
							if (!src_obj && was_freed) {
								ERR_FAIL_MSG("Trying to assign invalid previously freed instance.");
							}

							if (src_obj && !ClassDB::is_parent_class(src_obj->get_class_name(), nc->get_name())) {
								ERR_FAIL_MSG("Trying to assign value of type '" + src_obj->get_class_name() +
										"' to a variable of type '" + nc->get_name() + "'.");
							}
						}
#endif
						*dst = *src;
					}, asmjit::FuncSignature::build<void, Variant *, const Variant *, const Variant *>());
					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, G(inst.args[1]));
					invoke_node->set_arg(2, G(inst.args[2]));
					break;
				}

				case IROp::AssignTypedScript: {
					asmjit::InvokeNode *invoke_node = emit_invoke(+[](Variant *dst, const Variant *src, const Variant *type) {
#ifdef DEBUG_ENABLED
						Script *base_type = Object::cast_to<Script>(type->operator Object *());
						ERR_FAIL_NULL(base_type);

						if (src->get_type() != Variant::OBJECT && src->get_type() != Variant::NIL) {
							ERR_FAIL_MSG("Trying to assign a non-object value to a variable of type '" + base_type->get_path().get_file() + "'.");
						}

						if (src->get_type() == Variant::OBJECT) {
							bool was_freed = false;
							Object *val_obj = src->get_validated_object_with_check(was_freed);
							if (!val_obj && was_freed) {
								ERR_FAIL_MSG("Trying to assign invalid previously freed instance.");
							}

							if (val_obj) {
								ScriptInstance *scr_inst = val_obj->get_script_instance();
								if (!scr_inst) {
									ERR_FAIL_MSG("Trying to assign value of type '" + val_obj->get_class_name() +
											"' to a variable of type '" + base_type->get_path().get_file() + "'.");
								}

								Script *src_type = scr_inst->get_script().ptr();
								bool valid = false;
								while (src_type) {
									if (src_type == base_type) {
										valid = true;
										break;
									}
									src_type = src_type->get_base_script().ptr();
								}

								if (!valid) {
									ERR_FAIL_MSG("Trying to assign value of type '" + scr_inst->get_script()->get_path().get_file() +
											"' to a variable of type '" + base_type->get_path().get_file() + "'.");
								}
							}
						}
#endif
						*dst = *src;
					}, asmjit::FuncSignature::build<void, Variant *, const Variant *, const Variant *>());
					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, G(inst.args[1]));
					invoke_node->set_arg(2, G(inst.args[2]));
					break;
				}

				case IROp::AssignTypedArray: {
					Gp builtin_type = pc.new_gp64();
					Gp native_type_ptr = pc.new_gp64();
					pc.mov(builtin_type, uint32_t(inst.imm));
					pc.mov(native_type_ptr, uint64_t(&function->_global_names_ptr[uint32_t(inst.aux)]));
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_assign_typed_array, asmjit::FuncSignature::build<void, Variant *, const Variant *, const Variant *, int, const StringName *>());
					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, G(inst.args[1]));
					invoke_node->set_arg(2, G(inst.args[2]));
					invoke_node->set_arg(3, builtin_type.r32());
					invoke_node->set_arg(4, native_type_ptr);
					break;
				}

				case IROp::AssignTypedDictionary: {
					Gp key_builtin_type = pc.new_gp64();
					Gp key_native_type_ptr = pc.new_gp64();
					Gp value_builtin_type = pc.new_gp64();
					Gp value_native_type_ptr = pc.new_gp64();
					pc.mov(key_builtin_type, uint32_t(inst.imm & 0xFFFFFFFFu));
					pc.mov(key_native_type_ptr, uint64_t(&function->_global_names_ptr[uint32_t(inst.aux & 0xFFFFFFFFu)]));
					pc.mov(value_builtin_type, uint32_t((inst.imm >> 32) & 0xFFFFFFFFu));
					pc.mov(value_native_type_ptr, uint64_t(&function->_global_names_ptr[uint32_t((inst.aux >> 32) & 0xFFFFFFFFu)]));
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_assign_typed_dictionary, asmjit::FuncSignature::build<void, Variant *, const Variant *, const Variant *, int, const StringName *, const Variant *, int, const StringName *>());
					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, G(inst.args[1]));
					invoke_node->set_arg(2, G(inst.args[2]));
					invoke_node->set_arg(3, key_builtin_type.r32());
					invoke_node->set_arg(4, key_native_type_ptr);
					invoke_node->set_arg(5, G(inst.args[3]));
					invoke_node->set_arg(6, value_builtin_type.r32());
					invoke_node->set_arg(7, value_native_type_ptr);
					break;
				}

				case IROp::TypeAdjust: {
					Gp new_type = pc.new_gp64();
					pc.mov(new_type, uint32_t(inst.imm));
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_type_adjust, asmjit::FuncSignature::build<void, Variant *, int>());
					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, new_type.r32());
				} break;

				case IROp::GetNamed: {
					Gp name_ptr = pc.new_gp64();
					pc.mov(name_ptr, uint64_t(&function->_global_names_ptr[inst.imm]));
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_get_named, asmjit::FuncSignature::build<void, const Variant *, Variant *, const StringName *>());
					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, G(inst.args[1]));
					invoke_node->set_arg(2, name_ptr);
				} break;

				case IROp::SetNamed: {
					Gp name_ptr = pc.new_gp64();
					pc.mov(name_ptr, uint64_t(&function->_global_names_ptr[inst.imm]));
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_set_named, asmjit::FuncSignature::build<void, Variant *, const Variant *, const StringName *>());
					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, G(inst.args[1]));
					invoke_node->set_arg(2, name_ptr);
				} break;

				case IROp::GetNamedValidated: {
					asmjit::InvokeNode *invoke_node = emit_invoke(inst.imm, asmjit::FuncSignature::build<void, const Variant *, Variant *>());
					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, G(inst.args[1]));
				} break;

				case IROp::SetNamedValidated: {
					asmjit::InvokeNode *invoke_node = emit_invoke(inst.imm, asmjit::FuncSignature::build<void, Variant *, const Variant *>());
					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, G(inst.args[1]));
				} break;

				case IROp::GetMember: {
					Gp self_ptr = pc.new_gp64();
					Gp name_ptr = pc.new_gp64();
					pc.lea(self_ptr, addrOf(GDScriptCodeGenerator::Address(GDScriptCodeGenerator::Address::SELF, 0, GDScriptDataType())));
					pc.mov(name_ptr, uint64_t(&function->_global_names_ptr[inst.imm]));
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_get_member, asmjit::FuncSignature::build<void, const Variant *, Variant *, const StringName *>());
					invoke_node->set_arg(0, self_ptr);
					invoke_node->set_arg(1, G(inst.args[0]));
					invoke_node->set_arg(2, name_ptr);
				} break;

				case IROp::SetMember: {
					Gp self_ptr = pc.new_gp64();
					Gp name_ptr = pc.new_gp64();
					pc.lea(self_ptr, addrOf(GDScriptCodeGenerator::Address(GDScriptCodeGenerator::Address::SELF, 0, GDScriptDataType())));
					pc.mov(name_ptr, uint64_t(&function->_global_names_ptr[inst.imm]));
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_set_member, asmjit::FuncSignature::build<void, const Variant *, const Variant *, const StringName *>());
					invoke_node->set_arg(0, self_ptr);
					invoke_node->set_arg(1, G(inst.args[0]));
					invoke_node->set_arg(2, name_ptr);
				} break;

				case IROp::GetKeyed: {
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_get_keyed, asmjit::FuncSignature::build<void, const Variant *, const Variant *, Variant *>());
					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, G(inst.args[1]));
					invoke_node->set_arg(2, G(inst.args[2]));
				} break;

				case IROp::SetKeyed: {
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_set_keyed, asmjit::FuncSignature::build<void, Variant *, const Variant *, const Variant *>());
					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, G(inst.args[1]));
					invoke_node->set_arg(2, G(inst.args[2]));
				} break;

				case IROp::GetIndexedValidated: {
					Gp getter = pc.new_gp64();
					pc.mov(getter, inst.imm);
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_get_indexed_validated, asmjit::FuncSignature::build<void, const Variant *, const  int64_t, Variant *, uint64_t>());
					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, G(inst.args[1]));
					invoke_node->set_arg(2, G(inst.args[2]));
					invoke_node->set_arg(3, getter);
				} break;

				case IROp::SetIndexedValidated: {
					Gp setter = pc.new_gp64();
					pc.mov(setter, inst.imm);
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_set_indexed_validated, asmjit::FuncSignature::build<void, Variant *, const int64_t, const Variant *, uint64_t>());
					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, G(inst.args[1]));
					invoke_node->set_arg(2, G(inst.args[2]));
					invoke_node->set_arg(3, setter);
				} break;

				case IROp::GetKeyedValidated: {
					Gp getter = pc.new_gp64();
					pc.mov(getter, inst.imm);
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_get_keyed_validated, asmjit::FuncSignature::build<void, const Variant *, const Variant *, Variant *, uint64_t>());
					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, G(inst.args[1]));
					invoke_node->set_arg(2, G(inst.args[2]));
					invoke_node->set_arg(3, getter);
				} break;

				case IROp::SetKeyedValidated: {
					Gp setter = pc.new_gp64();
					pc.mov(setter, inst.imm);
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_set_keyed_validated, asmjit::FuncSignature::build<void, Variant *, const Variant *, const Variant *, uint64_t>());
					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, G(inst.args[1]));
					invoke_node->set_arg(2, G(inst.args[2]));
					invoke_node->set_arg(3, setter);
				} break;

				case IROp::ConstructTypedArray: {
					const int argc = inst.args.size() - 2;
					Gp args_ptr = instructionArgsPtr();
					Gp builtin_type = pc.new_gp64();
					Gp native_type_ptr = pc.new_gp64();
					pc.mov(builtin_type, inst.imm);
					pc.mov(native_type_ptr, uint64_t(&function->_global_names_ptr[inst.aux]));
					for (int i = 0; i < argc; i++) {
						pc.store_u64(mem_ptr(args_ptr, i * int(sizeof(Variant *))), G(inst.args[i]));
					}
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_construct_typed_array, asmjit::FuncSignature::build<void, Variant *, uint64_t, int, const Variant *, int, const StringName *>());
					invoke_node->set_arg(0, G(inst.args[argc]));
					invoke_node->set_arg(1, args_ptr);
					invoke_node->set_arg(2, argc);
					invoke_node->set_arg(3, G(inst.args[argc + 1]));
					invoke_node->set_arg(4, builtin_type.r32());
					invoke_node->set_arg(5, native_type_ptr);
				} break;

				case IROp::ConstructTypedDictionary: {
					const int argc = (inst.args.size() - 3) / 2;
					Gp args_ptr = instructionArgsPtr();
					Gp key_builtin_type = pc.new_gp64();
					Gp key_native_type_ptr = pc.new_gp64();
					Gp value_builtin_type = pc.new_gp64();
					Gp value_native_type_ptr = pc.new_gp64();
					pc.mov(key_builtin_type, uint32_t(inst.imm & 0xFFFFFFFFu));
					pc.mov(key_native_type_ptr, uint64_t(&function->_global_names_ptr[uint32_t(inst.aux & 0xFFFFFFFFu)]));
					pc.mov(value_builtin_type, uint32_t((inst.imm >> 32) & 0xFFFFFFFFu));
					pc.mov(value_native_type_ptr, uint64_t(&function->_global_names_ptr[uint32_t((inst.aux >> 32) & 0xFFFFFFFFu)]));
					for (int i = 0; i < argc * 2; i++) {
						pc.store_u64(mem_ptr(args_ptr, i * int(sizeof(Variant *))), G(inst.args[i]));
					}
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_construct_typed_dictionary, asmjit::FuncSignature::build<void, Variant *, uint64_t, int, const Variant *, int, const StringName *, const Variant *, int, const StringName *>());
					invoke_node->set_arg(0, G(inst.args[argc * 2]));
					invoke_node->set_arg(1, args_ptr);
					invoke_node->set_arg(2, argc);
					invoke_node->set_arg(3, G(inst.args[argc * 2 + 1]));
					invoke_node->set_arg(4, key_builtin_type.r32());
					invoke_node->set_arg(5, key_native_type_ptr);
					invoke_node->set_arg(6, G(inst.args[argc * 2 + 2]));
					invoke_node->set_arg(7, value_builtin_type.r32());
					invoke_node->set_arg(8, value_native_type_ptr);
				} break;

				case IROp::Call: {
					const int argc = inst.args.size() - 2;
					Gp args_ptr = instructionArgsPtr();
					Gp method_name_ptr = pc.new_gp64();
					pc.mov(method_name_ptr, uint64_t(&function->_global_names_ptr[inst.imm]));
					for (int i = 0; i < argc; i++) {
						pc.store_u64(mem_ptr(args_ptr, i * int(sizeof(Variant *))), G(inst.args[i]));
					}
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_call, asmjit::FuncSignature::build<void, Variant *, const StringName *, Variant *, uint64_t, int>());
					invoke_node->set_arg(0, G(inst.args[argc]));
					invoke_node->set_arg(1, method_name_ptr);
					invoke_node->set_arg(2, G(inst.args[argc + 1]));
					invoke_node->set_arg(3, args_ptr);
					invoke_node->set_arg(4, argc);
				} break;

				case IROp::CallSelf: {
					const int argc = inst.args.size() - 1;
					Gp args_ptr = instructionArgsPtr();
					Gp self_ptr = pc.new_gp64();
					Gp method_name_ptr = pc.new_gp64();
					pc.lea(self_ptr, addrOf(GDScriptCodeGenerator::Address(GDScriptCodeGenerator::Address::SELF, 0, GDScriptDataType())));
					pc.mov(method_name_ptr, uint64_t(&function->_global_names_ptr[inst.imm]));
					for (int i = 0; i < argc; i++) {
						pc.store_u64(mem_ptr(args_ptr, i * int(sizeof(Variant *))), G(inst.args[i]));
					}
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_call, asmjit::FuncSignature::build<void, Variant *, const StringName *, Variant *, uint64_t, int>());
					invoke_node->set_arg(0, self_ptr);
					invoke_node->set_arg(1, method_name_ptr);
					invoke_node->set_arg(2, G(inst.args[argc]));
					invoke_node->set_arg(3, args_ptr);
					invoke_node->set_arg(4, argc);
				} break;

				case IROp::CallUtility: {
					const int argc = inst.args.size() - 1;
					Gp args_ptr = instructionArgsPtr();
					Gp function_name_ptr = pc.new_gp64();
					pc.mov(function_name_ptr, uint64_t(&function->_global_names_ptr[inst.imm]));
					for (int i = 0; i < argc; i++) {
						pc.store_u64(mem_ptr(args_ptr, i * int(sizeof(Variant *))), G(inst.args[i]));
					}
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_call_utility, asmjit::FuncSignature::build<void, const StringName *, Variant *, uint64_t, int>());
					invoke_node->set_arg(0, function_name_ptr);
					invoke_node->set_arg(1, G(inst.args[argc]));
					invoke_node->set_arg(2, args_ptr);
					invoke_node->set_arg(3, argc);
				} break;

				case IROp::CallGDScriptUtility: {
					const int argc = inst.args.size() - 1;
					Gp args_ptr = instructionArgsPtr();
					Gp function_ptr = pc.new_gp64();
					pc.mov(function_ptr, uint64_t(function->_gds_utilities_ptr[uint32_t(inst.imm)]));
					for (int i = 0; i < argc; i++) {
						pc.store_u64(mem_ptr(args_ptr, i * int(sizeof(Variant *))), G(inst.args[i]));
					}
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_call_gdscript_utility, asmjit::FuncSignature::build<void, uint64_t, Variant *, uint64_t, int>());
					invoke_node->set_arg(0, function_ptr);
					invoke_node->set_arg(1, G(inst.args[argc]));
					invoke_node->set_arg(2, args_ptr);
					invoke_node->set_arg(3, argc);
				} break;

				case IROp::CallUtilityValidated: {
					const int argc = inst.args.size() - 1;
					Gp args_ptr = instructionArgsPtr();
					Gp function_ptr = pc.new_gp64();
					pc.mov(function_ptr, inst.imm);
					for (int i = 0; i < argc; i++) {
						pc.store_u64(mem_ptr(args_ptr, i * int(sizeof(Variant *))), G(inst.args[i]));
					}
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_call_utility_validated, asmjit::FuncSignature::build<void, uint64_t, Variant *, uint64_t, int>());
					invoke_node->set_arg(0, function_ptr);
					invoke_node->set_arg(1, G(inst.args[argc]));
					invoke_node->set_arg(2, args_ptr);
					invoke_node->set_arg(3, argc);
				} break;

				case IROp::CallBuiltinStatic: {
					const int argc = inst.args.size() - 1;
					Gp args_ptr = instructionArgsPtr();
					Gp method_name_ptr = pc.new_gp64();
					pc.mov(method_name_ptr, uint64_t(&function->_global_names_ptr[uint32_t(inst.imm)]));
					Gp builtin_type = pc.new_gp32();
					pc.mov(builtin_type, uint32_t(inst.aux));
					for (int i = 0; i < argc; i++) {
						pc.store_u64(mem_ptr(args_ptr, i * int(sizeof(Variant *))), G(inst.args[i]));
					}
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_call_builtin_static, asmjit::FuncSignature::build<void, const StringName *, int, Variant *, uint64_t, int>());
					invoke_node->set_arg(0, method_name_ptr);
					invoke_node->set_arg(1, builtin_type);
					invoke_node->set_arg(2, G(inst.args[argc]));
					invoke_node->set_arg(3, args_ptr);
					invoke_node->set_arg(4, argc);
				} break;

				case IROp::CallBuiltinValidated: {
					const int argc = inst.args.size() - 2;
					Gp args_ptr = instructionArgsPtr();
					Gp method_ptr = pc.new_gp64();
					pc.mov(method_ptr, inst.imm);
					for (int i = 0; i < argc; i++) {
						pc.store_u64(mem_ptr(args_ptr, i * int(sizeof(Variant *))), G(inst.args[i]));
					}
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_call_builtin_validated, asmjit::FuncSignature::build<void, uint64_t, Variant *, Variant *, uint64_t, int>());
					invoke_node->set_arg(0, method_ptr);
					invoke_node->set_arg(1, G(inst.args[argc]));
					invoke_node->set_arg(2, G(inst.args[argc + 1]));
					invoke_node->set_arg(3, args_ptr);
					invoke_node->set_arg(4, argc);
				} break;

				case IROp::CallMethodBind: {
					const int argc = inst.args.size() - 2;
					Gp args_ptr = instructionArgsPtr();
					Gp method_ptr = pc.new_gp64();
					pc.mov(method_ptr, inst.imm);
					for (int i = 0; i < argc; i++) {
						pc.store_u64(mem_ptr(args_ptr, i * int(sizeof(Variant *))), G(inst.args[i]));
					}
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_call_method_bind, asmjit::FuncSignature::build<void, uint64_t, const Variant *, Variant *, uint64_t, int>());
					invoke_node->set_arg(0, method_ptr);
					invoke_node->set_arg(1, G(inst.args[argc]));
					invoke_node->set_arg(2, G(inst.args[argc + 1]));
					invoke_node->set_arg(3, args_ptr);
					invoke_node->set_arg(4, argc);
				} break;

				case IROp::CallMethodBindValidated: {
					const int argc = inst.args.size() - 2;
					Gp args_ptr = instructionArgsPtr();
					Gp method_ptr = pc.new_gp64();
					pc.mov(method_ptr, inst.imm);
					for (int i = 0; i < argc; i++) {
						pc.store_u64(mem_ptr(args_ptr, i * int(sizeof(Variant *))), G(inst.args[i]));
					}
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_call_method_bind_validated, asmjit::FuncSignature::build<void, uint64_t, const Variant *, Variant *, uint64_t, int>());
					invoke_node->set_arg(0, method_ptr);
					invoke_node->set_arg(1, G(inst.args[argc]));
					invoke_node->set_arg(2, G(inst.args[argc + 1]));
					invoke_node->set_arg(3, args_ptr);
					invoke_node->set_arg(4, argc);
				} break;

				case IROp::CallBinOp: {
					asmjit::InvokeNode *invoke_node = emit_invoke(inst.imm, asmjit::FuncSignature::build<void, const Variant *, const Variant *, Variant *>());

					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, G(inst.args[1]));
					invoke_node->set_arg(2, G(inst.args[2]));
				} break;

				case IROp::CallOperator: {
					Gp op = pc.new_gp64();
					pc.mov(op, inst.imm);
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_operator, asmjit::FuncSignature::build<void, uint64_t, const Variant *, const Variant *, Variant *>());
					invoke_node->set_arg(0, op);
					invoke_node->set_arg(1, G(inst.args[0]));
					invoke_node->set_arg(2, G(inst.args[1]));
					invoke_node->set_arg(3, G(inst.args[2]));
				} break;

				case IROp::IterateBegin: {
					Gp result = pc.new_gp32();
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_iterate_begin, asmjit::FuncSignature::build<uint32_t, const Variant *, Variant *, Variant *>());
					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, G(inst.args[1]));
					invoke_node->set_arg(2, G(inst.args[2]));
					invoke_node->set_ret(0, result);
					gp_map[inst.dst.id] = result;
				} break;

				case IROp::Iterate: {
					Gp result = pc.new_gp32();
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_iterate, asmjit::FuncSignature::build<uint32_t, const Variant *, Variant *, Variant *>());
					invoke_node->set_arg(0, G(inst.args[0]));
					invoke_node->set_arg(1, G(inst.args[1]));
					invoke_node->set_arg(2, G(inst.args[2]));
					invoke_node->set_ret(0, result);
					gp_map[inst.dst.id] = result;
				} break;

				case IROp::Booleanize: {
					if (inst.imm == Variant::INT) {
						Gp value = pc.new_gp64();
						asmjit::Label bool_done = pc.new_label();
						pc.load_i64(value, mem_ptr(G(inst.args[0]), 8));
						pc.mov(G(inst.dst), 0);
						pc.j(bool_done, cmp_eq(value, asmjit::Imm(0)));
						pc.mov(G(inst.dst), 1);
						pc.bind(bool_done);
					} else {
						asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_booleanize, asmjit::FuncSignature::build<uint32_t, const Variant *>());
						invoke_node->set_arg(0, G(inst.args[0]));
						invoke_node->set_ret(0, G(inst.dst).r32());
					}
				} break;

				case IROp::Jump:
					pc.j(L(inst.imm));
					break;

				case IROp::JumpCc: {
					if (inst.args.size() == 1) {
						switch (static_cast<IRCond>(inst.aux)) {
							case IRCond::EQ:
								pc.j(L(inst.imm), test_z(G(inst.args[0])));
								break;
							case IRCond::NE:
								pc.j(L(inst.imm), test_nz(G(inst.args[0])));
								break;
							case IRCond::LT:
								pc.j(L(inst.imm), scmp_lt(G(inst.args[0]), asmjit::Imm(0)));
								break;
							case IRCond::LE:
								pc.j(L(inst.imm), scmp_le(G(inst.args[0]), asmjit::Imm(0)));
								break;
							case IRCond::GT:
								pc.j(L(inst.imm), scmp_gt(G(inst.args[0]), asmjit::Imm(0)));
								break;
							case IRCond::GE:
								pc.j(L(inst.imm), scmp_ge(G(inst.args[0]), asmjit::Imm(0)));
								break;
						}
					} else {
						const auto lhs_kind_it = value_kind_map.find(inst.args[0].id);
						const bool is_f64_cmp = lhs_kind_it != value_kind_map.end() && lhs_kind_it->value == IRValueKind::F64;
						if (is_f64_cmp) {
#ifdef ASMJIT_UJIT_AARCH64
							cc.fcmp(F(inst.args[0]), F(inst.args[1]));
							switch (static_cast<IRCond>(inst.aux)) {
								case IRCond::EQ:
									cc.b_eq(L(inst.imm));
									break;
								case IRCond::NE:
									cc.b_ne(L(inst.imm));
									break;
								case IRCond::LT:
									cc.b_lt(L(inst.imm));
									break;
								case IRCond::LE:
									cc.b_le(L(inst.imm));
									break;
								case IRCond::GT:
									cc.b_gt(L(inst.imm));
									break;
								case IRCond::GE:
									cc.b_ge(L(inst.imm));
									break;
							}
#else
							const Vec mask = pc.new_vec128_f64x1();
							const Gp bits = pc.new_gp64();
							switch (static_cast<IRCond>(inst.aux)) {
								case IRCond::EQ:
									pc.s_cmp_eq_f64(mask, F(inst.args[0]), F(inst.args[1]));
									break;
								case IRCond::NE:
									pc.s_cmp_ne_f64(mask, F(inst.args[0]), F(inst.args[1]));
									break;
								case IRCond::LT:
									pc.s_cmp_lt_f64(mask, F(inst.args[0]), F(inst.args[1]));
									break;
								case IRCond::LE:
									pc.s_cmp_le_f64(mask, F(inst.args[0]), F(inst.args[1]));
									break;
								case IRCond::GT:
									pc.s_cmp_gt_f64(mask, F(inst.args[0]), F(inst.args[1]));
									break;
								case IRCond::GE:
									pc.s_cmp_ge_f64(mask, F(inst.args[0]), F(inst.args[1]));
									break;
							}
							pc.s_extract_u64(bits, mask, 0);
							pc.j(L(inst.imm), test_nz(bits));
#endif
						} else {
							switch (static_cast<IRCond>(inst.aux)) {
								case IRCond::EQ:
									pc.j(L(inst.imm), cmp_eq(G(inst.args[0]), G(inst.args[1])));
									break;
								case IRCond::NE:
									pc.j(L(inst.imm), cmp_ne(G(inst.args[0]), G(inst.args[1])));
									break;
								case IRCond::LT:
									pc.j(L(inst.imm), scmp_lt(G(inst.args[0]), G(inst.args[1])));
									break;
								case IRCond::LE:
									pc.j(L(inst.imm), scmp_le(G(inst.args[0]), G(inst.args[1])));
									break;
								case IRCond::GT:
									pc.j(L(inst.imm), scmp_gt(G(inst.args[0]), G(inst.args[1])));
									break;
								case IRCond::GE:
									pc.j(L(inst.imm), scmp_ge(G(inst.args[0]), G(inst.args[1])));
									break;
							}
						}
					}
				} break;

				case IROp::Ret: {
					Gp type = pc.new_gp64();
					const auto kind_it = value_kind_map.find(inst.args[0].id);
					const bool is_f64 = kind_it != value_kind_map.end() && kind_it->value == IRValueKind::F64;
					if (is_f64) {
						pc.mov(type, Variant::FLOAT);
						pc.store_u64(mem_ptr(result_ptr, 0), type);
						pc.v_storeu64_f64(mem_ptr(result_ptr, 8), F(inst.args[0]));
					} else {
						pc.mov(type, Variant::INT);
						pc.store_u64(mem_ptr(result_ptr, 0), type);
						pc.store_u64(mem_ptr(result_ptr, 8), G(inst.args[0]));
					}
					pc.ret();
				} break;

				case IROp::RetVariant: {
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_return, asmjit::FuncSignature::build<void, Variant *, const Variant *>());
					invoke_node->set_arg(0, result_ptr);
					invoke_node->set_arg(1, G(inst.args[0]));
					pc.ret();
				} break;

				case IROp::RetTypedBuiltin: {
					Gp builtin_type = pc.new_gp64();
					pc.mov(builtin_type, uint32_t(inst.imm));
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_return_typed_builtin, asmjit::FuncSignature::build<void, Variant *, const Variant *, int>());
					invoke_node->set_arg(0, result_ptr);
					invoke_node->set_arg(1, G(inst.args[0]));
					invoke_node->set_arg(2, builtin_type.r32());
					pc.ret();
				} break;

				case IROp::RetTypedArray: {
					Gp builtin_type = pc.new_gp64();
					Gp native_type_ptr = pc.new_gp64();
					pc.mov(builtin_type, uint32_t(inst.imm));
					pc.mov(native_type_ptr, uint64_t(&function->_global_names_ptr[uint32_t(inst.aux)]));
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_assign_typed_array, asmjit::FuncSignature::build<void, Variant *, const Variant *, const Variant *, int, const StringName *>());
					invoke_node->set_arg(0, result_ptr);
					invoke_node->set_arg(1, G(inst.args[0]));
					invoke_node->set_arg(2, G(inst.args[1]));
					invoke_node->set_arg(3, builtin_type.r32());
					invoke_node->set_arg(4, native_type_ptr);
					pc.ret();
				} break;

				case IROp::RetTypedDictionary: {
					Gp key_builtin_type = pc.new_gp64();
					Gp key_native_type_ptr = pc.new_gp64();
					Gp value_builtin_type = pc.new_gp64();
					Gp value_native_type_ptr = pc.new_gp64();
					pc.mov(key_builtin_type, uint32_t(inst.imm & 0xFFFFFFFFu));
					pc.mov(key_native_type_ptr, uint64_t(&function->_global_names_ptr[uint32_t(inst.aux & 0xFFFFFFFFu)]));
					pc.mov(value_builtin_type, uint32_t((inst.imm >> 32) & 0xFFFFFFFFu));
					pc.mov(value_native_type_ptr, uint64_t(&function->_global_names_ptr[uint32_t((inst.aux >> 32) & 0xFFFFFFFFu)]));
					asmjit::InvokeNode *invoke_node = emit_invoke(_jit_variant_assign_typed_dictionary, asmjit::FuncSignature::build<void, Variant *, const Variant *, const Variant *, int, const StringName *, const Variant *, int, const StringName *>());
					invoke_node->set_arg(0, result_ptr);
					invoke_node->set_arg(1, G(inst.args[0]));
					invoke_node->set_arg(2, G(inst.args[1]));
					invoke_node->set_arg(3, key_builtin_type.r32());
					invoke_node->set_arg(4, key_native_type_ptr);
					invoke_node->set_arg(5, G(inst.args[2]));
					invoke_node->set_arg(6, value_builtin_type.r32());
					invoke_node->set_arg(7, value_native_type_ptr);
					pc.ret();
				} break;

				case IROp::StoreI64:
					pc.store_u64(addrOf(inst.mem_loc, 8), G(inst.args[0]));
					break;

				case IROp::StoreF64:
					pc.v_storeu64_f64(addrOf(inst.mem_loc, 8), F(inst.args[0]));
					break;

				case IROp::StoreType:
					Gp type = pc.new_gp64();
					pc.mov(type, inst.imm);
					pc.store_u64(addrOf(inst.mem_loc), type);
					break;
			}
		}
	}

	pc.end_func();
	pc.finalize();

	void *func_ptr = nullptr;
	const asmjit::Error err = runtime.add(&func_ptr, &code);
	if (err != asmjit::Error::kOk) {
		print_error(asmjit::DebugUtils::error_as_string(err));
	} else {
		function->jit_function = func_ptr;
	}

	reset();
}

JitRuntimeManager::AddressInfo JitRuntimeManager::decode_address_index(int encoded_address) {
	AddressInfo info{};

	info.address_type = (encoded_address & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS;
	info.address_index = encoded_address & GDScriptFunction::ADDR_MASK;

	return info;
}

int JitRuntimeManager::encode_address_index(int address_type, int address_index) {
	ERR_FAIL_COND_V(address_type < 0 || address_type >= GDScriptFunction::ADDR_TYPE_MAX, -1);

	ERR_FAIL_COND_V(address_index < 0 || address_index > GDScriptFunction::ADDR_MASK, -1);

	return address_index | (address_type << GDScriptFunction::ADDR_BITS);
}

void JitRuntimeManager::print_address_info(const GDScriptCodeGenerator::Address &p_address) {
	String mode_str;
	switch (p_address.mode) {
		case GDScriptCodeGenerator::Address::SELF:
			mode_str = "SELF";
			break;
		case GDScriptCodeGenerator::Address::CLASS:
			mode_str = "CLASS";
			break;
		case GDScriptCodeGenerator::Address::MEMBER:
			mode_str = "MEMBER";
			break;
		case GDScriptCodeGenerator::Address::CONSTANT:
			mode_str = "CONSTANT";
			break;
		case GDScriptCodeGenerator::Address::LOCAL_VARIABLE:
			mode_str = "LOCAL_VARIABLE";
			break;
		case GDScriptCodeGenerator::Address::FUNCTION_PARAMETER:
			mode_str = "FUNCTION_PARAMETER";
			break;
		case GDScriptCodeGenerator::Address::TEMPORARY:
			mode_str = "TEMPORARY";
			break;
		case GDScriptCodeGenerator::Address::NIL:
			mode_str = "NIL";
			break;
		default:
			mode_str = vformat("UNKNOWN(%d)", p_address.mode);
	}

	print_line(vformat("  Mode: %s", mode_str));
	print_line(vformat("  Address (raw): %d (0x%X)", p_address.address, p_address.address));

	int encoded_address = -1;
	int addr_type = -1;

	switch (p_address.mode) {
		case GDScriptCodeGenerator::Address::SELF:
			encoded_address = GDScriptFunction::ADDR_SELF;
			addr_type = GDScriptFunction::ADDR_TYPE_STACK;
			break;
		case GDScriptCodeGenerator::Address::CLASS:
			encoded_address = GDScriptFunction::ADDR_CLASS;
			addr_type = GDScriptFunction::ADDR_TYPE_STACK;
			break;
		case GDScriptCodeGenerator::Address::MEMBER:
			encoded_address = p_address.address | (GDScriptFunction::ADDR_TYPE_MEMBER << GDScriptFunction::ADDR_BITS);
			addr_type = GDScriptFunction::ADDR_TYPE_MEMBER;
			break;
		case GDScriptCodeGenerator::Address::CONSTANT:
			encoded_address = p_address.address | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS);
			addr_type = GDScriptFunction::ADDR_TYPE_CONSTANT;
			break;
		case GDScriptCodeGenerator::Address::LOCAL_VARIABLE:
		case GDScriptCodeGenerator::Address::FUNCTION_PARAMETER:
			encoded_address = p_address.address | (GDScriptFunction::ADDR_TYPE_STACK << GDScriptFunction::ADDR_BITS);
			addr_type = GDScriptFunction::ADDR_TYPE_STACK;
			break;
		case GDScriptCodeGenerator::Address::NIL:
			encoded_address = GDScriptFunction::ADDR_NIL;
			addr_type = GDScriptFunction::ADDR_TYPE_STACK;
			break;
		case GDScriptCodeGenerator::Address::TEMPORARY:
			print_line("  Encoded: N/A (TEMPORARY addresses are resolved later)");
			break;
	}

	if (encoded_address != -1) {
		print_line(vformat("  Encoded Address: %d (0x%X)", encoded_address, encoded_address));

		AddressInfo decoded = decode_address_index(encoded_address);
		print_line(vformat("  Decoded Type: %d", decoded.address_type));
		print_line(vformat("  Decoded Index: %d", decoded.address_index));
	}

	if (p_address.type.has_type()) {
		String kind_str;
		switch (p_address.type.kind) {
			case GDScriptDataType::VARIANT:
				kind_str = "VARIANT";
				break;
			case GDScriptDataType::BUILTIN:
				kind_str = vformat("BUILTIN (%s)", Variant::get_type_name(p_address.type.builtin_type));
				break;
			case GDScriptDataType::NATIVE:
				kind_str = vformat("NATIVE (%s)", p_address.type.native_type);
				break;
			case GDScriptDataType::SCRIPT:
				kind_str = "SCRIPT";
				break;
			case GDScriptDataType::GDSCRIPT:
				kind_str = "GDSCRIPT";
				break;
			default:
				kind_str = vformat("UNKNOWN(%d)", p_address.type.kind);
		}
		print_line(vformat("  Data Type: %s", kind_str));
	} else {
		print_line("  Data Type: None (untyped)");
	}
}


template <typename Fn>
asmjit::InvokeNode *JitRuntimeManager::emit_invoke(Fn op_func, asmjit::FuncSignature const &sig) {
#ifdef ASMJIT_UJIT_AARCH64
	Gp fn = cc.new_gpz("fn");
	cc.mov(fn, (uint64_t)op_func);

	asmjit::InvokeNode *invokeNode;

	cc.invoke(asmjit::Out(invokeNode), fn, sig);
	return invokeNode;
#else
	asmjit::InvokeNode *invokeNode;

	cc.invoke(asmjit::Out(invokeNode), op_func, sig);
	return invokeNode;
#endif
}
