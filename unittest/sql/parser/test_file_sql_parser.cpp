// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "sql/parser/ob_parser.h"
#include "lib/allocator/ob_allocator.h"
#include <gtest/gtest.h>

using namespace oceanbase::sql;
using namespace oceanbase::common;

static const ParseNode *file_table(const ParseNode *node) {
  if (!node) return nullptr;
  if (node->type_ == T_TABLE_COLLECTION_EXPRESSION && node->num_child_ == 3) return node;
  for (int i = 0; i < node->num_child_; ++i) if (const auto *found = file_table(node->children_[i])) return found;
  return nullptr;
}

TEST(FileSqlParser, TableArgumentsAndAliases) {
  for (const char *sql : {
      "SELECT * FROM read_csv('/tmp/a.csv')",
      "SELECT * FROM read_json('/tmp/a.jsonl', format='newline_delimited') j",
      "SELECT * FROM read_csv('/tmp/a.csv', header=true, sample_size=-1) AS c",
      "SELECT * FROM read_parquet('/tmp/a.parquet') p",
      "SELECT * FROM glob('/tmp/*.csv')"}) {
    ObArenaAllocator allocator;
    ObParser parser(allocator, SMO_DEFAULT);
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(sql), result)) << sql;
    const auto *table = file_table(result.result_tree_);
    ASSERT_NE(nullptr, table) << sql;
    ASSERT_EQ(T_EXPR_LIST, table->children_[0]->type_);
    ASSERT_GE(table->children_[0]->num_child_, 1);
    const auto *path = table->children_[0]->children_[0];
    ASSERT_NE(nullptr, path);
    EXPECT_TRUE(path->type_ == T_VARCHAR || path->type_ == T_CHAR) << "path type=" << path->type_ << ", children=" << path->num_child_;
    EXPECT_GT(path->str_len_, 0);
  }
}

TEST(FileSqlParser, OrdinarySqlIsPreserved) {
  for (const char *sql : {"SELECT 1", "SELECT a FROM t WHERE b=2", "EXPLAIN SELECT * FROM t", "DESCRIBE t"}) {
    ObArenaAllocator allocator; ObParser parser(allocator, SMO_DEFAULT); ParseResult result;
    EXPECT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(sql), result)) << sql << ": " << result.error_msg_;
  }
}

TEST(FileSqlParser, CopyQueriesAndTables) {
  for (const char *sql : {
      "COPY (SELECT id FROM read_csv('/tmp/a.csv')) TO '/tmp/out.csv' (FORMAT CSV, HEADER, NULLSTR 'NULL')",
      "COPY mydb.docs TO '/tmp/out.csv' (HEADER true, DELIMITER ';')",
      "COPY docs TO '/tmp/out.csv'"}) {
    ObArenaAllocator allocator; ObParser parser(allocator, SMO_DEFAULT); ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(sql), result)) << sql << ": " << result.error_msg_;
    ASSERT_NE(nullptr, result.result_tree_);
    const auto *select = result.result_tree_->children_[0];
    ASSERT_EQ(T_SELECT, select->type_);
    ASSERT_NE(nullptr, select->children_[PARSE_SELECT_INTO_EXTRA]);
    EXPECT_EQ(2, select->children_[PARSE_SELECT_INTO_EXTRA]->num_child_);
  }
}
