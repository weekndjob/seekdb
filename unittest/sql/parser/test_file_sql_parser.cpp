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
