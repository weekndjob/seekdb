// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "parquet_scan.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace oceanbase { namespace sql { namespace file_sql {
namespace {
constexpr size_t MAX_PAGE_BYTES = 8 * 1024 * 1024;
constexpr size_t MAX_FOOTER_BYTES = 16 * 1024 * 1024;
constexpr size_t MAX_PAGE_VALUES = 1024 * 1024;
constexpr size_t MAX_SCAN_BYTES = 64 * 1024 * 1024;

uint64_t little(const char *p, size_t n) {
  uint64_t value = 0;
  for (size_t i = 0; i < n; ++i) value |= uint64_t(static_cast<unsigned char>(p[i])) << (i * 8);
  return value;
}

struct CompactNode {
  int id = 0, type = 0;
  int64_t number = 0;
  std::string bytes;
  std::vector<CompactNode> children;
  const CompactNode *get(int key) const {
    for (const auto &child : children) if (child.id == key) return &child;
    return nullptr;
  }
  int64_t integer(int key, int64_t fallback = -1) const {
    const auto *node = get(key); return node && node->type >= 1 && node->type <= 6 ? node->number : fallback;
  }
};

class CompactReader {
public:
  explicit CompactReader(const std::string &data) : data_(data) {}
  bool structure(CompactNode &node, unsigned depth = 0) {
    if (depth > 32) return false;
    int id = 0;
    while (pos_ < data_.size()) {
      unsigned char header = static_cast<unsigned char>(data_[pos_++]);
      if (!header) return true;
      int64_t field = 0;
      if (header >> 4) id += header >> 4;
      else { if (!signed_number(field) || field < 1 || field > 32767) return false; id = field; }
      CompactNode child; child.id = id;
      if (!value(header & 15, child, depth + 1)) return false;
      node.children.push_back(std::move(child));
      if (++nodes_ > 100000) return false;
    }
    return false;
  }
  size_t position() const { return pos_; }
private:
  bool unsigned_number(uint64_t &value) {
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
      if (pos_ == data_.size()) return false;
      unsigned char byte = static_cast<unsigned char>(data_[pos_++]);
      if (shift == 63 && byte > 1) return false;
      value |= uint64_t(byte & 127) << shift;
      if (!(byte & 128)) return true;
    }
    return false;
  }
  bool signed_number(int64_t &value) {
    uint64_t raw;
    if (!unsigned_number(raw)) return false;
    value = static_cast<int64_t>((raw >> 1) ^ (0 - (raw & 1)));
    return true;
  }
  bool value(unsigned type, CompactNode &node, unsigned depth) {
    if (depth > 32) return false;
    node.type = type;
    uint64_t count = 0;
    switch (type) {
      case 1: node.number = 1; return true;
      case 2: node.number = 0; return true;
      case 3:
        if (pos_ == data_.size()) return false;
        node.number = static_cast<signed char>(data_[pos_++]); return true;
      case 4: case 5: case 6: return signed_number(node.number);
      case 7:
        if (data_.size() - pos_ < 8) return false;
        pos_ += 8; return true;
      case 8:
        if (!unsigned_number(count) || count > data_.size() - pos_ || count > MAX_FOOTER_BYTES) return false;
        node.bytes.assign(data_, pos_, count); pos_ += count; return true;
      case 9: case 10: {
        if (pos_ == data_.size()) return false;
        const unsigned char header = static_cast<unsigned char>(data_[pos_++]);
        count = header >> 4;
        if (count == 15 && !unsigned_number(count)) return false;
        if (count > 100000 || count > data_.size() - pos_) return false;
        for (uint64_t i = 0; i < count; ++i) {
          CompactNode child;
          if (++nodes_ > 100000 || !value(header & 15, child, depth + 1)) return false;
          node.children.push_back(std::move(child));
        }
        return true;
      }
      case 11: {
        if (!unsigned_number(count) || count > 100000) return false;
        if (!count) return true;
        if (pos_ == data_.size()) return false;
        const unsigned char types = static_cast<unsigned char>(data_[pos_++]);
        for (uint64_t i = 0; i < count; ++i) {
          CompactNode key, val;
          if (!value(types >> 4,key,depth+1) || !value(types & 15,val,depth+1)) return false;
        }
        return true;
      }
      case 12: return structure(node, depth + 1);
      default: return false;
    }
  }
  const std::string &data_;
  size_t pos_ = 0, nodes_ = 0;
};

