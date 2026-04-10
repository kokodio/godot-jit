#include "jit_ir_optimizer.h"

static bool _ir_can_elide_store(const GDScriptCodeGenerator::Address &p_address) {
	switch (p_address.mode) {
		case GDScriptCodeGenerator::Address::LOCAL_VARIABLE:
		case GDScriptCodeGenerator::Address::FUNCTION_PARAMETER:
		case GDScriptCodeGenerator::Address::TEMPORARY:
			return true;
		default:
			return false;
	}
}

static bool _ir_can_forward_load(const GDScriptCodeGenerator::Address &p_address) {
	switch (p_address.mode) {
		case GDScriptCodeGenerator::Address::CONSTANT:
		case GDScriptCodeGenerator::Address::LOCAL_VARIABLE:
		case GDScriptCodeGenerator::Address::FUNCTION_PARAMETER:
		case GDScriptCodeGenerator::Address::TEMPORARY:
			return true;
		default:
			return false;
	}
}

static uint64_t _ir_slot_key(const GDScriptCodeGenerator::Address &p_address) {
	return (uint64_t(p_address.mode) << 32) | uint64_t(uint32_t(p_address.address));
}

static uint64_t _ir_real_member_slot_key(const GDScriptCodeGenerator::Address &p_address, uint32_t p_byte_offset) {
	return (uint64_t(uint8_t(p_address.mode)) << 56) | (uint64_t(p_byte_offset & 0x00FFFFFFu) << 32) | uint64_t(uint32_t(p_address.address));
}

static bool _ir_is_value_store_op(IROp p_op) {
	return p_op == IROp::StoreI64 || p_op == IROp::StoreF64;
}

static bool _ir_inst_defines_value(const IRInst &p_inst) {
	switch (p_inst.op) {
		case IROp::ZeroI64:
		case IROp::LoadParam:
		case IROp::LoadDefArg:
		case IROp::LoadF64:
		case IROp::LoadRealMemberF64:
		case IROp::LoadPtr:
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
		case IROp::IterateBegin:
		case IROp::Iterate:
		case IROp::Booleanize:
			return true;
		default:
			return false;
	}
}

static bool _ir_try_get_value_kind(const IRInst &p_inst, IRValueKind &r_kind) {
	if (!_ir_inst_defines_value(p_inst)) {
		return false;
	}

	switch (p_inst.op) {
		case IROp::ZeroI64:
		case IROp::LoadDefArg:
			r_kind = IRValueKind::GP64;
			break;
		case IROp::LoadF64:
		case IROp::AddF64:
		case IROp::NegF64:
		case IROp::MulF64:
		case IROp::SubF64:
		case IROp::LoadRealMemberF64:
			r_kind = IRValueKind::F64;
			break;
		default:
			r_kind = IRValueKind::GP64;
			break;
	}

	return true;
}

enum IRLiveBits : uint8_t {
	IR_LIVE_VALUE = 1 << 0,
	IR_LIVE_TYPE = 1 << 1,
};

static bool _ir_live_sets_equal(const HashMap<uint64_t, uint8_t> &p_a, const HashMap<uint64_t, uint8_t> &p_b) {
	if (p_a.size() != p_b.size()) {
		return false;
	}

	for (const KeyValue<uint64_t, uint8_t> &E : p_a) {
		const auto it = p_b.find(E.key);
		if (it == p_b.end() || it->value != E.value) {
			return false;
		}
	}

	return true;
}

static void _ir_live_add(HashMap<uint64_t, uint8_t> &r_live, uint64_t p_key, uint8_t p_bits) {
	const auto it = r_live.find(p_key);
	if (it == r_live.end()) {
		r_live[p_key] = p_bits;
		return;
	}
	it->value |= p_bits;
}

static void _ir_live_remove(HashMap<uint64_t, uint8_t> &r_live, uint64_t p_key, uint8_t p_bits) {
	const auto it = r_live.find(p_key);
	if (it == r_live.end()) {
		return;
	}
	it->value &= ~p_bits;
	if (it->value == 0) {
		r_live.erase(p_key);
	}
}

static int _ir_find_matching_store_type(const Vector<IRInst> &p_code, int p_store_value_idx) {
	if (p_store_value_idx < 0 || p_store_value_idx >= p_code.size()) {
		return -1;
	}

	const IRInst &store_value = p_code[p_store_value_idx];
	if (!_ir_is_value_store_op(store_value.op)) {
		return -1;
	}

	for (int idx = p_store_value_idx - 1; idx >= 0; idx--) {
		const IRInst &inst = p_code[idx];
		if (inst.mem_loc.mode != store_value.mem_loc.mode || inst.mem_loc.address != store_value.mem_loc.address) {
			continue;
		}

		if (_ir_is_value_store_op(inst.op)) {
			return -1;
		}
		if (inst.op == IROp::StoreType) {
			return idx;
		}
	}

	return -1;
}

