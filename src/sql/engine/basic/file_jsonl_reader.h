/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#pragma once
#include <rapidjson/reader.h>
#include <rapidjson/memorystream.h>
#include "sql/engine/basic/file_value_conversion.h"
#include "lib/container/ob_array.h"

namespace oceanbase { namespace sql {
struct JsonFileField
{
  common::ObString name_;
  CsvField field_;
  common::ObObjType type_;
};

// Only one bounded JSONL record is retained. Nested values are rejected by the
// SAX handler before they can allocate a nested document tree.
class JsonlRecordReader
{
public:
  JsonlRecordReader(FILE *stream, common::ObIAllocator &allocator,
                    int64_t max_record_bytes = 64 * 1024 * 1024, int64_t max_fields = 1024)
    : stream_(stream), allocator_(allocator), max_record_bytes_(max_record_bytes),
      max_fields_(max_fields), line_(0) {}
  int64_t line() const { return line_; }
  int next(common::ObIArray<JsonFileField> &fields)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    fields.reset();
    ObSqlString record;
    if (stream_ == nullptr || max_record_bytes_ < 1 || max_fields_ < 1) { return OB_INVALID_ARGUMENT; }
    for (;;) {
      const int ch = std::fgetc(stream_);
      if (ch == EOF) {
        if (std::ferror(stream_)) { return OB_IO_ERROR; }
        if (record.empty()) { return OB_ITER_END; }
        break;
      }
      if (ch == '\n') { break; }
      if (record.length() >= max_record_bytes_) { return OB_SIZE_OVERFLOW; }
      const char byte = static_cast<char>(ch);
      if (OB_FAIL(record.append(&byte, 1))) { return ret; }
    }
    ObString text = record.string();
    if (++line_ == 1 && text.length() >= 3
        && std::memcmp(text.ptr(), "\xef\xbb\xbf", 3) == 0) {
      text.assign_ptr(text.ptr()+3, text.length()-3);
    }
    Handler handler(allocator_, fields, max_fields_);
    rapidjson::MemoryStream input(text.ptr(), text.length());
    rapidjson::Reader parser;
    const bool parsed = parser.Parse<rapidjson::kParseValidateEncodingFlag
                                     | rapidjson::kParseNumbersAsStringsFlag>(input, handler);
    ret = handler.error_;
    if (ret == OB_SUCCESS && (!parsed || !handler.complete_)) { ret = OB_INVALID_DATA; }
    if (ret != OB_SUCCESS) { fields.reset(); }
    return ret;
  }
private:
  struct Handler : rapidjson::BaseReaderHandler<rapidjson::UTF8<>, Handler>
  {
    Handler(common::ObIAllocator &allocator, common::ObIArray<JsonFileField> &fields, int64_t limit)
      : allocator_(allocator), fields_(fields), limit_(limit) {}
    bool StartObject() { if (started_) { return false; } started_ = true; return true; }
    bool EndObject(rapidjson::SizeType) { complete_ = started_ && !has_key_; return complete_; }
    bool StartArray() { return false; }
    bool Key(const char *text, rapidjson::SizeType length, bool)
    {
      using namespace common;
      if (!started_ || complete_ || has_key_ || fields_.count() >= limit_) { return false; }
      const ObString name(length, text);
      for (int64_t i = 0; i < fields_.count(); ++i) {
        if (name.case_compare(fields_.at(i).name_) == 0) { return false; }
      }
      error_ = ob_write_string(allocator_, name, key_);
      has_key_ = error_ == OB_SUCCESS;
      return has_key_;
    }
    bool Null() { return append(common::ObString(), false, common::ObNullType); }
    bool Bool(bool value)
    { return append(common::ObString::make_string(value ? "true" : "false"), true, common::ObTinyIntType); }
    bool RawNumber(const char *text, rapidjson::SizeType length, bool)
    {
      const CsvField field{common::ObString(length, text), true};
      return append(field.value_, true, FileValueConversion::infer(field));
    }
    bool String(const char *text, rapidjson::SizeType length, bool)
    {
      const CsvField field{common::ObString(length, text), true};
      common::ObObjType type = FileValueConversion::infer(field);
      if (type != common::ObDateType && type != common::ObDateTimeType) { type = common::ObVarcharType; }
      return append(field.value_, true, type);
    }
    bool append(const common::ObString &text, bool quoted, common::ObObjType type)
    {
      using namespace common;
      if (!has_key_ || complete_) { return false; }
      JsonFileField field{key_, CsvField{ObString(), quoted}, type};
      error_ = ob_write_string(allocator_, text, field.field_.value_);
      if (error_ == OB_SUCCESS) { error_ = fields_.push_back(field); }
      has_key_ = false;
      return error_ == OB_SUCCESS;
    }
    common::ObIAllocator &allocator_;
    common::ObIArray<JsonFileField> &fields_;
    int64_t limit_;
    common::ObString key_;
    int error_ = common::OB_SUCCESS;
    bool started_ = false;
    bool complete_ = false;
    bool has_key_ = false;
  };
  FILE *stream_;
  common::ObIAllocator &allocator_;
  int64_t max_record_bytes_;
  int64_t max_fields_;
  int64_t line_;
};
} }
