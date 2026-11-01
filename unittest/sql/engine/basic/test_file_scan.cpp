// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "sql/engine/basic/file_scan.h"
#include <gtest/gtest.h>
#include <fstream>
#include <unistd.h>

using namespace oceanbase::sql::file_sql;

class FileScanTest : public ::testing::Test {
protected:
  void SetUp() override {
    char path[] = "/tmp/seekdb-file-scan-XXXXXX";
    ASSERT_NE(nullptr, mkdtemp(path)); root = path;
  }
  void TearDown() override {
    for (const auto &path : files) unlink(path.c_str());
    rmdir(root.c_str());
  }
  std::string write(const std::string &name, const std::string &value) {
    std::string path = root + "/" + name;
    files.push_back(path);
    std::ofstream out(path, std::ios::binary); out << value; out.close();
    return path;
  }
  Options csv(const std::string &text) {
    Options options; options.root = root; options.path = write("input.csv", text); return options;
  }
  std::string root;
  std::vector<std::string> files;
};

TEST_F(FileScanTest, CsvTypesQuotesNullsAndRescan) {
  FileScan scan; Definition def;
  ASSERT_EQ(OK, scan.infer(csv("id,id,score,flag,day,created,note\r\n1,8,2,true,2024-02-29,2024-02-29T01:02:03,\"hello\r\nworld\"\r\n2,9,2.5,false,,,\"say \"\"hi\"\"\"\r\n"), def)) << scan.error();
  ASSERT_EQ(7U, def.columns.size());
  EXPECT_EQ("id__2", def.columns[1].name);
  EXPECT_EQ(BIGINT, def.columns[0].type); EXPECT_EQ(DOUBLE, def.columns[2].type);
  EXPECT_EQ(BOOLEAN, def.columns[3].type); EXPECT_EQ(DATE, def.columns[4].type);
  EXPECT_EQ(DATETIME, def.columns[5].type);
  EXPECT_FALSE(def.columns[0].nullable); EXPECT_TRUE(def.columns[4].nullable);
  std::vector<Cell> row;
  ASSERT_EQ(OK, scan.next(row)); EXPECT_EQ("hello\r\nworld", row[6].text);
  ASSERT_EQ(OK, scan.next(row)); EXPECT_TRUE(row[4].is_null); EXPECT_EQ("say \"hi\"", row[6].text);
  EXPECT_EQ(END, scan.next(row)); ASSERT_EQ(OK, scan.rescan()); EXPECT_EQ(OK, scan.next(row));
  Definition decoded; ASSERT_EQ(OK, FileScan::decode(FileScan::encode(def), decoded));
  FileScan other; ASSERT_EQ(OK, other.open(decoded)); EXPECT_EQ(OK, other.next(row));
  EXPECT_EQ(INVALID, FileScan::decode(FileScan::encode(def) + "x", decoded));
}

