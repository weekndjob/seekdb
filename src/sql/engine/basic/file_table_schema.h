/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#pragma once
#include "sql/engine/basic/file_scan_definition.h"
#include "sql/engine/basic/file_jsonl_reader.h"
#include "sql/engine/basic/file_parquet_reader.h"

namespace oceanbase { namespace sql {
class FileTableSchema
{
public:
  FileTableSchema(FileScanDefinition &definition, common::ObIAllocator &allocator, int (*check)() = nullptr)
    : definition_(definition), allocator_(allocator), rows_(0), check_(check) {}
  int infer(CsvFileTable &file)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    if (!definition_.columns_.empty()) { return OB_INIT_TWICE; }
    if (OB_FAIL(file.identity(definition_.identity_))) {
    } else if (definition_.format_ == FileScanDefinition::CSV) {
      ret = infer_csv(file.stream());
    } else if (definition_.format_ == FileScanDefinition::JSONL) {
      ret = infer_jsonl(file.stream());
    } else if (definition_.format_ == FileScanDefinition::PARQUET) {
      FileParquetReader reader;
      if (OB_FAIL(reader.open(file, nullptr, true))) {
      } else {
        const auto columns = reader.columns();
        for (size_t i = 0; OB_SUCC(ret) && i < columns.size(); ++i) {
          const auto &column = columns[i];
          if (OB_FAIL(add_column(ObString(column.name.size(), column.name.data())))) { break; }
          definition_.types_.at(i) = FileParquetReader::type(column.kind);
          definition_.nullable_.at(i) = column.nullable;
          lengths_.at(i) = 16*1024*1024;
          scales_.at(i) = definition_.types_.at(i) == ObDateTimeType ? 6 : 0;
        }
      }
    } else { ret = OB_NOT_SUPPORTED; }
    if (ret == OB_SUCCESS && definition_.columns_.empty()) { ret = OB_INVALID_DATA; }
    for (int64_t i = 0; ret == OB_SUCCESS && i < definition_.types_.count(); ++i) {
      if (definition_.types_.at(i) == ObNullType) { definition_.nullable_.at(i) = 1; }
    }
    if (ret == OB_SUCCESS) { ret = file.check_unchanged(); }
    return ret;
  }
  int64_t length(int64_t i) const { return lengths_.at(i); }
  int64_t scale(int64_t i) const { return scales_.at(i); }
private:
  int add_column(const common::ObString &requested)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    ObSqlString base, candidate;
    if (definition_.columns_.count() >= 1024) { return OB_SIZE_OVERFLOW; }
    if (requested.length() > 1024) { return OB_SIZE_OVERFLOW; }
    if (!requested.empty() && std::memchr(requested.ptr(), 0, requested.length())) { return OB_INVALID_DATA; }
    if (requested.empty()) { ret = base.append_fmt("column%ld", definition_.columns_.count()+1); }
    else { ret = base.append(requested); }
    if (OB_SUCC(ret)) { ret = candidate.assign(base.string()); }
    int64_t suffix = 2;
    while (ret == OB_SUCCESS && find(candidate.string()) >= 0) {
      candidate.reuse();
      ret = candidate.append_fmt("%.*s__%ld", base.string().length(), base.string().ptr(), suffix++);
    }
    ObString name;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ob_write_string(allocator_, candidate.string(), name))) {
    } else if (OB_FAIL(definition_.columns_.push_back(name))) {
    } else if (OB_FAIL(definition_.types_.push_back(ObNullType))) {
    } else if (OB_FAIL(definition_.nullable_.push_back(rows_ > 0))) {
    } else if (OB_FAIL(lengths_.push_back(1))) {
    } else if (OB_FAIL(scales_.push_back(0))) {
    }
    return ret;
  }
  int64_t find(const common::ObString &name) const
  {
    for (int64_t i = 0; i < definition_.columns_.count(); ++i) {
      if (name.case_compare(definition_.columns_.at(i)) == 0) { return i; }
    }
    return -1;
  }
  void observe(int64_t column, const CsvField &value, common::ObObjType type)
  {
    using namespace common;
    definition_.types_.at(column) = FileValueConversion::widen(
        static_cast<ObObjType>(definition_.types_.at(column)), type);
    if (type == ObNullType) { definition_.nullable_.at(column) = 1; }
    lengths_.at(column) = std::max(lengths_.at(column), static_cast<int64_t>(value.value_.length()));
    if (type == ObDateTimeType) {
      const ObString time = value.value_.trim();
      if (time.length() > 20) { scales_.at(column) = std::max(scales_.at(column), static_cast<int64_t>(time.length()-20)); }
    }
  }
  int infer_csv(FILE *stream)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    ObArenaAllocator row;
    ObArray<CsvField> fields;
    CsvRecordReader reader(stream, row, 16*1024*1024, 64*1024*1024, 1024,
        definition_.delimiter_, definition_.quote_, definition_.escape_,
        definition_.header_ ? ObString() : definition_.null_text_);
    if (OB_FAIL(reader.next(fields))) { return ret == OB_ITER_END ? OB_INVALID_DATA : ret; }
    for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
      ret = add_column(definition_.header_ ? fields.at(i).value_ : ObString());
    }
    reader.set_null_text(definition_.null_text_);
    bool header = definition_.header_;
    while (OB_SUCC(ret)) {
      if (check_ && OB_FAIL(check_())) { break; }
      if (!header) {
        if (fields.count() != definition_.columns_.count()) {
          ret = OB_INVALID_DATA;
          OB_LOGGER.log_user_message(ObLogger::USER_ERROR, ret,
              "CSV record %ld at line %ld has %ld fields; expected %ld",
              reader.record(), reader.line(), fields.count(), definition_.columns_.count());
          break;
        }
        for (int64_t i = 0; i < fields.count(); ++i) { observe(i, fields.at(i), FileValueConversion::infer(fields.at(i))); }
        ++rows_;
      }
      header = false;
      row.reuse();
      if (OB_FAIL(reader.next(fields))) { break; }
    }
    return ret == OB_ITER_END ? OB_SUCCESS : ret;
  }
  int infer_jsonl(FILE *stream)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    ObArenaAllocator row;
    ObArray<JsonFileField> fields;
    JsonlRecordReader reader(stream, row);
    while (OB_SUCC(ret)) {
      if (check_ && OB_FAIL(check_())) { break; }
      row.reuse();
      if (OB_FAIL(reader.next(fields))) { break; }
      ObArray<int64_t> seen;
      for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
        const JsonFileField &field = fields.at(i);
        int64_t index = find(field.name_);
        if (index < 0) {
          if (OB_FAIL(add_column(field.name_))) { break; }
          index = definition_.columns_.count()-1;
        }
        observe(index, field.field_, field.type_);
        ret = seen.push_back(index);
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < definition_.columns_.count(); ++i) {
        bool present = false;
        for (int64_t j = 0; j < seen.count(); ++j) { if (seen.at(j) == i) { present = true; break; } }
        if (!present) { definition_.nullable_.at(i) = 1; }
      }
      ++rows_;
    }
    return ret == OB_ITER_END ? OB_SUCCESS : ret;
  }
  FileScanDefinition &definition_;
  common::ObIAllocator &allocator_;
  common::ObArray<int64_t> lengths_;
  common::ObArray<int64_t> scales_;
  int64_t rows_;
  int (*check_)();
};
} }
