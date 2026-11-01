#include <gtest/gtest.h>
#include <cstdio>
#include <string>
#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_array.h"
#include "sql/engine/basic/file_csv_reader.h"

using namespace oceanbase::common;
using oceanbase::sql::CsvField;
using oceanbase::sql::CsvRecordReader;

TEST(FileCsvReader, SupportsDelimiterEscapeAndExplicitNullText)
{
  FILE *file = std::tmpfile();
  ASSERT_NE(nullptr, file);
  const char input[] = "1;NULL;\"NULL\";\"a\\\"b\";\n";
  std::fwrite(input, 1, sizeof(input)-1, file);
  std::rewind(file);
  ObArenaAllocator arena;
  ObArray<CsvField> fields;
  CsvRecordReader reader(file, arena, 1024, 4096, 8, ';', '"', '\\', ObString::make_string("NULL"));
  ASSERT_EQ(OB_SUCCESS, reader.next(fields));
  ASSERT_EQ(5, fields.count());
  EXPECT_TRUE(fields.at(1).value_.empty());
  EXPECT_FALSE(fields.at(1).quoted_);
  EXPECT_EQ(0, fields.at(2).value_.compare("NULL"));
  EXPECT_TRUE(fields.at(2).quoted_);
  EXPECT_EQ(0, fields.at(3).value_.compare("a\"b"));
  EXPECT_TRUE(fields.at(4).value_.empty());
  EXPECT_TRUE(fields.at(4).quoted_);
  std::fclose(file);
}

namespace {
FILE *file_with(const std::string &bytes)
{
  FILE *file = std::tmpfile();
  if (file != nullptr) {
    if (std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
      std::fclose(file);
      return nullptr;
    }
    std::rewind(file);
  }
  return file;
}
std::string text(const CsvField &field)
{
  return field.value_.empty() ? std::string() : std::string(field.value_.ptr(), field.value_.length());
}
}

TEST(FileCsvReader, RecordsQuotesMultilineAndEmptyFields)
{
  FILE *file = file_with("id,name,note\r\n1,\"海洋\",\"a,\"\"b\"\"\nline\"\r\n2,,");
  ASSERT_NE(nullptr, file);
  ObArenaAllocator allocator;
  ObArray<CsvField> fields;
  CsvRecordReader reader(file, allocator, 1024, 4096, 8);
  ASSERT_EQ(OB_SUCCESS, reader.next(fields));
  ASSERT_EQ(3, fields.count());
  EXPECT_EQ("id", text(fields.at(0)));
  ASSERT_EQ(OB_SUCCESS, reader.next(fields));
  ASSERT_EQ(3, fields.count());
  EXPECT_EQ("海洋", text(fields.at(1)));
  EXPECT_TRUE(fields.at(1).quoted_);
  EXPECT_EQ("a,\"b\"\nline", text(fields.at(2)));
  ASSERT_EQ(OB_SUCCESS, reader.next(fields));
  ASSERT_EQ(3, fields.count());
  EXPECT_EQ("2", text(fields.at(0)));
  EXPECT_TRUE(text(fields.at(1)).empty());
  EXPECT_TRUE(text(fields.at(2)).empty());
  EXPECT_EQ(OB_ITER_END, reader.next(fields));
  std::fclose(file);
}

TEST(FileCsvReader, RejectsMalformedRecordsAndResourceOverflow)
{
  const std::string examples[] = {"\"unterminated", "ab\"cd\n", "\"ok\"x\n", "a\rb", std::string("a,\xff\n", 4)};
  for (const auto &example : examples) {
    FILE *file = file_with(example);
    ASSERT_NE(nullptr, file);
    ObArenaAllocator allocator;
    ObArray<CsvField> fields;
    CsvRecordReader reader(file, allocator, 64, 128, 4);
    EXPECT_NE(OB_SUCCESS, reader.next(fields));
    EXPECT_TRUE(fields.empty());
    std::fclose(file);
  }
  FILE *file = file_with("12345\n");
  ASSERT_NE(nullptr, file);
  ObArenaAllocator allocator;
  ObArray<CsvField> fields;
  CsvRecordReader reader(file, allocator, 4, 128, 4);
  EXPECT_EQ(OB_SIZE_OVERFLOW, reader.next(fields));
  EXPECT_TRUE(fields.empty());
  std::fclose(file);
}

TEST(FileCsvReader, PreservesQuotedEmptyAndEnforcesRecordAndColumnLimits)
{
  FILE *file = file_with(",\"\",z\n");
  ASSERT_NE(nullptr, file);
  ObArenaAllocator allocator;
  ObArray<CsvField> fields;
  CsvRecordReader reader(file, allocator, 8, 8, 3);
  ASSERT_EQ(OB_SUCCESS, reader.next(fields));
  ASSERT_EQ(3, fields.count());
  EXPECT_FALSE(fields.at(0).quoted_);
  EXPECT_TRUE(fields.at(1).quoted_);
  EXPECT_TRUE(text(fields.at(0)).empty());
  EXPECT_TRUE(text(fields.at(1)).empty());
  EXPECT_EQ("z", text(fields.at(2)));
  EXPECT_EQ(OB_ITER_END, reader.next(fields));
  std::fclose(file);

  file = file_with("1,2,3\n");
  ASSERT_NE(nullptr, file);
  CsvRecordReader column_limited(file, allocator, 8, 16, 2);
  EXPECT_EQ(OB_SIZE_OVERFLOW, column_limited.next(fields));
  EXPECT_TRUE(fields.empty());
  std::rewind(file);
  CsvRecordReader record_limited(file, allocator, 8, 4, 8);
  EXPECT_EQ(OB_SIZE_OVERFLOW, record_limited.next(fields));
  EXPECT_TRUE(fields.empty());
  std::fclose(file);
}

TEST(FileCsvReader, HandlesBomAndShortInputsWithoutLosingBytes)
{
  const std::string inputs[] = {std::string("\xef\xbb\xbf") + "id,name\n", "x", "xy", ""};
  const int counts[] = {2, 1, 1, 0};
  const char *first[] = {"id", "x", "xy", ""};
  for (int i = 0; i < 4; ++i) {
    FILE *file = file_with(inputs[i]);
    ASSERT_NE(nullptr, file);
    ObArenaAllocator allocator;
    ObArray<CsvField> fields;
    CsvRecordReader reader(file, allocator, 32, 64, 4);
    if (counts[i] == 0) {
      EXPECT_EQ(OB_ITER_END, reader.next(fields));
    } else {
      ASSERT_EQ(OB_SUCCESS, reader.next(fields));
      ASSERT_EQ(counts[i], fields.count());
      EXPECT_EQ(first[i], text(fields.at(0)));
      EXPECT_EQ(OB_ITER_END, reader.next(fields));
    }
    std::fclose(file);
  }
}