bool read_at(FILE *file, uint64_t offset, size_t size, uint64_t file_size, std::string &out) {
  if (offset > file_size || size > file_size - offset || size > MAX_FOOTER_BYTES) return false;
  if (fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
  out.resize(size);
  return size == 0 || fread(&out[0], 1, size, file) == size;
}

bool varint(const std::string &data, size_t &pos, uint64_t &result) {
  result = 0;
  for (unsigned shift = 0; shift < 64; shift += 7) {
    if (pos == data.size()) return false;
    unsigned char byte = static_cast<unsigned char>(data[pos++]);
    if (shift == 63 && byte > 1) return false;
    result |= uint64_t(byte & 127) << shift;
    if (!(byte & 128)) return true;
  }
  return false;
}

bool snappy(const std::string &in, size_t expected, std::string &out) {
  size_t pos = 0; uint64_t length = 0;
  if (!varint(in,pos,length) || length != expected || length > MAX_PAGE_BYTES) return false;
  out.clear(); out.reserve(expected);
  while (pos < in.size() && out.size() < expected) {
    unsigned char tag = static_cast<unsigned char>(in[pos++]);
    const unsigned type = tag & 3;
    size_t count, offset = 0;
    if (type == 0) {
      count = tag >> 2;
      if (count < 60) ++count;
      else {
        const size_t n = count - 59;
        if (n > 4 || n > in.size() - pos) return false;
        count = little(in.data() + pos, n) + 1; pos += n;
      }
      if (count > in.size() - pos || count > expected - out.size()) return false;
      out.append(in, pos, count); pos += count;
    } else {
      if (type == 1) {
        if (pos == in.size()) return false;
        count = 4 + ((tag >> 2) & 7);
        offset = ((tag & 0xe0) << 3) | static_cast<unsigned char>(in[pos++]);
      } else {
        const size_t n = type == 2 ? 2 : 4;
        if (n > in.size() - pos) return false;
        count = (tag >> 2) + 1; offset = little(in.data()+pos,n); pos += n;
      }
      if (!offset || offset > out.size() || count > expected - out.size()) return false;
      for (size_t i = 0; i < count; ++i) out.push_back(out[out.size()-offset]);
    }
  }
  return pos == in.size() && out.size() == expected;
}

bool decompress(int codec, const std::string &in, size_t expected, std::string &out) {
  if (expected > MAX_PAGE_BYTES) return false;
  if (codec == 0) { if (in.size() != expected) return false; out = in; return true; }
  return codec == 1 && snappy(in,expected,out);
}

bool hybrid(const std::string &data, size_t &pos, size_t end, unsigned bits, size_t count, std::vector<uint32_t> &values) {
  if (bits > 32 || count > MAX_PAGE_VALUES || end > data.size() || pos > end) return false;
  values.clear(); values.reserve(count);
  while (values.size() < count) {
    uint64_t header;
    if (!varint(data,pos,header) || pos > end || header == 0) return false;
    uint64_t n = header >> 1;
    if (header & 1) {
      if ((bits && n > (end-pos) / bits) || n > (MAX_PAGE_VALUES+7)/8) return false;
      const size_t bytes = static_cast<size_t>(n) * bits;
      if (bytes > end-pos) return false;
      const size_t run = static_cast<size_t>(n) * 8;
      for (size_t i = 0; i < run && values.size() < count; ++i) {
        uint32_t value = 0;
        for (unsigned b = 0; b < bits; ++b) value |= ((static_cast<unsigned char>(data[pos+(i*bits+b)/8]) >> ((i*bits+b)%8)) & 1U) << b;
        values.push_back(value);
      }
      pos += bytes;
    } else {
      const size_t bytes = (bits+7)/8;
      if (n > count-values.size() || bytes > end-pos) return false;
      uint32_t value = static_cast<uint32_t>(little(data.data()+pos,bytes)); pos += bytes;
      if (bits < 32 && value >= (uint64_t(1) << bits)) return false;
      values.insert(values.end(), static_cast<size_t>(n), value);
    }
  }
  return true;
}

std::string civil_date(int64_t days) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days-146096) / 146097;
  const unsigned doe = static_cast<unsigned>(days - era*146097);
  const unsigned yoe = (doe-doe/1460+doe/36524-doe/146096)/365;
  int64_t year = yoe + era*400;
  const unsigned doy = doe-(365*yoe+yoe/4-yoe/100), mp = (5*doy+2)/153;
  const unsigned day = doy-(153*mp+2)/5+1, month = mp < 10 ? mp+3 : mp-9;
  year += month <= 2;
  if (year < 1 || year > 9999) return {};
  char text[32]; std::snprintf(text,sizeof(text),"%04d-%02u-%02u",static_cast<int>(year),month,day); return text;
}

