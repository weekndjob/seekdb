#include <gtest/gtest.h>
#include "lib/allocator/ob_allocator.h"
#include "sql/parser/ob_parser.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

static_assert(T_FILE_TABLE_EXPRESSION != T_FLUSH_PRIVILEGES, "file table parser type must be unique");
static_assert(T_FILE_SQL_OPTION != T_FLUSH_PRIVILEGES, "file option parser type must be unique");

namespace {
bool contains_file_table(const ParseNode *node)
{
  if (node == nullptr) {
    return false;
  }
  if (node->type_ == T_FILE_TABLE_EXPRESSION) {
    return node->num_child_ == 4 && node->children_[0] != nullptr && node->children_[1] != nullptr;
  }
  for (int i = 0; i < node->num_child_; ++i) {
    if (contains_file_table(node->children_[i])) {
      return true;
    }
  }
  return false;
}
}

TEST(FileSqlParser, ReadsFormatsOptionsAliasesAndNativeJoins)
{
  const char *queries[] = {
      "select * from read_csv('/tmp/a.csv')",
      "select * from read_csv('/tmp/a.csv', header=true, sample_size=-1) f",
      "select * from read_json('/tmp/a.jsonl', format='newline_delimited') AS j",
      "select * from read_ndjson('/tmp/a.jsonl')",
      "select count(*) from read_parquet('/tmp/a.parquet')",
      "select * from glob('/tmp/*.csv')",
      "select a.id from read_csv('/tmp/a.csv') a left join read_json('/tmp/b.jsonl') b on a.id=b.id",
      "describe select * from read_csv('/tmp/a.csv')"};
  for (const char *sql : queries) {
    ObArenaAllocator allocator;
    ObParser parser(allocator, 0);
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(sql), result)) << sql;
    EXPECT_TRUE(contains_file_table(result.result_tree_)) << sql;
  }
}

TEST(FileSqlParser, RejectsPositionalOptionsAndIncompleteSyntax)
{
  const char *queries[] = {
      "select * from read_csv('/tmp/a.csv', 'xml')",
      "select * from read_csv('/tmp/a.csv', header=)",
      "select * from read_parquet()"};
  for (const char *sql : queries) {
    ObArenaAllocator allocator;
    ObParser parser(allocator, 0);
    ParseResult result;
    EXPECT_NE(OB_SUCCESS, parser.parse(ObString::make_string(sql), result)) << sql;
  }
}

TEST(FileSqlParser, DistinguishesDescribeFromExplainAndLowersCopyToNativeInto)
{
  for (const char *sql : {"describe select * from read_csv('/tmp/a.csv')",
                          "explain select * from read_csv('/tmp/a.csv')"}) {
    ObArenaAllocator arena;
    ObParser parser(arena, 0);
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(sql), result));
    const ParseNode *node = result.result_tree_->children_[0];
    ASSERT_EQ(T_EXPLAIN, node->type_);
    EXPECT_EQ(0, node->value_); // statement parameter count is preserved
    if (sql[0] == 'd') {
      ASSERT_NE(nullptr, node->children_[0]);
      EXPECT_EQ(T_DEFAULT, node->children_[0]->type_);
    } else { EXPECT_EQ(nullptr, node->children_[0]); }
  }
  const char *queries[] = {
      "copy docs to '/tmp/out.csv'",
      "copy (select id,name from read_csv('/tmp/a.csv') order by id) to '/tmp/out.csv' (format csv,header,nullstr 'NULL')",
      "copy docs to '/tmp/out.csv' (header false,delimiter ';',quote '\"',escape '\"')"};
  for (const char *sql : queries) {
    ObArenaAllocator arena;
    ObParser parser(arena, 0);
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(sql), result)) << sql;
    const ParseNode *node = result.result_tree_->children_[0];
    ASSERT_EQ(T_SELECT, node->type_);
    const ParseNode *into = node->children_[PARSE_SELECT_INTO];
    ASSERT_NE(nullptr, into);
    EXPECT_EQ(T_INTO_OUTFILE, into->type_);
    EXPECT_EQ(1, into->value_);
    EXPECT_TRUE(into->is_tree_not_param_);
  }
}
