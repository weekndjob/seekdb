/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#pragma once
#include <charconv>
#include <chrono>
#include <limits>
#include "file_scan_bridge.h"
#include "common/object/ob_object.h"
#include "sql/engine/basic/file_scan_definition.h"

namespace oceanbase { namespace sql {
class FileParquetReader
{
  using ScanBox = rust::Box<file_sql::ParquetScan>;
public:
  FileParquetReader() = default;
  FileParquetReader(const FileParquetReader &) = delete;
  FileParquetReader &operator=(const FileParquetReader &) = delete;
  ~FileParquetReader() { close(); }
  void close() { if (scan_) { scan_->~ScanBox(); scan_ = nullptr; } selected_.reset(); }
  int open(CsvFileTable &file, const common::ObIArray<int64_t> *projection = nullptr, bool metadata_only = false)
  {
    using namespace common;
    close();
#ifdef _WIN32
    return OB_NOT_SUPPORTED;
#else
    if (!file.stream()) { return OB_NOT_INIT; }
    int ret = OB_SUCCESS;
    if (projection) {
      for (int64_t i = 0; OB_SUCC(ret) && i < projection->count(); ++i) {
        if (projection->at(i) < 0 || projection->at(i) >= 1024) { ret = OB_INVALID_ARGUMENT; }
        else { ret = selected_.push_back(static_cast<uint32_t>(projection->at(i))); }
      }
      if (OB_SUCC(ret) && selected_.empty()) { ret = selected_.push_back(0); }
    }
    if (ret != OB_SUCCESS) { return ret; }
    ScanBox box = file_sql::open_parquet(::fileno(file.stream()), rust::Slice<const uint32_t>(
          selected_.empty() ? nullptr : &selected_.at(0), selected_.count()), metadata_only);
    scan_ = new (storage_) ScanBox(std::move(box));
    if (!(*scan_)->error().empty()) { return OB_INVALID_DATA; }
    if (selected_.empty()) {
      for (size_t i = 0; OB_SUCC(ret) && i < (*scan_)->columns().size(); ++i) { ret = selected_.push_back(i); }
    }
    return ret;
#endif
  }
  rust::Slice<const file_sql::Column> columns() const { return (*scan_)->columns(); }
  static common::ObObjType type(file_sql::Kind kind)
  {
    using namespace common;
    switch (kind) {
      case file_sql::Kind::Integer: return ObIntType;
      case file_sql::Kind::Double: return ObDoubleType;
      case file_sql::Kind::Boolean: return ObTinyIntType;
      case file_sql::Kind::Date: return ObDateType;
      case file_sql::Kind::Datetime: return ObDateTimeType;
      case file_sql::Kind::Text: return ObVarcharType;
      default: return ObNullType;
    }
  }
  int next(common::ObIAllocator &arena, common::ObIArray<CsvField> &fields)
  {
    using namespace common;
    fields.reset();
    if (!scan_) { return OB_NOT_INIT; }
    const int status = (*scan_)->advance();
    if (status == 1) { return OB_ITER_END; }
    if (status != 0) { return OB_INVALID_DATA; }
    const auto row = (*scan_)->row();
    if (row.size() != static_cast<size_t>(selected_.count())) { return OB_INVALID_DATA; }
    int ret = fields.prepare_allocate(columns().size());
    if (ret != OB_SUCCESS) { return ret; }
    for (int64_t i = 0; i < fields.count(); ++i) { fields.at(i) = CsvField{ObString(), false}; }
    int64_t bytes = 0;
    for (int64_t i = 0; ret == OB_SUCCESS && i < selected_.count(); ++i) {
      const auto &value = row[i];
      if (value.kind == file_sql::Kind::Null) { continue; }
      char buffer[128] = {};
      int64_t length = 0;
      ObString text;
      if (value.kind == file_sql::Kind::Text) {
        text.assign_ptr(value.text.data(), value.text.size());
      } else if (value.kind == file_sql::Kind::Date || value.kind == file_sql::Kind::Datetime) {
        ret = format_time(value.integer, value.kind == file_sql::Kind::Date, buffer, sizeof(buffer), length);
        text.assign_ptr(buffer, length);
      } else if (value.kind == file_sql::Kind::Boolean) {
        text = ObString::make_string(value.integer ? "true" : "false");
      } else {
        const auto written = value.kind == file_sql::Kind::Double
            ? std::to_chars(buffer, buffer+sizeof(buffer), value.floating, std::chars_format::general,
                            std::numeric_limits<double>::max_digits10)
            : std::to_chars(buffer, buffer+sizeof(buffer), value.integer);
        if (written.ec != std::errc{}) { ret = OB_INVALID_DATA; }
        text.assign_ptr(buffer, written.ptr-buffer);
      }
      bytes += text.length();
      if (bytes > 64*1024*1024) { ret = OB_SIZE_OVERFLOW; }
      if (ret == OB_SUCCESS) {
        CsvField field{ObString(), true};
        if (OB_FAIL(ob_write_string(arena, text, field.value_))) {
        } else { fields.at(selected_.at(i)) = field; }
      }
    }
    return ret;
  }
private:
  static int format_time(int64_t value, bool date_only, char *buffer, int64_t capacity, int64_t &length)
  {
    using namespace std::chrono;
    if (date_only ? (value < -719162 || value > 2932896)
                  : (value < -62135596800000000LL || value > 253402300799999999LL)) {
      return common::OB_INVALID_DATA;
    }
    sys_days day;
    microseconds remaining{0};
    if (date_only) { day = sys_days{days{value}}; }
    else {
      const sys_time<microseconds> instant{microseconds{value}};
      day = floor<days>(instant);
      remaining = instant-day;
    }
    const year_month_day calendar{day};
    if (int(calendar.year()) < 1 || int(calendar.year()) > 9999 || !calendar.ok()) { return common::OB_INVALID_DATA; }
    const int hour = duration_cast<hours>(remaining).count(); remaining -= hours{hour};
    const int minute = duration_cast<minutes>(remaining).count(); remaining -= minutes{minute};
    const int second = duration_cast<seconds>(remaining).count(); remaining -= seconds{second};
    length = date_only
        ? std::snprintf(buffer, capacity, "%04d-%02u-%02u", int(calendar.year()), unsigned(calendar.month()), unsigned(calendar.day()))
        : std::snprintf(buffer, capacity, "%04d-%02u-%02u %02d:%02d:%02d.%06ld", int(calendar.year()),
              unsigned(calendar.month()), unsigned(calendar.day()), hour, minute, second, remaining.count());
    return length > 0 && length < capacity ? common::OB_SUCCESS : common::OB_SIZE_OVERFLOW;
  }
  ScanBox *scan_ = nullptr;
  alignas(ScanBox) char storage_[sizeof(ScanBox)];
  common::ObArray<uint32_t> selected_;
};
} }