static int _ir_find_matching_value_store_forward(const Vector<IRInst> &p_code, int p_store_type_idx, const Vector<bool> &p_remove_inst) {
	if (p_store_type_idx < 0 || p_store_type_idx >= p_code.size()) {
		return -1;
	}

	const IRInst &store_type = p_code[p_store_type_idx];
	if (store_type.op != IROp::StoreType) {
		return -1;
	}

	for (int idx = p_store_type_idx + 1; idx < p_code.size(); idx++) {
		const IRInst &inst = p_code[idx];
		if (inst.mem_loc.mode != store_type.mem_loc.mode || inst.mem_loc.address != store_type.mem_loc.address) {
			continue;
		}

		if (inst.op == IROp::StoreType) {
			return -1;
		}
		if (_ir_is_value_store_op(inst.op)) {
			return p_remove_inst[idx] ? -1 : idx;
		}
	}

	return -1;
}

struct IRCFG {
	Vector<IRBlock> blocks;
	Vector<Vector<int>> successors;
	Vector<Vector<int>> predecessors;
	HashMap<uint32_t, bool> value_used_across_blocks;
};

static void _ir_add_successor(IRCFG &p_cfg, int p_from, int p_to) {
	ERR_FAIL_INDEX(p_from, p_cfg.blocks.size());
	ERR_FAIL_INDEX(p_to, p_cfg.blocks.size());
	p_cfg.successors.write[p_from].push_back(p_to);
	p_cfg.predecessors.write[p_to].push_back(p_from);
}

static Vector<IRBlock> _ir_split_raw_blocks(const IRBuilder &p_ir) {
	Vector<IRBlock> raw_blocks;

	for (const IRBlock &raw_block : p_ir.blocks) {
		IRBlock current;
		current.id = raw_blocks.size();
		current.has_label = raw_block.has_label;
		current.label = raw_block.label;
		bool have_current = raw_block.has_label;

		for (int inst_idx = 0; inst_idx < raw_block.code.size(); inst_idx++) {
			if (!have_current) {
				current.id = raw_blocks.size();
				have_current = true;
			}

			current.code.push_back(raw_block.code[inst_idx]);
				if ((raw_block.code[inst_idx].op == IROp::Jump || raw_block.code[inst_idx].op == IROp::JumpCc || raw_block.code[inst_idx].op == IROp::Ret || raw_block.code[inst_idx].op == IROp::RetVariant || raw_block.code[inst_idx].op == IROp::RetTypedBuiltin) && inst_idx + 1 < raw_block.code.size()) {
				raw_blocks.push_back(current);
				current = IRBlock();
				have_current = false;
			}
		}

		if (have_current) {
			raw_blocks.push_back(current);
		}
	}

	for (int block_idx = 0; block_idx < raw_blocks.size(); block_idx++) {
		raw_blocks.write[block_idx].id = block_idx;
	}

	return raw_blocks;
}

static HashMap<uint32_t, bool> _ir_analyze_cross_block_value_uses(const Vector<IRBlock> &p_blocks) {
	HashMap<uint32_t, int> value_def_block;
	HashMap<uint32_t, bool> value_used_across_blocks;

	for (int block_idx = 0; block_idx < p_blocks.size(); block_idx++) {
		for (const IRInst &inst : p_blocks[block_idx].code) {
			if (_ir_inst_defines_value(inst)) {
				value_def_block[inst.dst.id] = block_idx;
			}
		}
	}

	for (int block_idx = 0; block_idx < p_blocks.size(); block_idx++) {
		for (const IRInst &inst : p_blocks[block_idx].code) {
			for (const ValueId &arg : inst.args) {
				if (const auto it = value_def_block.find(arg.id); it != value_def_block.end() && it->value != block_idx) {
					value_used_across_blocks[arg.id] = true;
				}
			}
		}
	}

	return value_used_across_blocks;
}

static IRCFG _ir_build_cfg_from_blocks(const Vector<IRBlock> &p_blocks) {
	IRCFG cfg;
	cfg.blocks = p_blocks;
	cfg.successors.resize(cfg.blocks.size());
	cfg.predecessors.resize(cfg.blocks.size());
	cfg.value_used_across_blocks = _ir_analyze_cross_block_value_uses(cfg.blocks);

	HashMap<uint32_t, int> label_to_block;
	for (int block_idx = 0; block_idx < cfg.blocks.size(); block_idx++) {
		if (cfg.blocks[block_idx].has_label) {
			label_to_block[cfg.blocks[block_idx].label.id] = block_idx;
		}
	}

	for (int block_idx = 0; block_idx < cfg.blocks.size(); block_idx++) {
		const Vector<IRInst> &code = cfg.blocks[block_idx].code;
		const IRInst *terminator = code.is_empty() ? nullptr : &code[code.size() - 1];

		if (terminator == nullptr) {
			if (block_idx + 1 < cfg.blocks.size()) {
				_ir_add_successor(cfg, block_idx, block_idx + 1);
			}
			continue;
		}

		switch (terminator->op) {
			case IROp::Jump:
				_ir_add_successor(cfg, block_idx, label_to_block[terminator->imm]);
				break;
			case IROp::JumpCc:
				_ir_add_successor(cfg, block_idx, label_to_block[terminator->imm]);
				if (block_idx + 1 < cfg.blocks.size()) {
					_ir_add_successor(cfg, block_idx, block_idx + 1);
				}
				break;
				case IROp::Ret:
				case IROp::RetVariant:
				case IROp::RetTypedBuiltin:
					break;
			default:
				if (block_idx + 1 < cfg.blocks.size()) {
					_ir_add_successor(cfg, block_idx, block_idx + 1);
				}
				break;
		}
	}

	return cfg;
}

