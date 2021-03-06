/*
 * syc, a compiler for SysY
 * Copyright (C) 2020  nzh63, skywf21
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "optimize_ir.h"

#include <list>
#include <set>
#include <unordered_map>

#include "config.h"
#include "context_asm.h"
#include "ir.h"

namespace {
void dead_code_elimination(IRList &ir) {
  ContextASM ctx(&ir, ir.begin());
  for (auto it = ir.begin(); it != ir.end(); it++) {
    ctx.set_ir_timestamp(*it);
  }

  for (auto it = std::prev(ir.end()); it != ir.begin(); it--) {
    ctx.set_var_latest_use_timestamp(*it);
    auto cur = ctx.ir_to_time[&*it];
    if (it->dest.type == OpName::Type::Var && it->dest.name[0] == '%' &&
        it->op_code != IR::OpCode::CALL && it->op_code != IR::OpCode::PHI_MOV) {
      if (ctx.var_latest_use_timestamp.find(it->dest.name) ==
              ctx.var_latest_use_timestamp.end() ||
          ctx.var_latest_use_timestamp[it->dest.name] <= cur) {
        it = ir.erase(it);
      }
    }
  }

  for (auto it = ir.begin(); it != ir.end(); it++) {
    ctx.set_var_define_timestamp(*it);
  }
}

struct compare_ir {
  bool operator()(const std::pair<IR, int> &_a,
                  const std::pair<IR, int> &_b) const {
    const auto &a = _a.first, &b = _b.first;
    if (a.op_code != b.op_code) return a.op_code < b.op_code;

#define F(op1)                                                       \
  if (a.op1.type != b.op1.type) return a.op1.type < b.op1.type;      \
  if (a.op1.type == OpName::Type::Var && a.op1.name != b.op1.name)   \
    return a.op1.name < b.op1.name;                                  \
  if (a.op1.type == OpName::Type::Imm && a.op1.value != b.op1.value) \
    return a.op1.value < b.op1.value;
    F(op1)
    F(op2)
    F(op3)
#undef F
    return false;
  }
};

void local_common_subexpression_elimination(IRList &ir) {
  std::set<std::pair<IR, int>, compare_ir> maybe_opt;
  std::set<std::string> mutability_var;

  ContextASM ctx(&ir, ir.begin());
  for (auto it = ir.begin(); it != ir.end(); it++) {
    ctx.set_ir_timestamp(*it);
  }

  for (auto it = ir.begin(); it != ir.end(); it++) {
    if (it->op_code != IR::OpCode::PHI_MOV) {
      mutability_var.insert(it->dest.name);
    }
    if (it->op_code != IR::OpCode::LABEL ||
        it->op_code != IR::OpCode::FUNCTION_BEGIN) {
      maybe_opt.clear();
    }
    if (it->dest.type == OpName::Type::Var && it->dest.name[0] == '%' &&
        it->op1.type != OpName::Type::Null &&
        it->op2.type != OpName::Type::Null &&
        it->op_code != IR::OpCode::MOVEQ && it->op_code != IR::OpCode::MOVNE &&
        it->op_code != IR::OpCode::MOVGT && it->op_code != IR::OpCode::MOVGE &&
        it->op_code != IR::OpCode::MOVLT && it->op_code != IR::OpCode::MOVLE &&
        it->op_code != IR::OpCode::MALLOC_IN_STACK &&
        it->op_code != IR::OpCode::LOAD && it->op_code != IR::OpCode::MOV &&
        it->op_code != IR::OpCode::PHI_MOV) {
      if (maybe_opt.find({*it, 0}) != maybe_opt.end()) {
        auto opt_ir = maybe_opt.find({*it, 0})->first;
        auto time = maybe_opt.find({*it, 0})->second;
        if (mutability_var.find(opt_ir.dest.name) == mutability_var.end()) {
          // 避免相距太远子表达式有望延长生命周期而溢出
          if (ctx.ir_to_time[&*it] - time < 20) {
            IR temp(IR::OpCode::MOV, it->dest, opt_ir.dest);
            it = ir.erase(it);
            it = ir.insert(it, temp);
            it--;
          }
        }
      }
      maybe_opt.insert({*it, ctx.ir_to_time[&*it]});
    }
  }
}

bool is_constexpr_function(const IRList &irs, IRList::const_iterator begin,
                           IRList::const_iterator end) {
  for (auto it = begin; it != end; it++) {
    auto &ir = *it;
#define F(op)                                                      \
  if (ir.op.type == OpName::Type::Var) {                           \
    if (ir.op.name[0] != '%' && ir.op.name.substr(0, 4) != "$arg") \
      return false;                                                \
  }
    F(op1);
    F(op2);
    F(op3);
    F(dest);
#undef F
    if (ir.op_code == IR::OpCode::INFO && ir.label == "NOT CONSTEXPR") {
      return false;
    }
  }
  return true;
}

std::set<std::string> find_constexpr_function(const IRList &irs) {
  std::set<std::string> ret;
  IRList::const_iterator function_begin_it;
  for (auto outter_it = irs.begin(); outter_it != irs.end(); outter_it++) {
    auto &ir = *outter_it;
    if (ir.op_code == IR::OpCode::FUNCTION_BEGIN) {
      function_begin_it = outter_it;
    } else if (ir.op_code == IR::OpCode::FUNCTION_END) {
      if (is_constexpr_function(irs, function_begin_it, outter_it)) {
        ret.insert(function_begin_it->label);
      }
    }
  }
  return ret;
}

void local_common_constexpr_function(
    IRList &ir, const std::set<std::string> &constexpr_function) {
  typedef std::unordered_map<int, OpName> CallArgs;
  std::unordered_map<std::string, std::vector<std::pair<CallArgs, std::string>>>
      calls;
  for (auto it = ir.begin(); it != ir.end(); it++) {
    if (it->op_code == IR::OpCode::LABEL ||
        it->op_code == IR::OpCode::FUNCTION_BEGIN) {
      calls.clear();
    }
    if (it->op_code == IR::OpCode::CALL) {
      CallArgs args;
      auto function_name = it->label;
      if (constexpr_function.find(function_name) == constexpr_function.end()) {
        continue;
      }
      auto it2 = std::prev(it);
      while (it2->op_code == IR::OpCode::SET_ARG) {
        args.insert({it2->dest.value, it2->op1});
        it2--;
      }
      bool can_optimize = true;
      for (auto kv : args) {
        if (kv.second.type == OpName::Type::Var) {
          if (kv.second.name[0] != '%') {
            can_optimize = false;
            break;
          }
          if (kv.second.name.substr(0, 2) == "%&") {
            can_optimize = false;
            break;
          }
        }
      }
      if (!can_optimize) continue;
      bool has_same_call = false;
      auto eq = [](const OpName &a, const OpName &b) -> bool {
        if (a.type != b.type) return false;
        if (a.type == OpName::Type::Imm) return a.value == b.value;
        if (a.type == OpName::Type::Var) return a.name == b.name;
        return true;
      };
      std::string prev_call_result;
      for (const auto &prev_call : calls[function_name]) {
        bool same = true;
        const auto &prev_call_args = prev_call.first;
        prev_call_result = prev_call.second;
        for (auto &kv : args) {
          if (prev_call_args.find(kv.first) == prev_call_args.end()) {
            same = false;
            break;
          }
          if (!eq(kv.second, prev_call_args.at(kv.first))) {
            same = false;
            break;
          }
        }
        if (same) {
          has_same_call = true;
          break;
        }
      }
      if (!has_same_call) {
        if (it->dest.type == OpName::Type::Var) {
          calls[function_name].push_back({args, it->dest.name});
        }
      } else {
        if (it->dest.type == OpName::Type::Var) {
          it->op_code = IR::OpCode::MOV;
          it->op1.type = OpName::Type::Var;
          it->op1.name = prev_call_result;
          it->op2.type = OpName::Type::Null;
          it->op2.type = OpName::Type::Null;
          it->label.clear();
        }
      }
    }
  }
}
}  // namespace

void optimize_ir(IRList &ir) {
  auto constexpr_function = find_constexpr_function(ir);
  for (int i = 0; i < 2; i++) {
    local_common_subexpression_elimination(ir);
    local_common_constexpr_function(ir, constexpr_function);
    dead_code_elimination(ir);
  }
}

namespace {
bool loop_invariant_code_motion(IRList &ir_before, IRList &ir_cond,
                                IRList &ir_jmp, IRList &ir_do,
                                IRList &ir_continue) {
  std::set<std::string> never_write_var;
  bool do_optimize = false;
  for (auto irs :
       std::vector<IRList *>({&ir_cond, &ir_jmp, &ir_do, &ir_continue})) {
    for (auto &ir : *irs) {
      if (ir.op_code == IR::OpCode::LOAD || ir.op_code == IR::OpCode::CALL) {
        continue;
      }
#define F(op)                            \
  if (ir.op.type == OpName::Type::Var) { \
    never_write_var.insert(ir.op.name);  \
  }
      F(op1);
      F(op2);
      F(op3);
#undef F
    }
  }
  for (auto irs :
       std::vector<IRList *>({&ir_cond, &ir_jmp, &ir_do, &ir_continue})) {
    for (auto &ir : *irs) {
      if (ir.dest.type == OpName::Type::Var) {
        never_write_var.erase(ir.dest.name);
      }
    }
  }
  for (auto irs :
       std::vector<IRList *>({&ir_cond, &ir_jmp, &ir_do, &ir_continue})) {
    for (auto &ir : *irs) {
      bool can_optimize = true;
      if (ir.op_code == IR::OpCode::LOAD || ir.op_code == IR::OpCode::CALL ||
          ir.op_code == IR::OpCode::PHI_MOV ||
          ir.op_code == IR::OpCode::LABEL || ir.op_code == IR::OpCode::CMP ||
          ir.op_code == IR::OpCode::MOVEQ || ir.op_code == IR::OpCode::MOVNE ||
          ir.op_code == IR::OpCode::MOVGT || ir.op_code == IR::OpCode::MOVGE ||
          ir.op_code == IR::OpCode::MOVLT || ir.op_code == IR::OpCode::MOVLE ||
          ir.op_code == IR::OpCode::NOOP) {
        can_optimize = false;
      }
#define F(op)                                                      \
  if (ir.op.type == OpName::Type::Var &&                           \
      never_write_var.find(ir.op.name) == never_write_var.end()) { \
    can_optimize = false;                                          \
  }
      F(op1);
      F(op2);
      F(op3);
#undef F
      if (ir.dest.type != OpName::Type::Var) {
        can_optimize = false;
      }
      if (can_optimize) {
        ir_before.push_back(ir);
        ir.op_code = IR::OpCode::NOOP;
        ir.op1.type = OpName::Type::Null;
        ir.op2.type = OpName::Type::Null;
        ir.op3.type = OpName::Type::Null;
        ir.dest.type = OpName::Type::Null;
        do_optimize = true;
      }
    }
  }
  return do_optimize;
}
}  // namespace

void optimize_loop_ir(IRList &ir_before, IRList &ir_cond, IRList &ir_jmp,
                      IRList &ir_do, IRList &ir_continue) {
  if (config::optimize_level > 0) {
    while (loop_invariant_code_motion(ir_before, ir_cond, ir_jmp, ir_do,
                                      ir_continue))
      ;
  }
}