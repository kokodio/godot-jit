#pragma once
#include <asmjit/core.h>
#include <asmjit/x86.h>

using Compiler = asmjit::x86::Compiler;
using Vec = asmjit::x86::Vec;
using Gp = asmjit::x86::Gp;
using Mem = asmjit::x86::Mem;
namespace Arch {
using namespace ::asmjit::x86;
}

static constexpr int PTR_SIZE = sizeof(void *);