static IRCFG _ir_build_cfg(const IRBuilder &p_ir) {
	return _ir_build_cfg_from_blocks(_ir_split_raw_blocks(p_ir));
}

class IROptPassBase {
public:
	virtual ~IROptPassBase() = default;
	virtual Vector<IRBlock> run(const IRCFG &p_cfg, const Vector<IRBlock> &p_blocks) const = 0;
};

class IRKnownValuesPass : public IROptPassBase {
public:
	virtual Vector<IRBlock> run(const IRCFG &p_cfg, const Vector<IRBlock> &p_blocks) const override {
		(void)p_blocks;
		Vector<IRBlock> opt_blocks;
		opt_blocks.resize(p_cfg.blocks.size());

		struct IRKnownState {
			HashMap<uint64_t, ValueId> values;
			HashMap<uint64_t, ValueId> ptrs;
			HashMap<uint64_t, ValueId> real_members;
		};

		HashMap<uint32_t, ValueId> aliases;

		auto remap_value = [](const HashMap<uint32_t, ValueId> &p_aliases, ValueId p_value) -> ValueId {
			if (const auto it = p_aliases.find(p_value.id); it != p_aliases.end()) {
				return it->value;
			}
			return p_value;
		};

		auto invalidate_ptr_slot = [&](IRKnownState &r_state, const HashMap<uint32_t, uint64_t> &p_ptr_slots, ValueId p_ptr) {
			if (const auto it = p_ptr_slots.find(p_ptr.id); it != p_ptr_slots.end()) {
				const uint64_t slot_key = it->value;
				r_state.values.erase(slot_key);
				Vector<uint64_t> real_member_keys_to_erase;
				for (const KeyValue<uint64_t, ValueId> &E : r_state.real_members) {
					if ((E.key & 0xFFFFFFFFULL) == (slot_key & 0xFFFFFFFFULL) && ((E.key >> 56) & 0xFF) == ((slot_key >> 32) & 0xFF)) {
						real_member_keys_to_erase.push_back(E.key);
					}
				}
				for (uint64_t key : real_member_keys_to_erase) {
					r_state.real_members.erase(key);
				}
			}
		};

		for (int block_idx = 0; block_idx < p_cfg.blocks.size(); block_idx++) {
			IRBlock &opt_block = opt_blocks.write[block_idx];
			opt_block.id = p_cfg.blocks[block_idx].id;
			opt_block.has_label = p_cfg.blocks[block_idx].has_label;
			opt_block.label = p_cfg.blocks[block_idx].label;

			IRKnownState state;
			aliases.clear();
			HashMap<uint32_t, uint64_t> ptr_slots;

			for (int inst_idx = 0; inst_idx < p_cfg.blocks[block_idx].code.size(); inst_idx++) {
				const IRInst &raw_inst = p_cfg.blocks[block_idx].code[inst_idx];
				IRInst inst = raw_inst;

				for (int arg_idx = 0; arg_idx < inst.args.size(); arg_idx++) {
					inst.args.write[arg_idx] = remap_value(aliases, inst.args[arg_idx]);
				}

				switch (inst.op) {
					case IROp::LoadParam:
					case IROp::LoadF64: {
						if (_ir_can_forward_load(inst.mem_loc)) {
							const uint64_t key = _ir_slot_key(inst.mem_loc);
							if (state.values.has(key) && !p_cfg.value_used_across_blocks.has(inst.dst.id)) {
								aliases[inst.dst.id] = state.values[key];
								continue;
							}
							state.values[key] = inst.dst;
						}
						opt_block.code.push_back(inst);
					} break;

					case IROp::LoadRealMemberF64: {
						if (inst.args.is_empty() && _ir_can_forward_load(inst.mem_loc)) {
							const uint64_t key = _ir_real_member_slot_key(inst.mem_loc, uint32_t(inst.imm));
							if (state.real_members.has(key) && !p_cfg.value_used_across_blocks.has(inst.dst.id)) {
								aliases[inst.dst.id] = state.real_members[key];
								continue;
							}
							state.real_members[key] = inst.dst;
						}
						opt_block.code.push_back(inst);
					} break;

					case IROp::LoadPtr: {
						const uint64_t key = _ir_slot_key(inst.mem_loc);
						if (state.ptrs.has(key) && !p_cfg.value_used_across_blocks.has(inst.dst.id)) {
							aliases[inst.dst.id] = state.ptrs[key];
							continue;
						}
						state.ptrs[key] = inst.dst;
						ptr_slots[inst.dst.id] = key;
						opt_block.code.push_back(inst);
					} break;

					case IROp::CallBinOp:
					case IROp::CallOperator:
						invalidate_ptr_slot(state, ptr_slots, inst.args[2]);
						opt_block.code.push_back(inst);
						break;

					case IROp::IterateBegin:
					case IROp::Iterate:
						invalidate_ptr_slot(state, ptr_slots, inst.args[1]);
						invalidate_ptr_slot(state, ptr_slots, inst.args[2]);
						opt_block.code.push_back(inst);
						break;

					case IROp::Construct:
					case IROp::ConstructValidated:
					case IROp::ConstructArray:
					case IROp::ConstructDictionary:
						invalidate_ptr_slot(state, ptr_slots, inst.args[inst.args.size() - 1]);
						opt_block.code.push_back(inst);
						break;

					case IROp::ConstructTypedArray:
					case IROp::ConstructTypedDictionary:
						invalidate_ptr_slot(state, ptr_slots, inst.args[inst.args.size() - 2]);
						opt_block.code.push_back(inst);
						break;

					case IROp::AssignTypedArray:
					case IROp::AssignTypedDictionary:
						invalidate_ptr_slot(state, ptr_slots, inst.args[0]);
						opt_block.code.push_back(inst);
						break;

					case IROp::Call:
					case IROp::CallSelf:
					case IROp::CallUtility:
					case IROp::CallUtilityValidated:
					case IROp::CallBuiltinValidated:
					case IROp::CallMethodBindValidated:
						invalidate_ptr_slot(state, ptr_slots, inst.args[inst.args.size() - 1]);
						opt_block.code.push_back(inst);
						break;

					case IROp::Assign:
					case IROp::AssignNull:
					case IROp::AssignTrue:
					case IROp::AssignFalse:
					case IROp::AssignTypedBuiltin:
					case IROp::AssignTypedNative:
					case IROp::AssignTypedScript:
					case IROp::SetNamed:
					case IROp::SetNamedValidated:
					case IROp::SetMember:
					case IROp::SetKeyed:
						invalidate_ptr_slot(state, ptr_slots, inst.args[0]);
						opt_block.code.push_back(inst);
						break;

					case IROp::GetNamed:
					case IROp::GetNamedValidated:
						invalidate_ptr_slot(state, ptr_slots, inst.args[1]);
						opt_block.code.push_back(inst);
						break;

					case IROp::GetKeyed:
						invalidate_ptr_slot(state, ptr_slots, inst.args[2]);
						opt_block.code.push_back(inst);
						break;

					case IROp::GetMember:
						invalidate_ptr_slot(state, ptr_slots, inst.args[0]);
						opt_block.code.push_back(inst);
						break;

					case IROp::SetIndexedValidated:
					case IROp::SetKeyedValidated:
						invalidate_ptr_slot(state, ptr_slots, inst.args[0]);
						opt_block.code.push_back(inst);
						break;

					case IROp::GetIndexedValidated:
					case IROp::GetKeyedValidated:
						invalidate_ptr_slot(state, ptr_slots, inst.args[2]);
						opt_block.code.push_back(inst);
						break;

					case IROp::StoreType:
						if (_ir_can_elide_store(inst.mem_loc)) {
							const uint64_t key = _ir_slot_key(inst.mem_loc);
							bool has_matching_value_store = false;
							for (int lookahead = inst_idx + 1; lookahead < p_cfg.blocks[block_idx].code.size(); lookahead++) {
								const IRInst &next_inst = p_cfg.blocks[block_idx].code[lookahead];
								if (next_inst.mem_loc.mode != inst.mem_loc.mode || next_inst.mem_loc.address != inst.mem_loc.address) {
									continue;
								}
								if (next_inst.op == IROp::StoreType) {
									break;
								}
								if (_ir_is_value_store_op(next_inst.op)) {
									has_matching_value_store = true;
									break;
								}
							}
							if (!has_matching_value_store) {
								state.values.erase(key);
							}
							state.ptrs.erase(key);
							Vector<uint64_t> real_member_keys_to_erase;
							for (const KeyValue<uint64_t, ValueId> &E : state.real_members) {
								if ((E.key & 0xFFFFFFFFULL) == uint64_t(uint32_t(inst.mem_loc.address)) && ((E.key >> 56) & 0xFF) == uint64_t(uint8_t(inst.mem_loc.mode))) {
									real_member_keys_to_erase.push_back(E.key);
								}
							}
							for (uint64_t real_member_key : real_member_keys_to_erase) {
								state.real_members.erase(real_member_key);
							}
						}
						opt_block.code.push_back(inst);
						break;

					case IROp::StoreI64:
					case IROp::StoreF64:
						if (_ir_can_elide_store(inst.mem_loc)) {
							const uint64_t key = _ir_slot_key(inst.mem_loc);
							state.values[key] = inst.args[0];
							state.ptrs.erase(key);
							Vector<uint64_t> real_member_keys_to_erase;
							for (const KeyValue<uint64_t, ValueId> &E : state.real_members) {
								if ((E.key & 0xFFFFFFFFULL) == uint64_t(uint32_t(inst.mem_loc.address)) && ((E.key >> 56) & 0xFF) == uint64_t(uint8_t(inst.mem_loc.mode))) {
									real_member_keys_to_erase.push_back(E.key);
								}
							}
							for (uint64_t real_member_key : real_member_keys_to_erase) {
								state.real_members.erase(real_member_key);
							}
						}
						opt_block.code.push_back(inst);
						break;

					case IROp::StoreRealMemberF64: {
						if (_ir_can_forward_load(inst.mem_loc)) {
#ifdef REAL_T_IS_DOUBLE
							if (inst.args.size() == 1) {
								state.real_members[_ir_real_member_slot_key(inst.mem_loc, uint32_t(inst.imm))] = inst.args[0];
							}
#else
							state.real_members.erase(_ir_real_member_slot_key(inst.mem_loc, uint32_t(inst.imm)));
#endif
						}
						opt_block.code.push_back(inst);
					} break;

					default:
						opt_block.code.push_back(inst);
						break;
				}
			}
		}

		return opt_blocks;
	}
};