struct PhysicalColumn { int type = -1, converted = -1; bool optional = false, null_only = false; };
struct Chunk { uint64_t offset = 0, end = 0; int codec = 0; uint64_t values = 0; };
struct RowGroup { uint64_t rows = 0; std::vector<Chunk> chunks; };
struct Cursor {
  Chunk chunk;
  uint64_t offset = 0, seen = 0;
  size_t row = 0, count = 0, data_pos = 0, boolean_bit = 0;
  int encoding = 0;
  std::string data;
  std::vector<uint32_t> definitions, indices;
  std::vector<Cell> dictionary;
};

bool plain(const PhysicalColumn &column, const std::string &data, size_t &pos, size_t &bit, Cell &cell) {
  cell.is_null = false; cell.text.clear();
  const int type = column.type;
  if (pos > data.size()) return false;
  if (type == 0) {
    if (pos == data.size()) return false;
    cell.text = ((static_cast<unsigned char>(data[pos]) >> bit) & 1) ? "true" : "false";
    if (++bit == 8) { bit = 0; ++pos; } return true;
  }
  const size_t n = type == 1 || type == 4 ? 4 : type == 2 || type == 5 ? 8 : 0;
  if (n) {
    if (n > data.size()-pos) return false;
    uint64_t raw = little(data.data()+pos,n); pos += n;
    if (type == 1 || type == 2) {
      const int64_t value = type == 1 ? static_cast<int32_t>(raw) : static_cast<int64_t>(raw);
      if (column.converted == 6) { cell.text = civil_date(value); return !cell.text.empty(); }
      if (column.converted == 9 || column.converted == 10) {
        const int64_t units = column.converted == 9 ? 1000 : 1000000;
        int64_t seconds = value / units, fraction = value % units;
        if (fraction < 0) { fraction += units; --seconds; }
        int64_t days = seconds / 86400, clock = seconds % 86400;
        if (clock < 0) { clock += 86400; --days; }
        const std::string day = civil_date(days);
        if (day.empty()) return false;
        char text[48]; std::snprintf(text,sizeof(text),"%s %02d:%02d:%02d.%06d",day.c_str(),int(clock/3600),int(clock/60%60),int(clock%60),int(fraction*(1000000/units)));
        cell.text = text;
      } else cell.text = std::to_string(value);
    } else {
      double value;
      if (type == 4) { uint32_t bits = raw; float f; std::memcpy(&f,&bits,4); value = f; }
      else std::memcpy(&value,&raw,8);
      if (!std::isfinite(value)) return false;
      char text[32]; std::snprintf(text,sizeof(text),"%.17g",value); cell.text = text;
    }
    return true;
  }
  if (type == 6) {
    if (data.size()-pos < 4) return false;
    const size_t length = little(data.data()+pos,4); pos += 4;
    if (length > data.size()-pos) return false;
    cell.text.assign(data,pos,length); pos += length; return true;
  }
  return false;
}
}

