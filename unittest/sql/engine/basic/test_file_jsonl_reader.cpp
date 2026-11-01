#include <gtest/gtest.h>
#include "sql/engine/basic/file_jsonl_reader.h"
using namespace oceanbase::common;
using namespace oceanbase::sql;
namespace {
struct JsonInput {
  explicit JsonInput(const char *text) : file(std::tmpfile())
  { if (file) { std::fwrite(text, 1, std::strlen(text), file); std::rewind(file); } }
  ~JsonInput() { if (file) { std::fclose(file); } }
  FILE *file;
};
}
TEST(FileJsonlReader, ReadsScalarsWithExactNullAndStringSemantics)
{
  JsonInput input("{\"id\":1,\"name\":\"123\",\"nil\":null,\"active\":true,\"text\":\"a\\nb\"}\r\n{\"id\":2}\n");
  ASSERT_NE(nullptr, input.file);
  ObArenaAllocator arena;
  ObArray<JsonFileField> fields;
  JsonlRecordReader reader(input.file, arena);
  ASSERT_EQ(OB_SUCCESS, reader.next(fields));
  ASSERT_EQ(5, fields.count());
  EXPECT_EQ(ObIntType, fields.at(0).type_);
  EXPECT_EQ(ObVarcharType, fields.at(1).type_);
  EXPECT_EQ(ObNullType, fields.at(2).type_);
  EXPECT_EQ(ObTinyIntType, fields.at(3).type_);
  EXPECT_EQ(0, fields.at(4).field_.value_.compare("a\nb"));
  arena.reuse();
  ASSERT_EQ(OB_SUCCESS, reader.next(fields));
  EXPECT_EQ(1, fields.count());
  EXPECT_EQ(2, reader.line());
  EXPECT_EQ(OB_ITER_END, reader.next(fields));
}
TEST(FileJsonlReader, RejectsNestedAmbiguousAndMalformedRecords)
{
  const char *invalid[] = {"[]\n", "{\"x\":{}}\n", "{\"x\":[]}\n", "{\"x\":1,\"X\":2}\n",
                           "{\"x\":}\n", "{\"x\":1} trailing\n", "\"string\"\n", "\n"};
  for (const char *text : invalid) {
    JsonInput input(text);
    ASSERT_NE(nullptr, input.file);
    ObArenaAllocator arena;
    ObArray<JsonFileField> fields;
    JsonlRecordReader reader(input.file, arena);
    EXPECT_NE(OB_SUCCESS, reader.next(fields)) << text;
    EXPECT_EQ(0, fields.count());
  }
}
TEST(FileJsonlReader, EnforcesBoundsAndReadsBom)
{
  JsonInput input("\xef\xbb\xbf{\"x\":1}\n{\"a\":1,\"b\":2}\n");
  ObArenaAllocator arena;
  ObArray<JsonFileField> fields;
  JsonlRecordReader reader(input.file, arena, 32, 1);
  ASSERT_EQ(OB_SUCCESS, reader.next(fields));
  EXPECT_EQ(ObIntType, fields.at(0).type_);
  EXPECT_NE(OB_SUCCESS, reader.next(fields));
  JsonInput long_line("{\"text\":\"12345\"}\n");
  JsonlRecordReader bounded(long_line.file, arena, 8);
  EXPECT_EQ(OB_SIZE_OVERFLOW, bounded.next(fields));
}