static HashMap<uint64_t, uint8_t> _ir_transfer_live_values(const IRBlock &p_block, const HashMap<uint64_t, uint8_t> &p_live_out) {
	HashMap<uint64_t, uint8_t> live = p_live_out;
	HashMap<uint32_t, uint64_t> ptr_slots;

	for (const IRInst &inst : p_block.code) {
		if (inst.op == IROp::LoadPtr && _ir_can_elide_store(inst.mem_loc)) {
			ptr_slots[inst.dst.id] = _ir_slot_key(inst.mem_loc);
		}
	}

	for (int inst_idx = p_block.code.size() - 1; inst_idx >= 0; inst_idx--) {
		const IRInst &inst = p_block.code[inst_idx];

		switch (inst.op) {
			case IROp::CallBinOp:
			case IROp::CallOperator:
				if (const auto it = ptr_slots.find(inst.args[2].id); it != ptr_slots.end()) {
					live.erase(it->value);
				}
				break;

			case IROp::IterateBegin:
			case IROp::Iterate:
				if (const auto it = ptr_slots.find(inst.args[1].id); it != ptr_slots.end()) {
					live.erase(it->value);
				}
				if (const auto it = ptr_slots.find(inst.args[2].id); it != ptr_slots.end()) {
					live.erase(it->value);
				}
				break;

			case IROp::Construct:
			case IROp::ConstructValidated:
			case IROp::ConstructArray:
			case IROp::ConstructDictionary:
				if (const auto it = ptr_slots.find(inst.args[inst.args.size() - 1].id); it != ptr_slots.end()) {
					live.erase(it->value);
				}
				break;

			case IROp::ConstructTypedArray:
			case IROp::ConstructTypedDictionary:
				if (const auto it = ptr_slots.find(inst.args[inst.args.size() - 2].id); it != ptr_slots.end()) {
					live.erase(it->value);
				}
				break;

			case IROp::AssignTypedArray:
			case IROp::AssignTypedDictionary:
				if (const auto it = ptr_slots.find(inst.args[0].id); it != ptr_slots.end()) {
					live.erase(it->value);
				}
				break;

			case IROp::Call:
			case IROp::CallSelf:
			case IROp::CallUtility:
			case IROp::CallUtilityValidated:
			case IROp::CallBuiltinValidated:
			case IROp::CallMethodBindValidated:
				if (const auto it = ptr_slots.find(inst.args[inst.args.size() - 1].id); it != ptr_slots.end()) {
					live.erase(it->value);
				}
				break;

			case IROp::Assign:
			case IROp::AssignNull:
			case IROp::AssignTrue:
			case IROp::AssignFalse:
			case IROp::AssignTypedBuiltin:
			case IROp::AssignTypedNative:
			case IROp::AssignTypedScript:
			case IROp::SetNamed:
			case IROp::SetNamedValidated:
			case IROp::StoreRealMemberF64:
			case IROp::SetMember:
			case IROp::SetKeyed:
				if (inst.op != IROp::StoreRealMemberF64 || inst.args.size() > 1) {
					if (const auto it = ptr_slots.find(inst.args[0].id); it != ptr_slots.end()) {
						live.erase(it->value);
					}
				}
				break;

			case IROp::GetNamed:
			case IROp::GetNamedValidated:
				if (const auto it = ptr_slots.find(inst.args[1].id); it != ptr_slots.end()) {
					live.erase(it->value);
				}
				break;

			case IROp::GetKeyed:
				if (const auto it = ptr_slots.find(inst.args[2].id); it != ptr_slots.end()) {
					live.erase(it->value);
				}
				break;

			case IROp::GetMember:
				if (const auto it = ptr_slots.find(inst.args[0].id); it != ptr_slots.end()) {
					live.erase(it->value);
				}
				break;

			case IROp::SetIndexedValidated:
			case IROp::SetKeyedValidated:
				if (const auto it = ptr_slots.find(inst.args[0].id); it != ptr_slots.end()) {
					live.erase(it->value);
				}
				break;

			case IROp::GetIndexedValidated:
			case IROp::GetKeyedValidated:
				if (const auto it = ptr_slots.find(inst.args[2].id); it != ptr_slots.end()) {
					live.erase(it->value);
				}
				break;

			case IROp::LoadParam:
			case IROp::LoadF64:
				if (_ir_can_elide_store(inst.mem_loc)) {
					_ir_live_add(live, _ir_slot_key(inst.mem_loc), IR_LIVE_VALUE);
				}
				break;

			case IROp::LoadPtr:
				if (_ir_can_elide_store(inst.mem_loc)) {
					_ir_live_add(live, _ir_slot_key(inst.mem_loc), IR_LIVE_VALUE | IR_LIVE_TYPE);
				}
				break;

			case IROp::StoreI64:
			case IROp::StoreF64:
				if (_ir_can_elide_store(inst.mem_loc)) {
					_ir_live_remove(live, _ir_slot_key(inst.mem_loc), IR_LIVE_VALUE);
				}
				break;

			case IROp::StoreType:
				if (_ir_can_elide_store(inst.mem_loc)) {
					_ir_live_remove(live, _ir_slot_key(inst.mem_loc), IR_LIVE_TYPE);
				}
				break;

			default:
				break;
		}
	}

	return live;
}