struct ParquetScan::Impl {
  FILE *file = nullptr;
  uint64_t size = 0, footer = 0, row = 0;
  size_t group = 0;
  std::string error;
  std::vector<Column> schema;
  std::vector<PhysicalColumn> physical;
  std::vector<RowGroup> groups;
  std::vector<int64_t> selected;
  std::vector<Cursor> cursors;
  Error fail(Error code, const char *message) { error = message; return code; }
  Error metadata();
  Error start_group();
  Error page(size_t index);
  size_t memory() const {
    size_t total = 0;
    for (const auto &cursor : cursors) {
      total += cursor.data.size() + (cursor.definitions.size()+cursor.indices.size())*sizeof(uint32_t);
      total += cursor.dictionary.size()*sizeof(Cell);
      for (const auto &cell : cursor.dictionary) total += cell.text.size();
    }
    return total;
  }
};

ParquetScan::ParquetScan() : impl_(new Impl()) {}
ParquetScan::~ParquetScan() = default;
const std::string &ParquetScan::error() const { return impl_->error; }

Error ParquetScan::Impl::metadata() {
  if (fseeko(file,0,SEEK_END) != 0) return fail(IO,"cannot seek Parquet footer");
  const off_t length = ftello(file);
  if (length < 12) return fail(INVALID,"truncated Parquet file");
  size = static_cast<uint64_t>(length);
  std::string bytes;
  if (!read_at(file,0,4,size,bytes) || bytes != "PAR1" || !read_at(file,size-8,8,size,bytes) || bytes.substr(4) != "PAR1") return fail(INVALID,"invalid Parquet magic");
  const uint64_t footer_size = little(bytes.data(),4);
  if (footer_size > MAX_FOOTER_BYTES || footer_size > size-12) return fail(LIMIT,"invalid Parquet footer size");
  footer = size-8-footer_size;
  if (!read_at(file,footer,footer_size,size,bytes)) return fail(IO,"cannot read Parquet footer");
  CompactNode metadata;
  CompactReader reader(bytes);
  if (!reader.structure(metadata) || reader.position() != bytes.size()) return fail(INVALID,"invalid compact Parquet metadata");
  const auto *fields = metadata.get(2), *row_groups = metadata.get(4);
  if (!fields || fields->children.size() < 2 || fields->children.size() > 1025 || !row_groups || row_groups->children.size() > 10000) return fail(INVALID,"unsupported Parquet schema");
  if (fields->children[0].integer(5) != static_cast<int64_t>(fields->children.size()-1)) return fail(UNSUPPORTED,"nested Parquet schema is unsupported");
  schema.clear(); physical.clear(); groups.clear();
  for (size_t i = 1; i < fields->children.size(); ++i) {
    const auto &field = fields->children[i];
    const auto *name = field.get(4);
    PhysicalColumn column;
    column.type = field.integer(1); column.converted = field.integer(6);
    if (const auto *logical = field.get(10)) {
      if (logical->children.size() != 1) return fail(INVALID,"invalid Parquet logical type");
      const auto &annotation = logical->children[0];
      if (annotation.id == 11) column.null_only = true;
      else if (annotation.id == 6 && column.type == 1) column.converted = 6;
      else if (annotation.id == 1 && column.type == 6) column.converted = 0;
      else if (annotation.id == 8 && column.type == 2) {
        const auto *unit = annotation.get(2);
        if (!unit || unit->children.size() != 1 || (unit->children[0].id != 1 && unit->children[0].id != 2)) return fail(UNSUPPORTED,"unsupported Parquet timestamp unit");
        column.converted = unit->children[0].id == 1 ? 9 : 10;
      } else if (annotation.id != 10) return fail(UNSUPPORTED,"unsupported Parquet logical type");
    }
    const int repetition = field.integer(3,0); column.optional = repetition == 1;
    if (!name || name->bytes.empty() || name->bytes.size() > 256 || name->bytes.find('\0') != std::string::npos ||
        repetition < 0 || repetition > 1 || field.integer(5,0) != 0) return fail(UNSUPPORTED,"nested or repeated Parquet column");
    for (const auto &existing : schema) if (existing.name == name->bytes) return fail(INVALID,"duplicate Parquet column");
    Type type = VARCHAR;
    if (column.type == 0) type = BOOLEAN;
    else if (column.type == 1 || column.type == 2) type = BIGINT;
    else if (column.type == 4 || column.type == 5) type = DOUBLE;
    else if (column.type != 6) return fail(UNSUPPORTED,"unsupported Parquet physical type");
    if (column.converted == 6 && column.type == 1) type = DATE;
    else if ((column.converted == 9 || column.converted == 10) && column.type == 2) type = DATETIME;
    else if (column.converted != -1 && column.converted != 0 && !(column.converted >= 15 && column.converted <= 18)) return fail(UNSUPPORTED,"unsupported Parquet logical type");
    if (column.null_only) type = NULL_TYPE;
    schema.push_back({name->bytes,type,column.optional, static_cast<uint8_t>(type == DATETIME ? (column.converted == 9 ? 3 : 6) : 0)}); physical.push_back(column);
  }
  uint64_t rows = 0;
  for (const auto &row_group : row_groups->children) {
    const auto *columns = row_group.get(1);
    const int64_t count = row_group.integer(3);
    if (!columns || columns->children.size() != schema.size() || count < 0 || uint64_t(count) > uint64_t(INT64_MAX)-rows) return fail(INVALID,"invalid Parquet row group");
    RowGroup group_data; group_data.rows = count; rows += count;
    for (size_t i = 0; i < columns->children.size(); ++i) {
      const auto &chunk = columns->children[i];
      const auto *meta = chunk.get(3), *external = chunk.get(1);
      if (!meta || (external && !external->bytes.empty())) return fail(UNSUPPORTED,"external Parquet column chunks are unsupported");
      const auto *path = meta->get(3);
      if (!path || path->children.size() != 1 || path->children[0].bytes != schema[i].name || meta->integer(1) != physical[i].type) return fail(INVALID,"Parquet column schema mismatch");
      const int64_t offset = meta->integer(9), dictionary = meta->integer(11,offset), compressed = meta->integer(7), values = meta->integer(5);
      const int codec = meta->integer(4);
      if (offset < 4 || dictionary < 4 || compressed < 0 || values != count || uint64_t(std::min(offset,dictionary)) > footer ||
          uint64_t(compressed) > footer-uint64_t(std::min(offset,dictionary))) return fail(INVALID,"Parquet column chunk outside data area");
      if (codec != 0 && codec != 1) return fail(UNSUPPORTED,"only uncompressed and Snappy Parquet are supported");
      Chunk data; data.offset = std::min(offset,dictionary); data.end = data.offset + compressed; data.codec = codec; data.values = values;
      group_data.chunks.push_back(data);
    }
    groups.push_back(std::move(group_data));
  }
  if (metadata.integer(3) < 0 || uint64_t(metadata.integer(3)) != rows) return fail(INVALID,"Parquet row count mismatch");
  return OK;
}

