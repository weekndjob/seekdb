/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#pragma once

#include <cstdio>
#include "lib/allocator/ob_allocator.h"
#include "lib/charset/ob_charset.h"
#include "lib/container/ob_iarray.h"
#include "lib/string/ob_sql_string.h"
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace sql
{

struct CsvField
{
  common::ObString value_;
  bool quoted_;
};

// Reads one logical record at a time. The caller owns the stream and must keep
// the supplied allocator alive until all returned fields have been consumed.
class CsvRecordReader
{
public:
  CsvRecordReader(FILE *stream, common::ObIAllocator &allocator,
                  int64_t max_field_bytes, int64_t max_record_bytes, int64_t max_columns)
    : stream_(stream), allocator_(allocator), max_field_bytes_(max_field_bytes),
      max_record_bytes_(max_record_bytes), max_columns_(max_columns), initialized_(false),
      pending_count_(0), pending_index_(0), pending_{} {}

  int next(common::ObIArray<CsvField> &fields)
  {
    using namespace common;
    enum State { START, UNQUOTED, QUOTED, AFTER_QUOTE };
    int ret = OB_SUCCESS;
    State state = START;
    ObSqlString field;
    bool was_quoted = false;
    bool saw_byte = false;
    bool done = false;
    int64_t bytes = 0;
    fields.reset();
    if (stream_ == nullptr || max_field_bytes_ < 1 || max_record_bytes_ < 1 || max_columns_ < 1) {
      ret = OB_INVALID_ARGUMENT;
    }
    while (OB_SUCC(ret) && !done) {
      const int ch = read_char();
      if (ch == EOF) {
        if (std::ferror(stream_)) {
          ret = OB_IO_ERROR;
        } else if (state == QUOTED) {
          ret = OB_INVALID_DATA;
        } else if (!saw_byte && fields.empty()) {
          ret = OB_ITER_END;
        } else if (OB_FAIL(append_field(field, was_quoted, fields))) {
        } else {
          done = true;
        }
      } else if (++bytes > max_record_bytes_) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        saw_byte = true;
        if (state == QUOTED) {
          if (ch == '"') {
            state = AFTER_QUOTE;
          } else if (OB_FAIL(append_char(field, ch))) {
          }
        } else if (state == AFTER_QUOTE && ch == '"') {
          state = QUOTED;
          if (OB_FAIL(append_char(field, ch))) {
          }
        } else if (ch == ',' || ch == '\n' || ch == '\r') {
          if (ch == '\r') {
            const int next = read_char();
            if (next == EOF && std::ferror(stream_)) {
              ret = OB_IO_ERROR;
            } else if (next != '\n') {
              ret = OB_INVALID_DATA;
            } else if (++bytes > max_record_bytes_) {
              ret = OB_SIZE_OVERFLOW;
            }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(append_field(field, was_quoted, fields))) {
          } else if (ch == ',') {
            state = START;
            was_quoted = false;
            field.reuse();
          } else {
            done = true;
          }
        } else if (state == START && ch == '"') {
          state = QUOTED;
          was_quoted = true;
        } else if (ch == '"' || state == AFTER_QUOTE) {
          ret = OB_INVALID_DATA;
        } else {
          state = UNQUOTED;
          if (OB_FAIL(append_char(field, ch))) {
          }
        }
      }
    }
    if (ret != OB_SUCCESS) {
      fields.reset();
    }
    return ret;
  }

private:
  int read_char()
  {
    if (!initialized_) {
      initialized_ = true;
      for (int i = 0; i < 3; ++i) {
        const int ch = std::fgetc(stream_);
        if (ch == EOF) {
          break;
        }
        pending_[pending_count_++] = static_cast<unsigned char>(ch);
      }
      if (pending_count_ == 3 && pending_[0] == 0xef && pending_[1] == 0xbb && pending_[2] == 0xbf) {
        pending_count_ = 0;
      }
    }
    return pending_index_ < pending_count_ ? pending_[pending_index_++] : std::fgetc(stream_);
  }

  int append_char(common::ObSqlString &field, int ch)
  {
    int ret = OB_SUCCESS;
    const char value = static_cast<char>(ch);
    if (field.length() >= max_field_bytes_) {
      ret = OB_SIZE_OVERFLOW;
    } else if (OB_FAIL(field.append(&value, 1))) {
    }
    return ret;
  }

  int append_field(const common::ObSqlString &field, bool quoted, common::ObIArray<CsvField> &fields)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    int64_t valid_bytes = 0;
    CsvField result{};
    const ObString value = field.string();
    if (fields.count() >= max_columns_) {
      ret = OB_SIZE_OVERFLOW;
    } else if (!value.empty() && OB_FAIL(ObCharset::well_formed_len(CS_TYPE_UTF8MB4_BIN,
                                                                  value.ptr(), value.length(), valid_bytes))) {
    } else if (!value.empty() && valid_bytes != value.length()) {
      ret = OB_INVALID_DATA;
    } else if (OB_FAIL(ob_write_string(allocator_, value, result.value_))) {
    } else {
      result.quoted_ = quoted;
      if (OB_FAIL(fields.push_back(result))) {
      }
    }
    return ret;
  }

  FILE *stream_;
  common::ObIAllocator &allocator_;
  int64_t max_field_bytes_;
  int64_t max_record_bytes_;
  int64_t max_columns_;
  bool initialized_;
  int pending_count_;
  int pending_index_;
  unsigned char pending_[3];
};

} // namespace sql
} // namespace oceanbase