TEST_F(FileScanTest, DateTimeFractionalPrecisionSurvivesDescriptor) {
  FileScan scan; Definition def, decoded;
  ASSERT_EQ(OK, scan.infer(csv(R"(created,plain
2026-08-01 10:11:12.123456,2026-08-01 10:11:12
2026-08-01 10:11:12.5,2026-08-01 10:11:13
)"), def));
  ASSERT_EQ(2U, def.columns.size());
  EXPECT_EQ(DATETIME, def.columns[0].type); EXPECT_EQ(6, def.columns[0].scale);
  EXPECT_EQ(DATETIME, def.columns[1].type); EXPECT_EQ(0, def.columns[1].scale);
  ASSERT_EQ(OK, FileScan::decode(FileScan::encode(def), decoded));
  EXPECT_EQ(6, decoded.columns[0].scale); EXPECT_EQ(0, decoded.columns[1].scale);
}

TEST_F(FileScanTest, EmptyQuotedFieldAndNoHeader) {
  auto options = csv("1,\"\",\n2,x,\n"); options.header = false;
  FileScan scan; Definition def; ASSERT_EQ(OK, scan.infer(options, def));
  EXPECT_EQ("column0", def.columns[0].name); EXPECT_EQ(NULL_TYPE, def.columns[2].type);
  std::vector<Cell> row; ASSERT_EQ(OK, scan.next(row));
  EXPECT_FALSE(row[1].is_null); EXPECT_TRUE(row[1].text.empty()); EXPECT_TRUE(row[2].is_null);
}

TEST_F(FileScanTest, HeaderCollisionsAndResourceLimit) {
  FileScan scan; Definition def;
  ASSERT_EQ(OK, scan.infer(csv("A,a,a__2\n1,2,3\n"), def));
  EXPECT_EQ("A", def.columns[0].name); EXPECT_EQ("a__2", def.columns[1].name); EXPECT_EQ("a__2__2", def.columns[2].name);
  EXPECT_EQ(LIMIT, scan.infer(csv("a\n" + std::string(8 * 1024 * 1024 + 1, 'x') + "\n"), def));
}

TEST_F(FileScanTest, JsonUnionAndScalarTypes) {
  Options options; options.format = "json"; options.root = root;
  options.path = write("input.jsonl", "{\"id\":1,\"x\":2,\"s\":\"123\"}\n{\"id\":2,\"x\":2.5,\"flag\":true,\"unicode\":\"\\u4e2d\\u6587\"}\n");
  FileScan scan; Definition def; ASSERT_EQ(OK, scan.infer(options, def)) << scan.error();
  ASSERT_EQ(5U, def.columns.size()); EXPECT_EQ(DOUBLE, def.columns[1].type); EXPECT_EQ(VARCHAR, def.columns[2].type);
  std::vector<Cell> row; ASSERT_EQ(OK, scan.next(row)); EXPECT_TRUE(row[3].is_null);
  ASSERT_EQ(OK, scan.next(row)); EXPECT_TRUE(row[2].is_null); EXPECT_EQ("\xe4\xb8\xad\xe6\x96\x87", row[4].text);
  EXPECT_EQ(END, scan.next(row));
}

TEST_F(FileScanTest, RejectMalformedInput) {
  FileScan scan; Definition def;
  EXPECT_EQ(INVALID, scan.infer(csv("a,b\n1\n"), def));
  EXPECT_EQ(INVALID, scan.infer(csv("a\n\"unterminated"), def));
  EXPECT_EQ(INVALID, scan.infer(csv("a\n\"x\"junk\n"), def));
  EXPECT_EQ(INVALID, scan.infer(csv("a\n\xff\n"), def));
  Options options; options.root = root; options.format = "json";
  options.path = write("bad.jsonl", "{\"a\":{\"nested\":1}}\n");
  EXPECT_EQ(INVALID, scan.infer(options, def));
  options.path = write("duplicate.jsonl", "{\"a\":1,\"a\":2}\n");
  EXPECT_EQ(INVALID, scan.infer(options, def));
}

TEST_F(FileScanTest, PathBoundaryAndChangedFile) {
  auto options = csv("id\n1\n"); FileScan scan; Definition def;
  ASSERT_EQ(OK, scan.infer(options, def));
  { std::ofstream out(options.path, std::ios::app); out << "2\n"; }
  std::vector<Cell> row; EXPECT_EQ(CHANGED, scan.next(row));
  FileScan other; EXPECT_EQ(CHANGED, other.open(def));
  options.path = "/etc/passwd"; EXPECT_EQ(DENIED, scan.infer(options, def));
  options.path = root + "/escape.csv"; files.push_back(options.path);
  ASSERT_EQ(0, symlink("/etc/passwd", options.path.c_str()));
  EXPECT_EQ(DENIED, scan.infer(options, def));
  options.root.clear(); EXPECT_EQ(DENIED, scan.infer(options, def));
}

TEST_F(FileScanTest, GlobIsOneColumnAndReadOnly) {
  write("a.csv", "id\n1\n"); write("b.csv", "id\n2\n");
  Options options; options.root = root; options.path = root + "/*.csv"; options.format = "glob";
  FileScan scan; Definition def; ASSERT_EQ(OK, scan.infer(options, def));
  ASSERT_EQ(1U, def.columns.size()); EXPECT_EQ("file", def.columns[0].name);
  std::vector<Cell> row; ASSERT_EQ(OK, scan.next(row)); EXPECT_EQ(root + "/a.csv", row[0].text);
  ASSERT_EQ(OK, scan.next(row)); EXPECT_EQ(root + "/b.csv", row[0].text); EXPECT_EQ(END, scan.next(row));
}

TEST_F(FileScanTest, CsvExportRoundTripAndNoOverwrite) {
  Options options; options.root = root; options.path = root + "/export.csv"; options.nullstr = "NULL";
  files.push_back(options.path);
  CsvWriter writer; ASSERT_EQ(OK, writer.open(options));
  ASSERT_EQ(OK, writer.write({{"id",false},{"text",false}}));
  ASSERT_EQ(OK, writer.write({{"1",false},{"a,\"b\"\n",false}}));
  ASSERT_EQ(OK, writer.write({{"2",false},{"",true}}));
  ASSERT_EQ(OK, writer.write({{"3",false},{"NULL",false}}));
  ASSERT_NE(0, access(options.path.c_str(), F_OK));
  ASSERT_EQ(OK, writer.finish());
  FileScan scan; Definition def; ASSERT_EQ(OK, scan.infer(options, def));
  std::vector<Cell> row;
  ASSERT_EQ(OK, scan.next(row)); EXPECT_EQ("a,\"b\"\n", row[1].text);
  ASSERT_EQ(OK, scan.next(row)); EXPECT_TRUE(row[1].is_null);
  ASSERT_EQ(OK, scan.next(row)); EXPECT_FALSE(row[1].is_null); EXPECT_EQ("NULL", row[1].text);
  EXPECT_EQ(END, scan.next(row)); EXPECT_EQ(IO, writer.open(options));
}

TEST_F(FileScanTest, FailedExportRemovesOnlyItsTemporaryFile) {
  Options options; options.root = root; options.path = root + "/failed.csv";
  { CsvWriter writer; ASSERT_EQ(OK, writer.open(options)); ASSERT_EQ(OK, writer.write({{"partial",false}})); }
  EXPECT_NE(0, access(options.path.c_str(),F_OK));
  options.path = root + "/../outside.csv";
  CsvWriter writer; EXPECT_EQ(DENIED, writer.open(options));
}

TEST(FileScanTypes, BoundariesAndWidening) {
  EXPECT_EQ(BIGINT, FileScan::infer_type("-9223372036854775808"));
  EXPECT_EQ(VARCHAR, FileScan::infer_type("9223372036854775808"));
  EXPECT_EQ(VARCHAR, FileScan::infer_type("NaN")); EXPECT_EQ(VARCHAR, FileScan::infer_type("0x1p4"));
  EXPECT_EQ(VARCHAR, FileScan::infer_type("2023-02-29")); EXPECT_EQ(DATE, FileScan::infer_type("2000-02-29"));
  EXPECT_EQ(VARCHAR, FileScan::infer_type("2024-01-01 25:00:00"));
  EXPECT_EQ(DOUBLE, FileScan::widen(BIGINT, DOUBLE)); EXPECT_EQ(DATETIME, FileScan::widen(DATE, DATETIME));
  EXPECT_EQ(VARCHAR, FileScan::widen(BOOLEAN, BIGINT));
}

static std::string parquet_fixture(const char *name) {
  std::string source(__FILE__);
  std::ifstream input(source.substr(0, source.rfind('/')) + "/file_scan_data/" + name + ".parquet", std::ios::binary);
  if (!input) ADD_FAILURE() << "missing Parquet fixture " << name;
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

TEST_F(FileScanTest, OfficialParquetFixture) {
  Options options; options.root = root;
  options.path = write("official.parquet", parquet_fixture("official")); options.format = "parquet";
  FileScan scan; Definition def; ASSERT_EQ(OK, scan.infer(options, def)) << scan.error();
  ASSERT_EQ(1U, def.columns.size()); EXPECT_EQ("c1", def.columns[0].name); EXPECT_EQ(BIGINT, def.columns[0].type);
  std::vector<Cell> row;
  for (int i = 0; i < 7; ++i) { ASSERT_EQ(OK, scan.next(row)) << scan.error(); EXPECT_EQ(std::to_string(i), row[0].text); }
  EXPECT_EQ(END, scan.next(row));
}

TEST_F(FileScanTest, ParquetTypesRowGroupsAndProjection) {
  for (const char *fixture : {"plain_snappy_v1", "plain_uncompressed_v2", "dictionary_snappy_v2"}) {
    Options options; options.root = root;
    options.path = write(std::string(fixture) + ".parquet", parquet_fixture(fixture)); options.format = "parquet";
    FileScan scan; Definition def; ASSERT_EQ(OK, scan.infer(options, def)) << fixture << ": " << scan.error();
    ASSERT_EQ(7U, def.columns.size());
    const Type expected[] = {BIGINT, DOUBLE, BOOLEAN, VARCHAR, DATE, DATETIME, NULL_TYPE};
    for (size_t i = 0; i < 7; ++i) EXPECT_EQ(expected[i], def.columns[i].type);
    std::vector<Cell> row;
    ASSERT_EQ(OK, scan.next(row)) << scan.error();
    EXPECT_EQ("-9223372036854775808", row[0].text); EXPECT_EQ("1.5", row[1].text);
    EXPECT_EQ("true", row[2].text); EXPECT_EQ("\xe4\xb8\xad\xe6\x96\x87", row[3].text);
    EXPECT_EQ("2024-02-29", row[4].text); EXPECT_EQ("2024-02-29 01:02:03.123456", row[5].text); EXPECT_TRUE(row[6].is_null);
    ASSERT_EQ(OK, scan.next(row)) << scan.error(); EXPECT_EQ("0", row[0].text); EXPECT_TRUE(row[1].is_null); EXPECT_EQ("false", row[2].text); EXPECT_FALSE(row[3].is_null); EXPECT_TRUE(row[3].text.empty());
    ASSERT_EQ(OK, scan.next(row)) << scan.error(); EXPECT_EQ("9223372036854775807", row[0].text); EXPECT_EQ("-2.25", row[1].text); EXPECT_TRUE(row[2].is_null); EXPECT_TRUE(row[3].is_null);
    ASSERT_EQ(OK, scan.next(row)) << scan.error(); EXPECT_TRUE(row[0].is_null); EXPECT_EQ("a\"b\n", row[3].text); EXPECT_EQ("1969-12-31 23:59:59.999999", row[5].text);
    EXPECT_EQ(END, scan.next(row));
    ASSERT_EQ(OK, scan.project({0})); ASSERT_EQ(OK, scan.next(row)); EXPECT_EQ("-9223372036854775808", row[0].text); EXPECT_TRUE(row[3].is_null);
    ASSERT_EQ(OK, scan.project({}));
    int count = 0; while (scan.next(row) == OK) ++count;
    EXPECT_EQ(4, count);
  }
}
