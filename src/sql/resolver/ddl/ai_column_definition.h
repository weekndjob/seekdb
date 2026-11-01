// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once

#include "sql/parser/parse_node.h"
#include "lib/string/ob_string.h"
#include "lib/ob_errno.h"
#include "lib/container/ob_iarray.h"

namespace oceanbase { namespace sql {

// String views point into the parse tree and must be copied before it is freed.
struct AiColumnDefinition {
  common::ObString expression;
  common::ObString model;
  int64_t max_retries = 0;
  bool embedding = false;
};

inline int validate_ai_column_definition(const ParseNode &column, AiColumnDefinition &out)
{
  using namespace common;
  out = AiColumnDefinition();
  if (column.type_ != T_COLUMN_DEFINITION || column.num_child_ != 8 || column.children_ == nullptr ||
      column.children_[4] == nullptr) return OB_INVALID_ARGUMENT;
  const ParseNode &expr = *column.children_[4];
  if (expr.type_ != T_FUN_SYS || expr.num_child_ != 2 || expr.children_ == nullptr ||
      expr.children_[0] == nullptr || expr.children_[1] == nullptr) return OB_NOT_SUPPORTED;
  const ParseNode &name = *expr.children_[0];
  if (name.str_value_ == nullptr || name.str_len_ <= 0) return OB_INVALID_ARGUMENT;
  const ObString function(name.str_len_, name.str_value_);
  if (function.case_compare("AI_EMBED") == 0) out.embedding = true;
  else if (function.case_compare("AI_COMPLETE") != 0) return OB_NOT_SUPPORTED;
  const ParseNode &args = *expr.children_[1];
  if (args.type_ != T_EXPR_LIST || args.children_ == nullptr || args.num_child_ < 2 ||
      args.num_child_ > 3 || args.children_[0] == nullptr || args.children_[1] == nullptr) return OB_INVALID_ARGUMENT;
  const ParseNode &model = *args.children_[0];
  if ((model.type_ != T_VARCHAR && model.type_ != T_CHAR) || model.str_value_ == nullptr || model.str_len_ <= 0)
    return OB_INVALID_ARGUMENT;
  out.model.assign_ptr(model.str_value_, model.str_len_);
  if (expr.str_value_ == nullptr || expr.str_len_ <= 0) return OB_INVALID_ARGUMENT;
  out.expression.assign_ptr(expr.str_value_, expr.str_len_);
  const ParseNode *options = column.children_[5];
  bool seen_sync = false, seen_retries = false;
  if (options != nullptr) {
    if (options->type_ != T_EXPR_LIST || options->children_ == nullptr) return OB_INVALID_ARGUMENT;
    for (int64_t i = 0; i < options->num_child_; ++i) {
      const ParseNode *opt = options->children_[i];
      if (opt == nullptr || opt->type_ != T_OP_EQ || opt->num_child_ != 2 || opt->children_ == nullptr ||
          opt->children_[0] == nullptr || opt->children_[1] == nullptr) return OB_INVALID_ARGUMENT;
      const ParseNode &key = *opt->children_[0];
      if (key.type_ != T_COLUMN_REF || key.num_child_ != 3 || key.children_ == nullptr ||
          key.children_[0] != nullptr || key.children_[1] != nullptr || key.children_[2] == nullptr ||
          key.children_[2]->str_value_ == nullptr) return OB_INVALID_ARGUMENT;
      const ObString name(key.children_[2]->str_len_, key.children_[2]->str_value_);
      const ParseNode &value = *opt->children_[1];
      if (name.case_compare("sync_mode") == 0) {
        if (seen_sync || (value.type_ != T_VARCHAR && value.type_ != T_CHAR) || value.str_value_ == nullptr)
          return OB_INVALID_ARGUMENT;
        seen_sync = true;
        if (ObString(value.str_len_, value.str_value_).case_compare("async") != 0) return OB_NOT_SUPPORTED;
      } else if (name.case_compare("max_retries") == 0) {
        if (seen_retries || value.type_ != T_INT || value.value_ < 0) return OB_INVALID_ARGUMENT;
        seen_retries = true;
        out.max_retries = value.value_;
      } else return OB_NOT_SUPPORTED;
    }
  }
  return OB_SUCCESS;
}

// Collect distinct column references from the expression tree. Literals and
// function names are not dependencies. Views refer to the parser's allocator.
struct AiColumnReference {
  common::ObString database;
  common::ObString table;
  common::ObString column;
};

inline int collect_ai_column_references(const ParseNode &node,
                                        common::ObIArray<AiColumnReference> &out,
                                        const int64_t depth = 0)
{
  using namespace common;
  if (depth > 128) return OB_SIZE_OVERFLOW;
  if (node.type_ == T_COLUMN_REF) {
    if (node.num_child_ != 3 || node.children_ == nullptr || node.children_[2] == nullptr ||
        node.children_[2]->str_value_ == nullptr || node.children_[2]->str_len_ <= 0)
      return OB_INVALID_ARGUMENT;
    AiColumnReference ref;
    for (int64_t i = 0; i < 3; ++i) {
      const ParseNode *part = node.children_[i];
      if (part != nullptr) {
        if (part->str_value_ == nullptr || part->str_len_ <= 0) return OB_INVALID_ARGUMENT;
        ObString value(part->str_len_, part->str_value_);
        if (i == 0) ref.database = value;
        else if (i == 1) ref.table = value;
        else ref.column = value;
      }
    }
    for (int64_t i = 0; i < out.count(); ++i) {
      if (out.at(i).database.case_compare(ref.database) == 0 &&
          out.at(i).table.case_compare(ref.table) == 0 &&
          out.at(i).column.case_compare(ref.column) == 0) return OB_SUCCESS;
    }
    return out.push_back(ref);
  }
  if (node.num_child_ > 0 && node.children_ == nullptr) return OB_INVALID_ARGUMENT;
  for (int64_t i = 0; i < node.num_child_; ++i) {
    if (node.children_[i] != nullptr) {
      const int ret = collect_ai_column_references(*node.children_[i], out, depth + 1);
      if (ret != OB_SUCCESS) return ret;
    }
  }
  return OB_SUCCESS;
}

} } // namespace oceanbase::sql
