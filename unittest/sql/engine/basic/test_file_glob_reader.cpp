#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <set>
#include "sql/engine/basic/file_glob_reader.h"
using namespace oceanbase::common;
using oceanbase::sql::FileGlobReader;
TEST(FileGlobReader, DiscoversOnlyAllowedFilesAndCanReopen)
{
  char temp[] = "/tmp/seekdb-glob-XXXXXX";
  ASSERT_NE(nullptr, ::mkdtemp(temp));
  const std::filesystem::path base(temp);
  std::filesystem::create_directory(base / "allowed");
  std::filesystem::create_directory(base / "allowed/dir");
  std::ofstream(base / "allowed/a.csv") << "x\n1\n";
  std::ofstream(base / "allowed/b.txt") << "text\n";
  std::ofstream(base / "outside.csv") << "secret\n";
  std::filesystem::create_symlink(base / "outside.csv", base / "allowed/escape.csv");
  const auto root = (base / "allowed").string();
  const auto pattern = (base / "allowed/*").string();
  FileGlobReader reader;
  ASSERT_EQ(OB_SUCCESS, reader.open(ObString(root.size(), root.data()), ObString(pattern.size(), pattern.data())));
  ObArenaAllocator row;
  ObString path;
  std::set<std::string> found;
  int ret = OB_SUCCESS;
  while ((ret = reader.next(row, path)) == OB_SUCCESS) {
    found.insert(std::filesystem::path(std::string(path.ptr(), path.length())).filename().string());
    row.reuse();
  }
  EXPECT_EQ(OB_ITER_END, ret);
  EXPECT_EQ((std::set<std::string>{"a.csv", "b.txt"}), found);
  const auto csv = (base / "allowed/*.csv").string();
  ASSERT_EQ(OB_SUCCESS, reader.open(ObString(root.size(), root.data()), ObString(csv.size(), csv.data())));
  EXPECT_EQ(OB_SUCCESS, reader.next(row, path));
  EXPECT_EQ(OB_ITER_END, reader.next(row, path));
  const auto outside = (base / "*").string();
  EXPECT_NE(OB_SUCCESS, reader.open(ObString(root.size(), root.data()), ObString(outside.size(), outside.data())));
  reader.close();
  std::filesystem::remove_all(base);
}
