#include "jit_runtime_manager.h"

#include "gdscript.h"
#include "gdscript_function.h"

using namespace asmjit::ujit;

JitRuntimeManager *JitRuntimeManager::singleton = nullptr;

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
#ifdef DEV_ENABLED
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
//TODO blocks, opt out optimization(?), ops
void JitRuntimeManager::compile(GDScriptFunction *gd_function) {
	asmjit::FuncNode *func_node = pc.add_func(asmjit::FuncSignature::build<void, Variant *, Variant *, Variant *, Variant *>());

	result_ptr = pc.new_gpz("result_ptr");
	stack_ptr = pc.new_gpz("stack_ptr");
	members_ptr = pc.new_gpz("members_ptr");
	constants_ptr = pc.new_gpz("constants_ptr");

	func_node->set_arg(0, result_ptr);
	func_node->set_arg(1, stack_ptr);
	func_node->set_arg(2, members_ptr);
	func_node->set_arg(3, constants_ptr);

	function = gd_function;
	code_size = gd_function->_code_size;
	ip = 0;

	collect_jump_targets();

	while (has_next()) {
		if (jump_labels.has(ip)) {
			asmjit::Label label = jump_labels[ip];
			pc.bind(label);
		}

		const GDScriptFunction::Opcode op = opcode();

		if (op == GDScriptFunction::OPCODE_LINE) {
			next();
			continue;
		}

		emit_opcode(op);

		next();
	}

	pc.end_func();
	pc.finalize();

	print_line(code.code_size());
	void *func_ptr = nullptr;
	const asmjit::Error err = runtime.add(&func_ptr, &code);
	if (err != asmjit::Error::kOk) {
		print_error(asmjit::DebugUtils::error_as_string(err));
	} else {
		function->jit_function = func_ptr;
	}

	reset();
}

void JitRuntimeManager::release_function(void *func_ptr) {
	if (func_ptr) {
		runtime.release(func_ptr);
	}
}

void JitRuntimeManager::reset() {
	code.reinit();
}

void JitRuntimeManager::emit_opcode(GDScriptFunction::Opcode opcode) {
	switch (opcode) {
		case GDScriptFunction::OPCODE_OPERATOR_VALIDATED:
			emit_operator_validated();
			break;

		case GDScriptFunction::OPCODE_JUMP: {
			const int target_ip = operand(1);
			pc.j(jump_labels[target_ip]);
			break;
		}

		case GDScriptFunction::OPCODE_JUMP_IF_NOT: {
			const AddressInfo condition = decode_address(1);
			const int target_ip = operand(2);

			const Gp bool_result = booleanize(condition);

			pc.j(jump_labels[target_ip], test_z(bool_result, bool_result));
			break;
		}

		case GDScriptFunction::OPCODE_ASSIGN: {
			const auto dst_addr = decode_address(1);
			const auto src_addr = decode_address(2);

			copy_variant(dst_addr, src_addr);
		} break;

		case GDScriptFunction::OPCODE_ITERATE_BEGIN_RANGE: {
			const AddressInfo counter = decode_address(1);
			const AddressInfo from_val = decode_address(2);
			const AddressInfo to_val = decode_address(3);
			const AddressInfo step_val = decode_address(4);
			const AddressInfo iterator = decode_address(5);
			const int target_ip = operand(6);

			loop_ctx.to = pc.new_gp64("range_to");
			loop_ctx.step = pc.new_gp64("range_step");
			loop_ctx.count = pc.new_gp64("range_cnt");

			load64(loop_ctx.count, from_val, 8);
			load64(loop_ctx.to, to_val, 8);
			load64(loop_ctx.step, step_val, 8);

			const Gp cond = pc.new_gp64("range_cond");

			pc.store_u64(get_variant_mem(counter, 8), loop_ctx.count);
			assign_type(counter, Variant::INT);

			pc.sub(cond, loop_ctx.count, loop_ctx.to);
			pc.mul(cond, cond, loop_ctx.step);

			pc.j(jump_labels[target_ip], scmp_ge(cond, 0));

			pc.store_u64(get_variant_mem(iterator, 8), loop_ctx.count);
			assign_type(iterator, Variant::INT);
		} break;

		case GDScriptFunction::OPCODE_ITERATE_RANGE: {
			const AddressInfo iter = decode_address(4);
			const int target_ip = operand(5);
			const Gp cond = pc.new_gp64("range_cond_it");

			pc.add(loop_ctx.count, loop_ctx.count, loop_ctx.step);
			pc.sub(cond, loop_ctx.count, loop_ctx.to);
			pc.mul(cond, cond, loop_ctx.step);

			pc.j(jump_labels[target_ip], scmp_ge(cond, 0));
			pc.store_u64(get_variant_mem(iter, 8), loop_ctx.count);
		} break;

		case GDScriptFunction::OPCODE_RETURN:
			emit_return();
			break;

		case GDScriptFunction::OPCODE_END:
			break;

		default:
			ERR_PRINT("Not implemented opcode: " + itos(opcode));
			break;
	}
}