static Vector<HashMap<uint64_t, uint8_t>> _ir_analyze_live_out_sets(const Vector<IRBlock> &p_blocks, const Vector<Vector<int>> &p_successors) {
	Vector<HashMap<uint64_t, uint8_t>> live_in;
	Vector<HashMap<uint64_t, uint8_t>> live_out;
	live_in.resize(p_blocks.size());
	live_out.resize(p_blocks.size());

	bool changed = true;
	while (changed) {
		changed = false;

		for (int block_idx = p_blocks.size() - 1; block_idx >= 0; block_idx--) {
			HashMap<uint64_t, uint8_t> out_live;
			for (int succ_idx : p_successors[block_idx]) {
				for (const KeyValue<uint64_t, uint8_t> &E : live_in[succ_idx]) {
					_ir_live_add(out_live, E.key, E.value);
				}
			}

			if (!_ir_live_sets_equal(live_out[block_idx], out_live)) {
				live_out.write[block_idx] = out_live;
				changed = true;
			}

			HashMap<uint64_t, uint8_t> in_live = _ir_transfer_live_values(p_blocks[block_idx], out_live);
			if (!_ir_live_sets_equal(live_in[block_idx], in_live)) {
				live_in.write[block_idx] = in_live;
				changed = true;
			}
		}
	}

	return live_out;
}

