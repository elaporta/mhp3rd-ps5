// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "psprecomp/runtime.hpp"

namespace yakumo::game_import {
// A same-thread HLE callback changes PC without changing the thread token.
// Return normally only if neither the scheduler nor the handler redirected it.
inline void complete(psprecomp::AllegrexContext& ctx,psprecomp::RuntimeExecutionContextToken token,
                     std::uint32_t import_pc){
    if(psprecomp::runtime_execution_context_matches(token)&&ctx.pc==import_pc)ctx.pc=ctx.gpr[31];
}
}
