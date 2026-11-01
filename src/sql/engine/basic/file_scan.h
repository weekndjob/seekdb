// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <memory>
#include <sys/stat.h>

namespace oceanbase { namespace sql { namespace file_sql {

enum Error { OK, END, INVALID, IO, CHANGED, DENIED, LIMIT, UNSUPPORTED };
enum Type { NULL_TYPE, BIGINT, DOUBLE, BOOLEAN, VARCHAR, DATE, DATETIME };
struct Cell { std::string text; bool is_null = true; };
struct Column { std::string name; Type type = NULL_TYPE; bool nullable = false; uint8_t scale = 0; };
struct Options {
  std::string format = "csv", path, root, nullstr;
  char delimiter = ',', quote = '"', escape = '"';
  bool header = true;
};
struct Definition {
  Options options;
  std::vector<Column> columns;
  struct stat identity {};
};
class ParquetScan;

// Owns one read-only descriptor and one row; inference rewinds the same file.
class FileScan {
public:
  FileScan();
  ~FileScan();
  FileScan(const FileScan &) = delete;
  FileScan &operator=(const FileScan &) = delete;
  Error infer(const Options &options, Definition &definition);
  Error open(const Definition &definition);
  Error next(std::vector<Cell> &row);
  Error rescan();
  Error project(const std::vector<int64_t> &columns);
  void close();
  const std::string &error() const { return error_; }
  static std::string encode(const Definition &definition);
  static Error decode(const std::string &bytes, Definition &definition);
  static bool is_definition(const std::string &bytes);
  static Type infer_type(const std::string &value);
  static Type widen(Type a, Type b);
private:
  Error open_file(const Options &options);
  Error check_identity();
  Error csv_row(std::vector<Cell> &row);
  Error json_row(std::vector<Cell> &row, bool inference);
  Error discover();
  Error fail(Error code, const std::string &message);
  FILE *file_ = nullptr;
  std::unique_ptr<ParquetScan> parquet_;
  Definition definition_;
  std::vector<std::string> paths_;
  size_t path_index_ = 0;
  uint64_t line_ = 1;
  uint64_t json_rows_ = 0;
  std::string error_;
};

class CsvWriter {
public:
  CsvWriter() = default;
  ~CsvWriter();
  CsvWriter(const CsvWriter &) = delete;
  CsvWriter &operator=(const CsvWriter &) = delete;
  Error open(const Options &options);
  Error write(const std::vector<Cell> &row);
  Error finish();
  void close();
private:
  Options options_;
  FILE *file_ = nullptr;
  int directory_ = -1;
  std::string temporary_, target_;
};

} } }