void JitRuntimeManager::collect_jump_targets() {
	ip = 0;
    jump_labels.clear();

    while (ip < code_size) {
        GDScriptFunction::Opcode op = opcode();

        switch (op) {
            case GDScriptFunction::OPCODE_JUMP: {
                int target_ip = operand(1);
                if (!jump_labels.has(target_ip)) {
                    jump_labels[target_ip] = pc.new_label();
                }
                break;
            }

            case GDScriptFunction::OPCODE_JUMP_IF:
            case GDScriptFunction::OPCODE_JUMP_IF_NOT: {
                int target_ip = operand(2);
                if (!jump_labels.has(target_ip)) {
                    jump_labels[target_ip] = pc.new_label();
                }
                break;
            }

            case GDScriptFunction::OPCODE_ITERATE_BEGIN:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_INT:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_FLOAT:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_VECTOR2:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_VECTOR2I:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_VECTOR3:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_VECTOR3I:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_STRING:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_DICTIONARY:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_BYTE_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_INT32_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_INT64_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_FLOAT32_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_FLOAT64_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_STRING_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_VECTOR2_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_VECTOR3_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_COLOR_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_PACKED_VECTOR4_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_BEGIN_OBJECT: {
                int target_ip = operand(4);
                if (!jump_labels.has(target_ip)) {
                    jump_labels[target_ip] = pc.new_label();
                }
                break;
            }

            case GDScriptFunction::OPCODE_ITERATE_BEGIN_RANGE: {
                int target_ip = operand(6);
                if (!jump_labels.has(target_ip)) {
                    jump_labels[target_ip] = pc.new_label();
                }
                break;
            }

            case GDScriptFunction::OPCODE_ITERATE:
            case GDScriptFunction::OPCODE_ITERATE_INT:
            case GDScriptFunction::OPCODE_ITERATE_FLOAT:
            case GDScriptFunction::OPCODE_ITERATE_VECTOR2:
            case GDScriptFunction::OPCODE_ITERATE_VECTOR2I:
            case GDScriptFunction::OPCODE_ITERATE_VECTOR3:
            case GDScriptFunction::OPCODE_ITERATE_VECTOR3I:
            case GDScriptFunction::OPCODE_ITERATE_STRING:
            case GDScriptFunction::OPCODE_ITERATE_DICTIONARY:
            case GDScriptFunction::OPCODE_ITERATE_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_PACKED_BYTE_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_PACKED_INT32_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_PACKED_INT64_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_PACKED_FLOAT32_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_PACKED_FLOAT64_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_PACKED_STRING_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_PACKED_VECTOR2_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_PACKED_VECTOR3_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_PACKED_COLOR_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_PACKED_VECTOR4_ARRAY:
            case GDScriptFunction::OPCODE_ITERATE_OBJECT:
            case GDScriptFunction::OPCODE_ITERATE_RANGE: {
                int target_ip = operand(4);
                if (!jump_labels.has(target_ip)) {
                    jump_labels[target_ip] = pc.new_label();
                }
                break;
            }
			default: break;
		}

        next();
    }

	ip = 0;
}

int JitRuntimeManager::position() const {
	return ip;
}

bool JitRuntimeManager::has_next() const {
	return ip < code_size;
}

GDScriptFunction::Opcode JitRuntimeManager::opcode() const {
	ERR_FAIL_COND_V(ip >= code_size, GDScriptFunction::OPCODE_END);
	return static_cast<GDScriptFunction::Opcode>(function->code[ip]);
}

GDScriptFunction::Opcode JitRuntimeManager::peek(int positions_ahead = 1) {
	int future_ip = ip;
	for (int i = 0; i < positions_ahead && future_ip < code_size; i++) {
		int size = get_instruction_size(future_ip);
		future_ip += size;
	}
	if (future_ip >= code_size) {
		return GDScriptFunction::OPCODE_END;
	}
	return static_cast<GDScriptFunction::Opcode>(function->code[future_ip]);
}

int JitRuntimeManager::operand(int offset) const {
	ERR_FAIL_COND_V(ip + offset >= code_size, 0);
	return function->code[ip + offset];
}

void JitRuntimeManager::next() {
	ip += get_instruction_size(ip);
}

int JitRuntimeManager::instruction_size() {
	return get_instruction_size(ip);
}