class IRDeadStoresPass : public IROptPassBase {
public:
	virtual Vector<IRBlock> run(const IRCFG &p_cfg, const Vector<IRBlock> &p_blocks) const override {
		Vector<IRBlock> final_blocks;
		final_blocks.resize(p_blocks.size());
		const Vector<HashMap<uint64_t, uint8_t>> p_live_out = _ir_analyze_live_out_sets(p_blocks, p_cfg.successors);

		for (int block_idx = 0; block_idx < p_blocks.size(); block_idx++) {
			const IRBlock &opt_block = p_blocks[block_idx];
			IRBlock &final_block = final_blocks.write[block_idx];
			final_block.id = opt_block.id;
			final_block.has_label = opt_block.has_label;
			final_block.label = opt_block.label;

			Vector<bool> remove_inst;
			remove_inst.resize_initialized(opt_block.code.size());

			HashMap<uint64_t, uint8_t> live = p_live_out[block_idx];
			HashMap<uint32_t, uint64_t> ptr_slots;
			for (const IRInst &inst : opt_block.code) {
				if (inst.op == IROp::LoadPtr && _ir_can_elide_store(inst.mem_loc)) {
					ptr_slots[inst.dst.id] = _ir_slot_key(inst.mem_loc);
				}
			}

			for (int inst_idx = opt_block.code.size() - 1; inst_idx >= 0; inst_idx--) {
				const IRInst &inst = opt_block.code[inst_idx];

				switch (inst.op) {
					case IROp::CallBinOp:
					case IROp::CallOperator:
						if (const auto it = ptr_slots.find(inst.args[2].id); it != ptr_slots.end()) {
							live.erase(it->value);
						}
						break;

					case IROp::IterateBegin:
					case IROp::Iterate:
						if (const auto it = ptr_slots.find(inst.args[1].id); it != ptr_slots.end()) {
							live.erase(it->value);
						}
						if (const auto it = ptr_slots.find(inst.args[2].id); it != ptr_slots.end()) {
							live.erase(it->value);
						}
						break;

					case IROp::Construct:
					case IROp::ConstructValidated:
					case IROp::ConstructArray:
					case IROp::ConstructDictionary:
						if (const auto it = ptr_slots.find(inst.args[inst.args.size() - 1].id); it != ptr_slots.end()) {
							live.erase(it->value);
						}
						break;

					case IROp::ConstructTypedArray:
					case IROp::ConstructTypedDictionary:
						if (const auto it = ptr_slots.find(inst.args[inst.args.size() - 2].id); it != ptr_slots.end()) {
							live.erase(it->value);
						}
						break;

					case IROp::AssignTypedArray:
					case IROp::AssignTypedDictionary:
						if (const auto it = ptr_slots.find(inst.args[0].id); it != ptr_slots.end()) {
							live.erase(it->value);
						}
						break;

					case IROp::Call:
					case IROp::CallSelf:
					case IROp::CallUtility:
					case IROp::CallUtilityValidated:
					case IROp::CallBuiltinValidated:
					case IROp::CallMethodBindValidated:
						if (const auto it = ptr_slots.find(inst.args[inst.args.size() - 1].id); it != ptr_slots.end()) {
							live.erase(it->value);
						}
						break;

					case IROp::Assign:
					case IROp::AssignNull:
					case IROp::AssignTrue:
					case IROp::AssignFalse:
					case IROp::AssignTypedBuiltin:
					case IROp::AssignTypedNative:
					case IROp::AssignTypedScript:
					case IROp::SetNamed:
					case IROp::SetNamedValidated:
					case IROp::StoreRealMemberF64:
					case IROp::SetMember:
					case IROp::SetKeyed:
						if (inst.op != IROp::StoreRealMemberF64 || inst.args.size() > 1) {
							if (const auto it = ptr_slots.find(inst.args[0].id); it != ptr_slots.end()) {
								live.erase(it->value);
							}
						}
						break;

					case IROp::GetNamed:
					case IROp::GetNamedValidated:
						if (const auto it = ptr_slots.find(inst.args[1].id); it != ptr_slots.end()) {
							live.erase(it->value);
						}
						break;

					case IROp::GetKeyed:
						if (const auto it = ptr_slots.find(inst.args[2].id); it != ptr_slots.end()) {
							live.erase(it->value);
						}
						break;

					case IROp::GetMember:
						if (const auto it = ptr_slots.find(inst.args[0].id); it != ptr_slots.end()) {
							live.erase(it->value);
						}
						break;

					case IROp::SetIndexedValidated:
					case IROp::SetKeyedValidated:
						if (const auto it = ptr_slots.find(inst.args[0].id); it != ptr_slots.end()) {
							live.erase(it->value);
						}
						break;

					case IROp::GetIndexedValidated:
					case IROp::GetKeyedValidated:
						if (const auto it = ptr_slots.find(inst.args[2].id); it != ptr_slots.end()) {
							live.erase(it->value);
						}
						break;

					case IROp::LoadParam:
					case IROp::LoadF64:
						if (_ir_can_elide_store(inst.mem_loc)) {
							_ir_live_add(live, _ir_slot_key(inst.mem_loc), IR_LIVE_VALUE);
						}
						break;

					case IROp::LoadPtr:
						if (_ir_can_elide_store(inst.mem_loc)) {
							_ir_live_add(live, _ir_slot_key(inst.mem_loc), IR_LIVE_VALUE | IR_LIVE_TYPE);
						}
						break;

					case IROp::StoreI64:
					case IROp::StoreF64: {
						if (_ir_can_elide_store(inst.mem_loc)) {
							const uint64_t key = _ir_slot_key(inst.mem_loc);
							const auto live_it = live.find(key);
							if (live_it == live.end() || (live_it->value & IR_LIVE_VALUE) == 0) {
								remove_inst.write[inst_idx] = true;
							} else {
								_ir_live_remove(live, key, IR_LIVE_VALUE);
							}
						}
					} break;

					case IROp::StoreType: {
						if (_ir_can_elide_store(inst.mem_loc)) {
							const uint64_t key = _ir_slot_key(inst.mem_loc);
							const auto live_it = live.find(key);
							if (live_it == live.end() || (live_it->value & IR_LIVE_TYPE) == 0) {
								remove_inst.write[inst_idx] = true;
							} else {
								_ir_live_remove(live, key, IR_LIVE_TYPE);
							}
						}
					} break;

					default:
						break;
				}
			}

			for (int inst_idx = 0; inst_idx < opt_block.code.size(); inst_idx++) {
				if (!remove_inst[inst_idx]) {
					final_block.code.push_back(opt_block.code[inst_idx]);
				}
			}
		}

		return final_blocks;
	}
};

