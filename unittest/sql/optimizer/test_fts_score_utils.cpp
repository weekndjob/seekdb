// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "sql/optimizer/ob_fts_score_utils.h"
#include <gtest/gtest.h>

using namespace oceanbase::common;
using namespace oceanbase::sql;

class FtsScoreElision : public ::testing::Test {
protected:
  void SetUp() override {
    count_.set_expr_type(T_FUN_COUNT);
    table_.type_ = TableItem::BASE_TABLE;
    SelectItem item;
    item.expr_ = &count_;
    ASSERT_EQ(OB_SUCCESS, stmt_.add_select_item(item));
    ASSERT_EQ(OB_SUCCESS, stmt_.add_agg_item(count_));
    ASSERT_EQ(OB_SUCCESS, stmt_.get_table_items().push_back(&table_));
    ASSERT_EQ(OB_SUCCESS, stmt_.add_condition_expr(&match_));
  }
  ObSelectStmt stmt_;
  ObMatchFunRawExpr match_;
  ObAggFunRawExpr count_;
  TableItem table_;
  ObConstRawExpr value_;
};

TEST_F(FtsScoreElision, BareCountConsumesMembershipOnly) {
  EXPECT_TRUE(can_omit_fts_score_for_count(stmt_, match_));
  ObArenaAllocator allocator;
  ObOpRawExpr boolean(allocator);
  boolean.set_expr_type(T_OP_BOOL);
  ASSERT_EQ(OB_SUCCESS, boolean.set_param_expr(&match_));
  stmt_.get_condition_exprs().at(0) = &boolean;
  EXPECT_TRUE(can_omit_fts_score_for_count(stmt_, match_));
}

TEST_F(FtsScoreElision, ThresholdsAndOtherConditionsKeepScores) {
  ObArenaAllocator allocator;
  ObOpRawExpr comparison(allocator);
  comparison.set_expr_type(T_OP_GT);
  ASSERT_EQ(OB_SUCCESS, comparison.set_param_exprs(&match_, &value_));
  stmt_.get_condition_exprs().at(0) = &comparison;
  EXPECT_FALSE(can_omit_fts_score_for_count(stmt_, match_));
  stmt_.get_condition_exprs().at(0) = &match_;
  ASSERT_EQ(OB_SUCCESS, stmt_.add_condition_expr(&value_));
  EXPECT_FALSE(can_omit_fts_score_for_count(stmt_, match_));
}

TEST_F(FtsScoreElision, ProjectionGroupingAndRankingKeepScores) {
  stmt_.get_select_item(0).expr_ = &match_;
  EXPECT_FALSE(can_omit_fts_score_for_count(stmt_, match_));
  stmt_.get_select_item(0).expr_ = &count_;
  stmt_.set_limit_offset(&value_, nullptr);
  EXPECT_FALSE(can_omit_fts_score_for_count(stmt_, match_));
  stmt_.set_limit_offset(nullptr, nullptr);
  ASSERT_EQ(OB_SUCCESS, stmt_.get_group_exprs().push_back(&value_));
  EXPECT_FALSE(can_omit_fts_score_for_count(stmt_, match_));
  stmt_.get_group_exprs().reuse();
  ASSERT_EQ(OB_SUCCESS, stmt_.get_having_exprs().push_back(&value_));
  EXPECT_FALSE(can_omit_fts_score_for_count(stmt_, match_));
}

TEST_F(FtsScoreElision, BooleanAndBoostedSearchKeepScores) {
  match_.set_mode_flag(BOOLEAN_MODE);
  EXPECT_FALSE(can_omit_fts_score_for_count(stmt_, match_));
  match_.set_mode_flag(NATURAL_LANGUAGE_MODE);
  ASSERT_EQ(OB_SUCCESS, match_.get_columns_boosts().push_back(&value_));
  EXPECT_FALSE(can_omit_fts_score_for_count(stmt_, match_));
  match_.get_columns_boosts().reuse();
  match_.set_param_text_expr(&value_);
  EXPECT_FALSE(can_omit_fts_score_for_count(stmt_, match_));
}
