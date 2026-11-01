// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "sql/parser/ob_parser.h"
#include "lib/allocator/ob_allocator.h"
#include <gtest/gtest.h>

using namespace oceanbase::sql;
using namespace oceanbase::common;

static const ParseNode *ai_column_definition(const ParseNode *node)
{
  if (node == nullptr) return nullptr;
  if (node->type_ == T_COLUMN_DEFINITION && node->num_child_ == 8) return node;
  for (int i = 0; i < node->num_child_; ++i) {
    if (const ParseNode *found = ai_column_definition(node->children_[i])) return found;
  }
  return nullptr;
}

TEST(AIColumnParser, CreateAndAddPreserveExpressionAndOptions)
{
  for (const char *sql : {
       "CREATE TABLE docs(id BIGINT PRIMARY KEY, content VARCHAR(200), "
       "tags JSON AI COLUMN (AI_COMPLETE('model', content)) "
       "WITH (sync_mode='async', max_retries=2))",
       "ALTER TABLE docs ADD COLUMN emb VECTOR(4) AI COLUMN "
       "(AI_EMBED('embed_model', content)) WITH (sync_mode='async', max_retries=2)"}) {
    ObArenaAllocator allocator;
    ObParser parser(allocator, SMO_DEFAULT);
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(sql), result))
        << sql << ": " << result.error_msg_;
    const ParseNode *column = ai_column_definition(result.result_tree_);
    ASSERT_NE(nullptr, column) << sql;
    ASSERT_NE(nullptr, column->children_[4]);
    ASSERT_NE(nullptr, column->children_[5]);
  }
}