static bool _ir_is_compare_op(IROp p_op) {
	return p_op == IROp::EqI64 || p_op == IROp::EqF64 ||
			p_op == IROp::NeI64 || p_op == IROp::NeF64 ||
			p_op == IROp::LtI64 || p_op == IROp::LtF64 ||
			p_op == IROp::LeI64 || p_op == IROp::LeF64 ||
			p_op == IROp::GtI64 || p_op == IROp::GtF64 ||
			p_op == IROp::GeI64 || p_op == IROp::GeF64;
}

static bool _ir_value_used_later_in_block(const Vector<IRInst> &p_code, int p_start_idx, ValueId p_value) {
	for (int idx = p_start_idx; idx < p_code.size(); idx++) {
		for (const ValueId &arg : p_code[idx].args) {
			if (arg.id == p_value.id) {
				return true;
			}
		}
	}
	return false;
}

static bool _ir_try_fold_compare_jump(IRInst &r_jump, const IRInst &p_compare) {
	if (r_jump.op != IROp::JumpCc || r_jump.args.size() != 1 || IRCond(r_jump.aux) != IRCond::EQ) {
		return false;
	}

	if (!_ir_is_compare_op(p_compare.op) || p_compare.dst.id != r_jump.args[0].id) {
		return false;
	}

	IRCond folded_cond = IRCond::EQ;
	switch (p_compare.op) {
		case IROp::EqI64:
		case IROp::EqF64:
			folded_cond = IRCond::NE;
			break;
		case IROp::NeI64:
		case IROp::NeF64:
			folded_cond = IRCond::EQ;
			break;
		case IROp::LtI64:
		case IROp::LtF64:
			folded_cond = IRCond::GE;
			break;
		case IROp::LeI64:
		case IROp::LeF64:
			folded_cond = IRCond::GT;
			break;
		case IROp::GtI64:
		case IROp::GtF64:
			folded_cond = IRCond::LE;
			break;
		case IROp::GeI64:
		case IROp::GeF64:
			folded_cond = IRCond::LT;
			break;
		default:
			return false;
	}

	r_jump.args = p_compare.args;
	r_jump.aux = uint64_t(folded_cond);
	return true;
}

