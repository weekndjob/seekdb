// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once

#include "sql/resolver/dml/ob_select_stmt.h"

namespace oceanbase
{
namespace sql
{
// Natural-language retrieval without boosts produces positive scores for
// matching documents. A scalar COUNT(*) with a bare MATCH predicate consumes
// membership only. Keep the scored path for thresholds, projected scores,
// ranking, boolean search, boosts, grouping and more complex predicates.
inline bool can_omit_fts_score_for_count(const ObSelectStmt &stmt,
                                        const ObMatchFunRawExpr &match)
{
  const ObRawExpr *projection = stmt.get_select_item_size() == 1
      ? stmt.get_select_item(0).expr_ : nullptr;
  const ObRawExpr *condition = stmt.get_condition_size() == 1
      ? stmt.get_condition_expr(0) : nullptr;
  if (condition != nullptr && condition->get_expr_type() == T_OP_BOOL
      && condition->get_param_count() == 1) {
    condition = condition->get_param_expr(0);
  }
  return projection != nullptr
      && projection->get_expr_type() == T_FUN_COUNT
      && projection->get_param_count() == 0
      && stmt.get_aggr_item_size() == 1
      && stmt.get_table_size() == 1
      && stmt.get_table_item(0) != nullptr && stmt.get_table_item(0)->is_basic_table()
      && stmt.is_scala_group_by() && !stmt.has_order_by() && !stmt.has_limit()
      && !stmt.has_having() && !stmt.has_window_function() && !stmt.is_set_stmt()
      && condition == &match
      && !match.is_es_match() && match.get_mode_flag() == NATURAL_LANGUAGE_MODE
      && match.get_columns_boosts().empty() && match.get_param_text_expr() == nullptr;
}
} // namespace sql
} // namespace oceanbase