Error ParquetScan::Impl::start_group() {
  cursors.clear();
  if (group >= groups.size()) return END;
  for (int64_t id : selected) {
    if (id < 0 || static_cast<size_t>(id) >= schema.size()) return fail(INVALID,"invalid projected Parquet column");
    Cursor cursor; cursor.chunk = groups[group].chunks[id]; cursor.offset = cursor.chunk.offset;
    cursors.push_back(std::move(cursor));
  }
  row = 0;
  return OK;
}

Error ParquetScan::open(FILE *file, std::vector<Column> &schema) {
  impl_.reset(new Impl()); impl_->file = file;
  Error rc = impl_->metadata();
  if (rc != OK) return rc;
  schema = impl_->schema;
  for (size_t i = 0; i < schema.size(); ++i) impl_->selected.push_back(i);
  return rescan();
}
Error ParquetScan::project(const std::vector<int64_t> &columns) {
  impl_->selected = columns; return rescan();
}
Error ParquetScan::rescan() { impl_->group = 0; const Error rc = impl_->start_group(); return rc == END ? OK : rc; }

Error ParquetScan::Impl::page(size_t index) {
  Cursor &cursor = cursors[index];
  const auto &column = physical[selected[index]];
  while (cursor.offset < cursor.chunk.end) {
    std::string header_bytes;
    const size_t available = std::min<uint64_t>(65536,cursor.chunk.end-cursor.offset);
    if (!read_at(file,cursor.offset,available,size,header_bytes)) return fail(IO,"cannot read Parquet page header");
    CompactNode header; CompactReader reader(header_bytes);
    if (!reader.structure(header)) return fail(INVALID,"invalid Parquet page header");
    cursor.offset += reader.position();
    const int64_t compressed = header.integer(3), uncompressed = header.integer(2), type = header.integer(1);
    if (compressed < 0 || uncompressed < 0 || compressed > MAX_PAGE_BYTES || uncompressed > MAX_PAGE_BYTES || uint64_t(compressed) > cursor.chunk.end-cursor.offset) return fail(LIMIT,"invalid Parquet page size");
    std::string packed;
    if (!read_at(file,cursor.offset,compressed,size,packed)) return fail(IO,"cannot read Parquet page");
    cursor.offset += compressed;
    if (type == 2) {
      const auto *dictionary = header.get(7);
      if (!dictionary || !cursor.dictionary.empty() || dictionary->integer(2) != 0 || dictionary->integer(1) < 0 || dictionary->integer(1) > MAX_PAGE_VALUES ||
          !decompress(cursor.chunk.codec,packed,uncompressed,cursor.data)) return fail(INVALID,"invalid Parquet dictionary");
      size_t pos = 0, bit = 0;
      for (int64_t i = 0; i < dictionary->integer(1); ++i) {
        Cell value;
        if (!plain(column,cursor.data,pos,bit,value)) return fail(INVALID,"invalid Parquet dictionary value");
        cursor.dictionary.push_back(std::move(value));
      }
      if (memory() > MAX_SCAN_BYTES) return fail(LIMIT,"Parquet decoder exceeds 64 MiB");
      continue;
    }
    if (type != 0 && type != 3) return fail(UNSUPPORTED,"unsupported Parquet page type");
    const auto *data_header = header.get(type == 0 ? 5 : 8);
    if (!data_header || data_header->integer(1) <= 0 || data_header->integer(1) > MAX_PAGE_VALUES) return fail(INVALID,"invalid Parquet data page");
    cursor.count = data_header->integer(1); cursor.row = cursor.data_pos = cursor.boolean_bit = 0;
    if (cursor.seen > cursor.chunk.values || cursor.count > cursor.chunk.values-cursor.seen) return fail(INVALID,"too many Parquet values");
    cursor.seen += cursor.count;
    cursor.encoding = data_header->integer(type == 0 ? 2 : 4);
    size_t pos = 0;
    if (type == 0) {
      if (!decompress(cursor.chunk.codec,packed,uncompressed,cursor.data)) return fail(INVALID,"Parquet decompression failed");
      if (column.optional) {
        if (data_header->integer(3) != 3 || cursor.data.size() < 4) return fail(UNSUPPORTED,"unsupported Parquet definition encoding");
        const size_t levels_size = little(cursor.data.data(),4); pos = 4;
        if (levels_size > cursor.data.size()-pos) return fail(INVALID,"truncated Parquet definition levels");
        const size_t end = pos+levels_size;
        if (!hybrid(cursor.data,pos,end,1,cursor.count,cursor.definitions) || pos != end) return fail(INVALID,"invalid Parquet definition levels");
      }
    } else {
      const int64_t def_size = data_header->integer(5), rep_size = data_header->integer(6), nulls = data_header->integer(2);
      if (rep_size != 0 || def_size < 0 || uint64_t(def_size) > packed.size() || def_size > uncompressed || nulls < 0 || uint64_t(nulls) > cursor.count ||
          data_header->integer(3) != int64_t(cursor.count)) return fail(INVALID,"invalid Parquet V2 levels");
      const std::string values = packed.substr(def_size);
      std::string decoded;
      const int codec = data_header->integer(7,1) ? cursor.chunk.codec : 0;
      if (!decompress(codec,values,uncompressed-def_size,decoded)) return fail(INVALID,"Parquet V2 decompression failed");
      cursor.data = packed.substr(0,def_size) + decoded;
      if (column.optional && (!hybrid(cursor.data,pos,def_size,1,cursor.count,cursor.definitions) || pos != size_t(def_size))) return fail(INVALID,"invalid Parquet V2 definition levels");
      if (!column.optional && def_size != 0) return fail(INVALID,"unexpected Parquet definition levels");
      if (column.optional && std::count(cursor.definitions.begin(),cursor.definitions.end(),0U) != nulls) return fail(INVALID,"Parquet null count mismatch");
    }
    const size_t present = column.optional ? std::count(cursor.definitions.begin(),cursor.definitions.end(),1U) : cursor.count;
    cursor.indices.clear();
    if (cursor.encoding == 3 && column.type == 0) {
      if (cursor.data.size()-pos < 4) return fail(INVALID,"missing Parquet Boolean RLE length");
      const size_t length = little(cursor.data.data()+pos,4); pos += 4;
      if (length > cursor.data.size()-pos || !hybrid(cursor.data,pos,pos+length,1,present,cursor.indices)) return fail(INVALID,"invalid Parquet Boolean RLE values");
      cursor.data_pos = 0;
    } else if (cursor.encoding == 2 || cursor.encoding == 8) {
      if (cursor.dictionary.empty() && present) return fail(INVALID,"missing Parquet dictionary");
      if (present) {
        if (pos == cursor.data.size()) return fail(INVALID,"missing Parquet dictionary indices");
        const unsigned width = static_cast<unsigned char>(cursor.data[pos++]);
        if (!hybrid(cursor.data,pos,cursor.data.size(),width,present,cursor.indices)) return fail(INVALID,"invalid Parquet dictionary indices");
        for (uint32_t id : cursor.indices) if (id >= cursor.dictionary.size()) return fail(INVALID,"Parquet dictionary index out of range");
      }
      cursor.data_pos = 0;
    } else if (cursor.encoding == 0) cursor.data_pos = pos;
    else return fail(UNSUPPORTED,"unsupported Parquet value encoding");
    if (memory() > MAX_SCAN_BYTES) return fail(LIMIT,"Parquet decoder exceeds 64 MiB");
    return OK;
  }
  return fail(INVALID,"Parquet column ended before row group");
}

