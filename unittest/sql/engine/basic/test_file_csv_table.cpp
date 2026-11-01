#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "sql/engine/basic/file_csv_table.h"
using namespace oceanbase::common;
using oceanbase::sql::CsvFileTable;
#ifndef _WIN32
namespace {
class FileSandbox : public ::testing::Test {
protected:
  void SetUp() override
  {
    char path[] = "/tmp/seekdb-file-scan-XXXXXX";
    const char *created = ::mkdtemp(path);
    ASSERT_NE(nullptr, created);
    directory = created;
    std::filesystem::create_directory(directory / "allowed");
    std::ofstream(directory / "allowed/rows.csv") << "id\n1\n";
    std::ofstream(directory / "outside.csv") << "id\n2\n";
  }
  void TearDown() override { std::filesystem::remove_all(directory); }
  std::filesystem::path directory;
  int open(CsvFileTable &file, const std::filesystem::path &path)
  {
    std::string root = (directory / "allowed").string(), input = path.string();
    return file.open(ObString(root.size(), root.data()), ObString(input.size(), input.data()));
  }
};
}
TEST_F(FileSandbox, RejectsEscapeAndSpecialFiles)
{
  CsvFileTable file;
  ASSERT_EQ(OB_SUCCESS, open(file, directory / "allowed/rows.csv"));
  EXPECT_EQ(OB_SUCCESS, file.check_unchanged());
  EXPECT_NE(OB_SUCCESS, open(file, directory / "outside.csv"));
  std::filesystem::create_symlink(directory / "outside.csv", directory / "allowed/escape.csv");
  EXPECT_NE(OB_SUCCESS, open(file, directory / "allowed/escape.csv"));
  EXPECT_NE(OB_SUCCESS, open(file, directory / "allowed"));
  EXPECT_NE(OB_SUCCESS, file.open(ObString(), ObString::make_string("/etc/passwd")));
  EXPECT_NE(OB_SUCCESS, open(file, directory / "allowed/missing.csv"));
}
TEST_F(FileSandbox, DetectsMutationAndPathReplacement)
{
  CsvFileTable file;
  ASSERT_EQ(OB_SUCCESS, open(file, directory / "allowed/rows.csv"));
  std::ofstream(directory / "allowed/rows.csv", std::ios::app) << "2\n";
  EXPECT_NE(OB_SUCCESS, file.check_unchanged());
  ASSERT_EQ(OB_SUCCESS, open(file, directory / "allowed/rows.csv"));
  std::filesystem::rename(directory / "allowed/rows.csv", directory / "allowed/previous.csv");
  std::ofstream(directory / "allowed/rows.csv") << "id\n1\n2\n";
  EXPECT_NE(OB_SUCCESS, file.check_unchanged());
}
#endif