JitRuntimeManager::AddressInfo JitRuntimeManager::decode_address(int offset) const {
	AddressInfo info{};
	info.encoded = operand(offset);
	info.type = (info.encoded >> GDScriptFunction::ADDR_BITS);
	info.index = info.encoded & GDScriptFunction::ADDR_MASK;
	info.variant_type = Variant::NIL;

	if (info.is_stack()) {
		info.variant_type = get_stack_type(info.index);
	} else if (info.is_constant()) {
		if (info.index >= 0 && info.index < function->_constant_count) {
			const Variant& constant = function->get_constant(info.index);
			info.variant_type = constant.get_type();
		}
	}
	else if (info.is_member()) {
		for (const KeyValue<StringName, GDScript::MemberInfo>& E : function->_script->member_indices) {
			if (E.value.index == info.index) {
				const GDScript::MemberInfo& member_info = E.value;
				info.variant_type = member_info.data_type.builtin_type;

				break;
			}
		}
	}

	return info;
}

int JitRuntimeManager::get_instruction_size(int pos) const {
    if (pos >= code_size) return 0;

	const auto op = static_cast<GDScriptFunction::Opcode>(function->code[pos]);

    switch (op) {
    	case GDScriptFunction::OPCODE_OPERATOR_VALIDATED:
    		return 5;

    	case GDScriptFunction::OPCODE_ASSIGN:
    		return 3;

    	case GDScriptFunction::OPCODE_JUMP:
    		return 2;

    	case GDScriptFunction::OPCODE_JUMP_IF_NOT:
    		return 3;

    	case GDScriptFunction::OPCODE_RETURN:
    		return 2;

    	case GDScriptFunction::OPCODE_ITERATE_BEGIN_RANGE:
    		return 7;

    	case GDScriptFunction::OPCODE_ITERATE_RANGE:
    		return 6;

    	case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN:
    		return 3 + 1 + function->code[pos + 1];

    	case GDScriptFunction::OPCODE_GET_NAMED_VALIDATED:
    		return 4;

    	case GDScriptFunction::OPCODE_LINE:
    		return 2;

    	case GDScriptFunction::OPCODE_END:
    		return 1;

        default:
            ERR_PRINT("Unknown opcode: " + itos(op));
            return 1;
    }
}

