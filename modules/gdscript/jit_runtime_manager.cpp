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
#ifdef DEBUG_ENABLED
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
	asmjit::FuncNode *func_node = pc.add_func(asmjit::FuncSignature::build<void, Variant *, Variant *, Variant *, Variant *>());

	result_ptr = pc.new_gpz("result_ptr");
	stack_ptr = pc.new_gpz("stack_ptr");
	members_ptr = pc.new_gpz("members_ptr");
	constants_ptr = pc.new_gpz("constants_ptr");

	func_node->set_arg(0, result_ptr);
	func_node->set_arg(1, stack_ptr);
	func_node->set_arg(2, members_ptr);
	func_node->set_arg(3, constants_ptr);

	print_line(max_locals);

	HashMap<uint32_t, Gp> vmap;

	auto V = [&](const ValueId v) -> const Gp & {
		if (const auto it = vmap.find(v.id); it != vmap.end()) {
			return it->value;
		}

		const Gp r = cc.new_gp64();
		vmap[v.id] = r;
		return vmap[v.id];
	};

	auto addrOf = [&](const GDScriptCodeGenerator::Address &p_address, const int offset = 0) -> Mem {
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
			return mem_ptr(stack_ptr, (p_address.address + max_locals) * variant_size + offset); //fix
		}

		return mem_ptr(stack_ptr, p_address.address * variant_size + offset);
	};

	for (const IRInst &inst : ir.code) {
		switch (inst.op) {
			case IROp::LoadParam:
				pc.load_i64(V(inst.dst), addrOf(inst.mem_loc, 8));
				break;

			case IROp::AddI64:
				pc.add(V(inst.dst), V(inst.args[0]), V(inst.args[1]));
				break;

			case IROp::Ret: {
				auto type = pc.new_gp64();
				pc.mov(type, Variant::INT);
				pc.store_u64(mem_ptr(result_ptr, 0), type);
				pc.store_u64(mem_ptr(result_ptr, 8), V(inst.args[0]));
				pc.ret();
			} break;

			case IROp::StoreI64:
				pc.store_u64(addrOf(inst.mem_loc, 8), V(inst.args[0]));
				break;
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

