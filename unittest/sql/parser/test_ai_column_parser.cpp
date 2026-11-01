// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "sql/parser/ob_parser.h"
#include "sql/resolver/ddl/ai_column_definition.h"
#include "lib/allocator/ob_allocator.h"
#include <gtest/gtest.h>
#include "lib/container/ob_se_array.h"

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

TEST(AIColumnParser, ValidatesFunctionAndOptions)
{
  struct Case { const char *expr; const char *opts; int expected; int retries; bool embedding; };
  const Case cases[] = {
    {"AI_COMPLETE('model', content)", "WITH (sync_mode='async', max_retries=2)", OB_SUCCESS, 2, false},
    {"AI_EMBED('embed_model', content)", "", OB_SUCCESS, 0, true},
    {"AI_COMPLETE('', content)", "", OB_INVALID_ARGUMENT, 0, false},
    {"AI_COMPLETE('model', content)", "WITH (sync_mode='sync')", OB_NOT_SUPPORTED, 0, false},
    {"AI_COMPLETE('model', content)", "WITH (max_retries=-1)", OB_INVALID_ARGUMENT, 0, false},
    {"AI_COMPLETE('model', content)", "WITH (max_retries=1, max_retries=2)", OB_INVALID_ARGUMENT, 0, false},
    {"AI_COMPLETE('model', content)", "WITH (unknown=2)", OB_NOT_SUPPORTED, 0, false},
    {"ABS(42)", "", OB_NOT_SUPPORTED, 0, false},
  };
  for (const auto &c : cases) {
    const std::string sql = std::string("CREATE TABLE docs(content VARCHAR(200), tags JSON AI COLUMN (")
      + c.expr + ") " + c.opts + ")";
    ObArenaAllocator allocator; ObParser parser(allocator, SMO_DEFAULT); ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(sql.c_str()), result)) << sql;
    const ParseNode *column = ai_column_definition(result.result_tree_);
    ASSERT_NE(nullptr, column) << sql;
    AiColumnDefinition definition;
    EXPECT_EQ(c.expected, validate_ai_column_definition(*column, definition)) << sql;
    if (c.expected == OB_SUCCESS) {
      EXPECT_EQ(c.retries, definition.max_retries);
      EXPECT_EQ(c.embedding, definition.embedding);
      EXPECT_GT(definition.expression.length(), 0);
      EXPECT_GT(definition.model.length(), 0);
    }
  }
}

TEST(AIColumnParser, FindsDistinctDependenciesInExpressions)
{
  const char *sql = "CREATE TABLE docs(content VARCHAR(200), title VARCHAR(100), "
    "tags JSON AI COLUMN (AI_COMPLETE('model', CONCAT(content, 'title', CONTENT, title))))";
  ObArenaAllocator allocator; ObParser parser(allocator, SMO_DEFAULT); ParseResult result;
  ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(sql), result));
  const ParseNode *column = ai_column_definition(result.result_tree_);
  ASSERT_NE(nullptr, column);
  ObSEArray<AiColumnReference, 4> refs;
  ASSERT_EQ(OB_SUCCESS, collect_ai_column_references(*column->children_[4], refs));
  ASSERT_EQ(2, refs.count());
  EXPECT_EQ(0, refs.at(0).column.case_compare("content"));
  EXPECT_EQ(0, refs.at(1).column.case_compare("title"));
}