template <typename... Args>
void JitRuntimeManager::print_debug(Args... p_args) {
#ifdef DEV_ENABLED
	Variant variants[sizeof...(p_args)] = { p_args... };
	__print_line(stringify_variants(Span(variants)));
#endif
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

Gp JitRuntimeManager::get_variant_ptr(const AddressInfo &p_address) {
	Gp variant_ptr = pc.new_gpz();
	pc.lea(variant_ptr, get_variant_mem(p_address));

	return variant_ptr;
}

Mem JitRuntimeManager::get_variant_mem(const AddressInfo &p_address, int offset) const {
	constexpr int variant_size = sizeof(Variant);

	if (p_address.is_stack()) {
		const int index = p_address.index;

		if (index == GDScriptFunction::ADDR_STACK_SELF) {
			return mem_ptr(stack_ptr, GDScriptFunction::ADDR_STACK_SELF * variant_size + offset);
		}
		if (index == GDScriptFunction::ADDR_STACK_CLASS) {
			return mem_ptr(stack_ptr, GDScriptFunction::ADDR_STACK_CLASS * variant_size + offset);
		}
		if (index == GDScriptFunction::ADDR_STACK_NIL) {
			return mem_ptr(stack_ptr, GDScriptFunction::ADDR_STACK_NIL * variant_size + offset);
		}

		return mem_ptr(stack_ptr, index * variant_size + offset);
	}
	else if (p_address.is_constant()) {
		return mem_ptr(constants_ptr, p_address.index * variant_size + offset);
	}
	else if (p_address.is_member()) {
		return mem_ptr(members_ptr, p_address.index * variant_size + offset);
	}

	return mem_ptr(stack_ptr);
}

void JitRuntimeManager::emit_operator_validated() {
	const AddressInfo a = decode_address(1);
	const AddressInfo b = decode_address(2);
	const AddressInfo dst = decode_address(3);
	const int operator_idx = operand(4);

	if (operator_idx < 0 || operator_idx >= function->_operator_funcs_count) {
		ERR_PRINT("Invalid operator index: " + itos(operator_idx));
		return;
	}

	Variant::ValidatedOperatorEvaluator op_func = function->_operator_funcs_ptr[operator_idx];

	const Gp a_ptr = get_variant_ptr(a);
	const Gp b_ptr = get_variant_ptr(b);
	const Gp dst_ptr = get_variant_ptr(dst);

	asmjit::InvokeNode *invoke = emit_invoke(
			op_func,
			asmjit::FuncSignature::build<void, const Variant *, const Variant *, Variant *>());
	invoke->set_arg(0, a_ptr);
	invoke->set_arg(1, b_ptr);
	invoke->set_arg(2, dst_ptr);
}

void JitRuntimeManager::emit_return() {
	const AddressInfo return_value = decode_address(1);

	const Gp ret_val_ptr = get_variant_ptr(return_value);

	asmjit::InvokeNode *invoke = emit_invoke(
		+[](Variant *dst, const Variant *src) { *dst = *src; },
		asmjit::FuncSignature::build<void, Variant *, const Variant *>()
	);

	invoke->set_arg(0, result_ptr);
	invoke->set_arg(1, ret_val_ptr);

	pc.ret();
}

void JitRuntimeManager::copy_variant(const Gp &dst_ptr, const Gp &src_ptr) {
	asmjit::InvokeNode *copy_invoke = emit_invoke(
			+[](Variant *dst, const Variant *src) {
				*dst = *src;
			},
			asmjit::FuncSignature::build<void, Variant *, const Variant *>());
	copy_invoke->set_arg(0, dst_ptr);
	copy_invoke->set_arg(1, src_ptr);
}

void JitRuntimeManager::copy_variant(const AddressInfo &dst, const AddressInfo &src) {
	if ((src.variant_type == dst.variant_type) || (dst.variant_type == Variant::NIL && src.variant_type != Variant::NIL && dst.is_stack())) {
		if (dst.is_stack()) {
			function->locals_slots[dst.index] = src.variant_type;
		}

		switch (src.variant_type) {
			case Variant::INT: {
				const Gp source_reg = pc.new_gp64();
				load64(source_reg, src, 8);
				assign_type(dst, src.variant_type);
				pc.store_u64(get_variant_mem(dst, 8), source_reg);
			} break;
			case Variant::BOOL: {
				const Gp source_reg = pc.new_gp64();
				load64(source_reg, src, 8);
				assign_type(dst, src.variant_type);
				pc.store_u64(get_variant_mem(dst, 8), source_reg);
			} break;
			default: {
				const Gp src_ptr = get_variant_ptr(src);
				const Gp dst_ptr = get_variant_ptr(dst);

				copy_variant(dst_ptr, src_ptr);
			} break;
		}
	} else {
		const Gp src_ptr = get_variant_ptr(src);
		const Gp dst_ptr = get_variant_ptr(dst);

		copy_variant(dst_ptr, src_ptr);
	}
}

Gp JitRuntimeManager::booleanize(const AddressInfo &p_address) {
	Gp bool_result = pc.new_gp32("bool_result");
	const Gp condition_ptr = get_variant_ptr(p_address);
	asmjit::InvokeNode *booleanize_invoke = emit_invoke(
			+[](const Variant *v) -> uint32_t {
				return v->booleanize() ? 1 : 0; //TODO
			},
			asmjit::FuncSignature::build<uint32_t, const Variant *>());
	booleanize_invoke->set_arg(0, condition_ptr);
	booleanize_invoke->set_ret(0, bool_result);
	return bool_result;
}

void JitRuntimeManager::assign_type(const AddressInfo &dst, const int type_value) {
	const Mem mem = get_variant_mem(dst);
#ifdef ASMJIT_UJIT_X86
	cc.mov(mem.clone_resized(4), type_value);
#else
	const Gp tmp = pc.new_gp32();
	pc.mov(tmp, type_value);
	pc.store_u32(mem, tmp);
#endif
}

void JitRuntimeManager::load64(const Gp &dst, const AddressInfo &src, const int offset) {
#ifdef ASMJIT_UJIT_X86
	if (!src.is_constant()) {
#endif
		pc.load_u64(dst, get_variant_mem(src, offset));
#ifdef ASMJIT_UJIT_X86
	} else {
		Variant var_val = function->get_constant(src.index);
		cc.mov(dst, VariantInternalAccessor<int64_t>::get(&var_val));
	}
#endif
}

Variant::Type JitRuntimeManager::get_stack_type(int index) const {
	if (index == GDScriptFunction::ADDR_STACK_SELF) {
		return Variant::OBJECT;
	}
	if (index == GDScriptFunction::ADDR_STACK_CLASS) {
		return Variant::OBJECT;
	}
	if (index == GDScriptFunction::ADDR_STACK_NIL) {
		return Variant::NIL;
	}

	if (function->temporary_slots.has(index)) {
		return function->temporary_slots[index];
	}

	if (function->locals_slots.has(index)) {
		return function->locals_slots[index];
	}

	if (function->argument_types.size() > 0) {
		int arg_index = index - GDScriptFunction::FIXED_ADDRESSES_MAX;
		if (arg_index >= 0 && arg_index < function->_argument_count) {
			const GDScriptDataType &arg_type = function->argument_types[arg_index];
			if (arg_type.has_type()) {
				return arg_type.builtin_type;
			}
		}
	}

	return Variant::NIL;
}