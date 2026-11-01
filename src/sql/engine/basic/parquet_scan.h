// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once
#include "file_scan.h"
#include <memory>

namespace oceanbase { namespace sql { namespace file_sql {

class ParquetScan {
public:
  ParquetScan();
  ~ParquetScan();
  Error open(FILE *file, std::vector<Column> &schema);
  Error project(const std::vector<int64_t> &columns);
  Error rescan();
  Error next(std::vector<Cell> &row);
  const std::string &error() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} } }