Error ParquetScan::next(std::vector<Cell> &result) {
  Impl &state = *impl_;
  while (state.group < state.groups.size() && state.row == state.groups[state.group].rows) {
    for (const auto &cursor : state.cursors) if (cursor.row != cursor.count || cursor.seen != cursor.chunk.values) return state.fail(INVALID,"Parquet column row count mismatch");
    ++state.group;
    const Error rc = state.start_group(); if (rc == END) return END; if (rc != OK) return rc;
  }
  if (state.group == state.groups.size()) return END;
  result.assign(state.schema.size(),Cell());
  for (size_t i = 0; i < state.cursors.size(); ++i) {
    Cursor &cursor = state.cursors[i];
    if (cursor.row == cursor.count) { const Error rc = state.page(i); if (rc != OK) return rc; }
    const int64_t id = state.selected[i];
    Cell &cell = result[id];
    const bool present = !state.physical[id].optional || cursor.definitions[cursor.row] == 1;
    ++cursor.row;
    if (!present) continue;
    if (state.physical[id].null_only) return state.fail(INVALID,"non-null value in Parquet NULL column");
    if (cursor.encoding == 3 && state.physical[id].type == 0) {
      if (cursor.data_pos >= cursor.indices.size()) return state.fail(INVALID,"truncated Parquet Boolean values");
      cell.is_null = false; cell.text = cursor.indices[cursor.data_pos++] ? "true" : "false";
    } else if (cursor.encoding == 2 || cursor.encoding == 8) {
      if (cursor.data_pos >= cursor.indices.size()) return state.fail(INVALID,"truncated Parquet dictionary indices");
      cell = cursor.dictionary[cursor.indices[cursor.data_pos++]];
    } else if (!plain(state.physical[id],cursor.data,cursor.data_pos,cursor.boolean_bit,cell)) return state.fail(INVALID,"invalid Parquet plain value");
  }
  ++state.row;
  return OK;
}

} } }
