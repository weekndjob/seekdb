#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "sql/engine/basic/file_copy_writer.h"
using namespace oceanbase::common;
using namespace oceanbase::sql;
namespace {
class CopySandbox : public ::testing::Test {
protected:
  void SetUp() override {
    char temporary[] = "/tmp/seekdb-copy-test-XXXXXX";
    ASSERT_NE(nullptr, ::mkdtemp(temporary));
    root = temporary;
    target = root+"/export.csv";
  }
  void TearDown() override { std::filesystem::remove_all(root); }
  int open(FileCopyWriter &writer) {
    return writer.open(ObString(root.size(), root.data()), ObString(target.size(), target.data()));
  }
  std::string root, target;
};
}
TEST_F(CopySandbox, CommitsQuotedCsvThatCanBeReadBack)
{
  FileCopyWriter writer;
  ASSERT_EQ(OB_SUCCESS, open(writer));
  FileScanDefinition options;
  options.null_text_ = ObString::make_string("NULL");
  ObArray<CsvField> fields;
  ASSERT_EQ(OB_SUCCESS, fields.push_back(CsvField{ObString::make_string("a,\"b\"\nline"), true}));
  ASSERT_EQ(OB_SUCCESS, fields.push_back(CsvField{ObString(), false}));
  ASSERT_EQ(OB_SUCCESS, fields.push_back(CsvField{ObString::make_string("NULL"), true}));
  ASSERT_EQ(OB_SUCCESS, fields.push_back(CsvField{ObString(), true}));
  ASSERT_EQ(OB_SUCCESS, writer.write(fields, options));
  ASSERT_FALSE(std::filesystem::exists(target));
  ASSERT_EQ(OB_SUCCESS, writer.commit());
  FILE *input = std::fopen(target.c_str(), "rb");
  ASSERT_NE(nullptr, input);
  ObArenaAllocator arena;
  CsvRecordReader reader(input, arena, 1024, 4096, 8, ',', '"', '"', options.null_text_);
  ASSERT_EQ(OB_SUCCESS, reader.next(fields));
  ASSERT_EQ(4, fields.count());
  EXPECT_EQ(0, fields.at(0).value_.compare("a,\"b\"\nline"));
  EXPECT_TRUE(fields.at(1).value_.empty());
  EXPECT_FALSE(fields.at(1).quoted_);
  EXPECT_EQ(0, fields.at(2).value_.compare("NULL"));
  EXPECT_TRUE(fields.at(2).quoted_);
  EXPECT_TRUE(fields.at(3).quoted_);
  std::fclose(input);
}
TEST_F(CopySandbox, AbortsWithoutLeavingTemporaryFiles)
{
  { FileCopyWriter writer; ASSERT_EQ(OB_SUCCESS, open(writer)); }
  EXPECT_TRUE(std::filesystem::is_empty(root));
}
TEST_F(CopySandbox, NeverOverwritesAnExistingOrConcurrentlyCreatedTarget)
{
  FileCopyWriter writer;
  ASSERT_EQ(OB_SUCCESS, open(writer));
  std::ofstream(target) << "winner";
  EXPECT_EQ(OB_FILE_ALREADY_EXIST, writer.commit());
  writer.abort();
  FileCopyWriter other;
  EXPECT_EQ(OB_FILE_ALREADY_EXIST, open(other));
  std::ifstream input(target);
  std::string content;
  input >> content;
  EXPECT_EQ("winner", content);
  EXPECT_EQ(1, std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator()));
}
