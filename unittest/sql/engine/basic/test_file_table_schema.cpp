#include <gtest/gtest.h>
#include "sql/engine/basic/file_table_schema.h"
using namespace oceanbase::common;
using namespace oceanbase::sql;
namespace {
int infer(const char *text, FileScanDefinition &definition, ObIAllocator &arena)
{
  char path[] = "/tmp/seekdb-schema-XXXXXX";
  const int named_fd = ::mkstemp(path);
  if (named_fd < 0) { return OB_IO_ERROR; }
  FILE *stream = ::fdopen(named_fd, "wb");
  if (!stream) { ::close(named_fd); ::unlink(path); return OB_IO_ERROR; }
  std::fwrite(text, 1, std::strlen(text), stream);
  std::fclose(stream);
  CsvFileTable file;
  int ret = file.open(ObString::make_string("/tmp"), ObString::make_string(path));
  if (ret == OB_SUCCESS) {
    FileTableSchema schema(definition, arena);
    ret = schema.infer(file);
  }
  ::unlink(path);
  return ret;
}
}
TEST(FileTableSchema, InfersEveryCsvRowAndPersistsMetadata)
{
  ObArenaAllocator arena;
  FileScanDefinition definition;
  ASSERT_EQ(OB_SUCCESS, infer("id,id,day,nil\n1,2,2024-02-29,\n3.5,4,2024-03-01T12:00:00,\n", definition, arena));
  ASSERT_EQ(4, definition.columns_.count());
  EXPECT_EQ(0, definition.columns_.at(1).compare("id__2"));
  EXPECT_EQ(ObDoubleType, definition.types_.at(0));
  EXPECT_EQ(ObDateTimeType, definition.types_.at(2));
  EXPECT_EQ(ObNullType, definition.types_.at(3));
  EXPECT_EQ(1, definition.nullable_.at(3));
  ObString encoded;
  ASSERT_EQ(OB_SUCCESS, definition.encode(arena, encoded));
  FileScanDefinition decoded;
  ASSERT_EQ(OB_SUCCESS, decoded.decode(encoded));
  EXPECT_EQ(definition.identity_.count(), decoded.identity_.count());
  EXPECT_EQ(0, decoded.columns_.at(1).compare("id__2"));
  EXPECT_EQ(ObDoubleType, decoded.types_.at(0));
}
TEST(FileTableSchema, InfersJsonUnionAndMissingValues)
{
  ObArenaAllocator arena;
  FileScanDefinition definition;
  definition.format_ = FileScanDefinition::JSONL;
  ASSERT_EQ(OB_SUCCESS, infer("{\"id\":1,\"name\":\"123\"}\n{\"id\":2.5,\"late\":true}\n", definition, arena));
  ASSERT_EQ(3, definition.columns_.count());
  EXPECT_EQ(ObDoubleType, definition.types_.at(0));
  EXPECT_EQ(ObVarcharType, definition.types_.at(1));
  EXPECT_EQ(ObTinyIntType, definition.types_.at(2));
  EXPECT_EQ(0, definition.nullable_.at(0));
  EXPECT_EQ(1, definition.nullable_.at(1));
  EXPECT_EQ(1, definition.nullable_.at(2));
}
TEST(FileTableSchema, SupportsHeaderlessCsvAndExplicitNullText)
{
  ObArenaAllocator arena;
  FileScanDefinition definition;
  definition.header_ = false;
  definition.delimiter_ = ';';
  definition.null_text_ = ObString::make_string("NULL");
  ASSERT_EQ(OB_SUCCESS, infer("1;NULL\n2;\n", definition, arena));
  ASSERT_EQ(2, definition.columns_.count());
  EXPECT_EQ(0, definition.columns_.at(0).compare("column1"));
  EXPECT_EQ(ObIntType, definition.types_.at(0));
  EXPECT_EQ(ObVarcharType, definition.types_.at(1));
  EXPECT_EQ(1, definition.nullable_.at(1));
}