class IRFoldCompareJumpsPass : public IROptPassBase {
public:
	virtual Vector<IRBlock> run(const IRCFG &p_cfg, const Vector<IRBlock> &p_blocks) const override {
		(void)p_cfg;
		Vector<IRBlock> folded_blocks;
		folded_blocks.resize(p_blocks.size());

		for (int block_idx = 0; block_idx < p_blocks.size(); block_idx++) {
			const IRBlock &src_block = p_blocks[block_idx];
			IRBlock &dst_block = folded_blocks.write[block_idx];
			dst_block.id = src_block.id;
			dst_block.has_label = src_block.has_label;
			dst_block.label = src_block.label;

			Vector<bool> remove_inst;
			remove_inst.resize_initialized(src_block.code.size());
			Vector<IRInst> rewritten_code = src_block.code;

			for (int inst_idx = 1; inst_idx < src_block.code.size(); inst_idx++) {
				IRInst &jump = rewritten_code.write[inst_idx];
				const IRInst &compare = src_block.code[inst_idx - 1];
				if (_ir_try_fold_compare_jump(jump, compare) &&
						!_ir_value_used_later_in_block(src_block.code, inst_idx + 1, compare.dst)) {
					remove_inst.write[inst_idx - 1] = true;
				}
			}

			for (int inst_idx = 0; inst_idx < src_block.code.size(); inst_idx++) {
				if (!remove_inst[inst_idx]) {
					dst_block.code.push_back(rewritten_code[inst_idx]);
				}
			}
		}

		return folded_blocks;
	}
};

static const IRKnownValuesPass _ir_known_values_pass;
static const IRDeadStoresPass _ir_dead_stores_pass;
static const IRFoldCompareJumpsPass _ir_fold_compare_jumps_pass;

static const IROptPassBase *const _IR_OPT_PASSES[] = {
	&_ir_known_values_pass,
	&_ir_dead_stores_pass,
	&_ir_fold_compare_jumps_pass,
};

IROptimizationResult optimize_ir(const IRBuilder &p_ir) {
	IROptimizationResult result;
	const IRCFG raw_cfg = _ir_build_cfg(p_ir);
	result.raw_blocks = raw_cfg.blocks;
	result.optimized_blocks = raw_cfg.blocks;

	for (const IROptPassBase *pass : _IR_OPT_PASSES) {
		const IRCFG pass_cfg = _ir_build_cfg_from_blocks(result.optimized_blocks);
		result.optimized_blocks = pass->run(pass_cfg, result.optimized_blocks);
	}

	for (const IRBlock &block : result.optimized_blocks) {
		for (const IRInst &inst : block.code) {
			IRValueKind value_kind = IRValueKind::GP64;
			if (_ir_try_get_value_kind(inst, value_kind)) {
				result.value_kind_map[inst.dst.id] = value_kind;
			}
		}
	}

	return result;
}
