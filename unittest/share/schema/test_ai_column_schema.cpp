/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <string>
#include <cstring>
#include "lib/allocator/ob_allocator.h"
#include "share/schema/ai_column_definition.h"
#include "share/schema/ai_column_schema_builder.h"

using namespace oceanbase::common;
using namespace oceanbase::share::schema;
namespace {
void valid_rule(AiColumnDefinition &rule)
{
  rule.raw_expr_ = ObString::make_string("AI_COMPLETE('model',content)");
  rule.normalized_expr_ = rule.raw_expr_;
  rule.model_ = ObString::make_string("model");
  rule.input_expr_ = ObString::make_string("`content`");
  rule.result_type_ = ObString::make_string("JSON");
  rule.system_prompt_ = ObString::make_string("Return exactly one valid JSON value.");
  ASSERT_EQ(OB_SUCCESS, rule.dependencies_.push_back(16));
  ASSERT_EQ(OB_SUCCESS, rule.dependency_names_.push_back(ObString::make_string("content")));
}
void add_column(ObTableSchema &table, uint64_t id, const char *name)
{
  ObColumnSchemaV2 column;
  column.set_table_id(table.get_table_id());
  column.set_column_id(id);
  column.set_data_type(ObVarcharType);
  column.set_data_length(100);
  column.set_charset_type(CHARSET_UTF8MB4);
  column.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  ASSERT_EQ(OB_SUCCESS, column.set_column_name(name));
  ASSERT_EQ(OB_SUCCESS, table.add_column(column));
}
}
TEST(AiColumnSchema, PacketBoundsVersionsAndDependencyPairs)
{
  ObArenaAllocator arena;
  AiColumnDefinition rule;
  valid_rule(rule);
  ObString packet;
  EXPECT_NE(OB_SUCCESS, rule.encode(arena, packet));
  rule.definition_version_ = 1;
  rule.state_column_id_ = 19;
  rule.request_column_id_ = 20;
  rule.result_spec_column_id_ = 21;
  ASSERT_EQ(OB_SUCCESS, rule.encode(arena, packet));
  AiColumnDefinition restored;
  ASSERT_EQ(OB_SUCCESS, restored.decode(packet));
  EXPECT_EQ(rule.raw_expr_, restored.raw_expr_);
  EXPECT_EQ(rule.system_prompt_, restored.system_prompt_);
  ASSERT_EQ(1, restored.dependencies_.count());
  EXPECT_EQ(16, restored.dependencies_.at(0));
  EXPECT_EQ(ObString::make_string("content"), restored.dependency_names_.at(0));
  EXPECT_NE(OB_SUCCESS, restored.decode(ObString(packet.length()-1, packet.ptr())));
  std::string corrupt(packet.ptr(), packet.length());
  corrupt[0] = 2;
  EXPECT_EQ(OB_INVALID_DATA, restored.decode(ObString(corrupt.size(), corrupt.data())));
  rule.dependency_names_.reset();
  EXPECT_NE(OB_SUCCESS, rule.encode(arena, packet, false));
}
TEST(AiColumnSchema, ColumnOwnsPacketAndCopySurvivesSourceReuse)
{
  ObArenaAllocator arena;
  AiColumnDefinition rule;
  valid_rule(rule);
  ObString packet;
  ASSERT_EQ(OB_SUCCESS, rule.encode(arena, packet, false));
  ObColumnSchemaV2 source, copy;
  source.add_column_flag(AI_COLUMN_FLAG);
  ASSERT_EQ(OB_SUCCESS, source.set_ai_column_info(packet));
  ASSERT_EQ(OB_SUCCESS, copy.assign(source));
  source.reset();
  memset(packet.ptr(), 0, packet.length());
  AiColumnDefinition restored;
  ASSERT_EQ(OB_SUCCESS, restored.decode(copy.get_ai_column_info(), false));
  EXPECT_EQ(ObString::make_string("AI_COMPLETE('model',content)"), restored.raw_expr_);
  EXPECT_TRUE(copy.is_ai_column());
  EXPECT_EQ(0, static_cast<uint64_t>(AI_COLUMN_FLAG) >> 32);
}
TEST(AiColumnSchema, FinalIdsAndHiddenColumnsAreAllocatedAfterAlterRemapping)
{
  ObArenaAllocator arena;
  ObTableSchema table;
  table.set_table_id(500001);
  table.set_table_type(USER_TABLE);
  table.set_max_used_column_id(42);
  add_column(table, 42, "content");
  add_column(table, 43, "tags");
  ObColumnSchemaV2 *result = table.get_column_schema(43);
  ASSERT_NE(nullptr, result);
  result->add_column_flag(AI_COLUMN_FLAG);
  AiColumnDefinition rule;
  valid_rule(rule); // Resolver provisional dependency 16 must become physical 42.
  ObString packet;
  ASSERT_EQ(OB_SUCCESS, rule.encode(arena, packet, false));
  ASSERT_EQ(OB_SUCCESS, result->set_ai_column_info(packet));
  ObSEArray<ObColumnSchemaV2, 3> added;
  ASSERT_EQ(OB_SUCCESS, AiColumnSchemaBuilder::complete(table, *result, arena, &added));
  ASSERT_EQ(3, added.count());
  ASSERT_EQ(OB_SUCCESS, rule.decode(result->get_ai_column_info()));
  EXPECT_EQ(42, rule.dependencies_.at(0));
  EXPECT_EQ(44, rule.state_column_id_);
  EXPECT_EQ(45, rule.request_column_id_);
  EXPECT_EQ(46, rule.result_spec_column_id_);
  EXPECT_EQ(46, table.get_max_used_column_id());
  EXPECT_FALSE(result->is_hidden());
  for (int64_t i = 0; i < added.count(); ++i) {
    EXPECT_TRUE(added.at(i).is_hidden());
    EXPECT_TRUE(added.at(i).is_ai_column_aux());
    EXPECT_FALSE(added.at(i).is_ai_column());
  }
  EXPECT_EQ(ObString::make_string("PENDING"), added.at(0).get_orig_default_value().get_string());
  ASSERT_EQ(OB_SUCCESS, AiColumnSchemaBuilder::complete(table, *result, arena));
  EXPECT_EQ(5, table.get_column_count()); // Completion is idempotent.
}
