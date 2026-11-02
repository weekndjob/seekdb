#include <gtest/gtest.h>
#include "lib/allocator/ob_allocator.h"
#include "sql/parser/ob_parser.h"
using namespace oceanbase::common;
using namespace oceanbase::sql;
int count_ai(const ParseNode *node) {
  if (!node) { return 0; }
  int count=node->type_==T_AI_COLUMN_DEFINITION ? 1 : 0;
  for (int i=0;i<node->num_child_;++i) { count+=count_ai(node->children_[i]); }
  return count;
}
TEST(AiColumnParser, CreatesAndAltersWithoutChangingOrdinaryColumns) {
  for (const char *sql : {
      "CREATE TABLE docs(id BIGINT PRIMARY KEY,content VARCHAR(200),tags JSON AI COLUMN(AI_COMPLETE('text',CONCAT('Classify: ',content))) WITH(sync_mode='async',max_retries=2))",
      "CREATE TABLE docs(content VARCHAR(200),emb VECTOR(4) AI COLUMN(AI_EMBED('embed',content)))",
      "ALTER TABLE docs ADD COLUMN tags JSON AI COLUMN(AI_COMPLETE('text',content)) WITH(sync_mode='async',max_retries=1)",
      "ALTER TABLE docs MODIFY COLUMN tags JSON AI COLUMN(AI_COMPLETE('text',content))"}) {
    ObArenaAllocator arena; ObParser parser(arena,0);ParseResult parsed;
    ASSERT_EQ(OB_SUCCESS,parser.parse(ObString::make_string(sql),parsed)) << sql;
    EXPECT_EQ(1,count_ai(parsed.result_tree_));
  }
  for (const char *sql : {"CREATE TABLE ordinary(id INT PRIMARY KEY,content VARCHAR(20))", "CREATE TABLE ordinary(id INT,twice INT AS(id*2))", "SELECT AI_COMPLETE('text','hello')"}) {
    ObArenaAllocator arena; ObParser parser(arena,0);ParseResult parsed;
    ASSERT_EQ(OB_SUCCESS,parser.parse(ObString::make_string(sql),parsed)) << sql;
    EXPECT_EQ(0,count_ai(parsed.result_tree_));
  }
}

TEST(AiColumnParser, RetryUsesNativeUpdateAndPreservesParameterCount)
{
  for (const char *sql : {"ALTER TABLE docs RETRY AI COLUMN tags WHERE status='FAILED' AND id=1",
      "ALTER TABLE docs RETRY AI COLUMN tags WHERE `status`='active'",
      "ALTER TABLE docs RETRY AI COLUMN tags", "ALTER TABLE docs RETRY AI COLUMN tags WHERE id=1, ADD COLUMN extra INT"}) {
    ObArenaAllocator arena;
    ObParser parser(arena, 0);
    ParseResult parsed;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(sql), parsed)) << sql;
    const auto *statement = parsed.result_tree_->children_[0];
    ASSERT_EQ(T_UPDATE, statement->type_);
    ASSERT_EQ(12, statement->num_child_);
    ASSERT_NE(nullptr, statement->children_[11]);
    EXPECT_EQ(T_AI_COLUMN_RETRY, statement->children_[11]->type_);
    EXPECT_EQ(0, statement->value_);
  }
}
TEST(AiColumnParser, ShowsAiMetadataAndRetainsOrdinaryColumnFlags)
{
  for (const char *sql : {"SHOW AI COLUMNS FROM docs", "SHOW AI COLUMNS FROM db.docs"}) {
    ObArenaAllocator arena;
    ObParser parser(arena, 0);
    ParseResult parsed;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(sql), parsed)) << sql;
    const auto *statement = parsed.result_tree_->children_[0];
    ASSERT_EQ(T_SHOW_COLUMNS, statement->type_);
    ASSERT_EQ(4, statement->num_child_);
    EXPECT_EQ(4, statement->children_[0]->value_);
  }
}
