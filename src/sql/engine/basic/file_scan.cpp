// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "file_scan.h"
#include "parquet_scan.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <unistd.h>
#include <rapidjson/document.h>

namespace oceanbase { namespace sql { namespace file_sql {
namespace {
constexpr size_t MAX_ROW = 8 * 1024 * 1024;
constexpr size_t MAX_COLUMNS = 1024;
constexpr size_t MAX_FILES = 100000;
const char MAGIC[] = "SEEKDB_FILE_SQL_2:";

std::string identifier_key(std::string name)
{
  for (char &byte : name) if (static_cast<unsigned char>(byte) < 128) byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
  return name;
}

bool within(const std::string &root, const std::string &path)
{
  return root == "/" || (path.size() > root.size() &&
    path.compare(0, root.size(), root) == 0 && path[root.size()] == '/');
}

Error canonical_options(const Options &input, Options &output, bool pattern)
{
  if (input.root.empty() || input.root == "NULL" || input.root.find('\0') != std::string::npos ||
      input.path.empty() || input.path.find('\0') != std::string::npos ||
      input.root.size() >= PATH_MAX || input.path.size() >= PATH_MAX) return DENIED;
  char resolved[PATH_MAX];
  output = input;
  if (!realpath(input.root.c_str(), resolved)) return DENIED;
  output.root = resolved;
  if (pattern) {
    size_t wildcard = input.path.find_first_of("*?[");
    size_t slash = input.path.rfind('/', wildcard);
    std::string parent = slash == std::string::npos ? "." : input.path.substr(0, slash);
    if (parent.empty()) parent = "/";
    if (!realpath(parent.c_str(), resolved)) return DENIED;
    std::string canonical_parent = resolved;
    if (canonical_parent != output.root && !within(output.root, canonical_parent)) return DENIED;
    output.path = canonical_parent + (canonical_parent == "/" ? "" : "/") + input.path.substr(slash == std::string::npos ? 0 : slash + 1);
  } else {
    if (!realpath(input.path.c_str(), resolved)) return IO;
    output.path = resolved;
    if (!within(output.root, output.path)) return DENIED;
  }
  return OK;
}

int safe_open(const Options &options)
{
  int dir = ::open(options.root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (dir < 0) return -1;
  const std::string relative = options.path.substr(options.root == "/" ? 1 : options.root.size() + 1);
  size_t pos = 0;
  while (true) {
    const size_t end = relative.find('/', pos);
    const std::string component = relative.substr(pos, end == std::string::npos ? end : end - pos);
    if (component.empty() || component == "." || component == "..") { ::close(dir); return -1; }
    const int flags = end == std::string::npos ? O_RDONLY | O_NONBLOCK : O_RDONLY | O_DIRECTORY;
    const int next = openat(dir, component.c_str(), flags | O_CLOEXEC | O_NOFOLLOW);
    ::close(dir);
    if (next < 0 || end == std::string::npos) return next;
    dir = next;
    pos = end + 1;
  }
}

bool same_file(const struct stat &a, const struct stat &b)
{
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
    a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
    a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}

bool valid_date(const std::string &s)
{
  if (s.size() < 10 || s[4] != '-' || s[7] != '-') return false;
  for (size_t i = 0; i < 10; ++i) if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9')) return false;
  const int year = std::atoi(s.substr(0, 4).c_str());
  const int month = std::atoi(s.substr(5, 2).c_str());
  const int day = std::atoi(s.substr(8, 2).c_str());
  const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  return year >= 1 && month >= 1 && month <= 12 && day >= 1 &&
    day <= days[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

bool valid_utf8(const std::string &s)
{
  for (size_t pos = 0; pos < s.size();) {
    unsigned char first = static_cast<unsigned char>(s[pos++]);
    if (first < 0x80) continue;
    unsigned n = first >= 0xc2 && first <= 0xdf ? 1 : first >= 0xe0 && first <= 0xef ? 2 : first >= 0xf0 && first <= 0xf4 ? 3 : 0;
    if (!n || n > s.size()-pos) return false;
    uint32_t code = first & (0x7f >> n);
    for (unsigned i = 0; i < n; ++i) {
      unsigned char byte = static_cast<unsigned char>(s[pos++]);
      if ((byte & 0xc0) != 0x80) return false;
      code = (code << 6) | (byte & 0x3f);
    }
    if ((n == 2 && code < 0x800) || (n == 3 && code < 0x10000) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
  }
  return true;
}

void put(std::string &out, const std::string &value)
{
  uint64_t n = value.size();
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>(n >> (i * 8)));
  out += value;
}

bool take(const std::string &in, size_t &pos, std::string &value)
{
  if (pos > in.size() || in.size() - pos < 8) return false;
  uint64_t n = 0;
  for (int i = 0; i < 8; ++i) n |= uint64_t(static_cast<unsigned char>(in[pos++])) << (i * 8);
  if (n > MAX_ROW || n > in.size() - pos) return false;
  value.assign(in, pos, static_cast<size_t>(n));
  pos += n;
  return true;
}
}

FileScan::FileScan() = default;
FileScan::~FileScan() { close(); }
void FileScan::close() { parquet_.reset(); if (file_) { fclose(file_); file_ = nullptr; } }
Error FileScan::fail(Error code, const std::string &message)
{
  error_ = definition_.options.path + ":" + std::to_string(line_) + ": " + message;
  return code;
}

Type FileScan::infer_type(const std::string &s)
{
  if (s == "true" || s == "false" || s == "TRUE" || s == "FALSE") return BOOLEAN;
  if (valid_date(s)) {
    if (s.size() == 10) return DATE;
    if (s.size() >= 19 && s.size() <= 26 && (s[10] == ' ' || s[10] == 'T') && s[13] == ':' && s[16] == ':') {
      bool valid = true;
      for (size_t i = 11; i < s.size(); ++i) {
        if (i == 13 || i == 16) continue;
        if (i == 19) valid &= s[i] == '.' && s.size() > 20;
        else valid &= s[i] >= '0' && s[i] <= '9';
      }
      if (valid && std::atoi(s.substr(11,2).c_str()) < 24 && std::atoi(s.substr(14,2).c_str()) < 60 &&
          std::atoi(s.substr(17,2).c_str()) < 60) return DATETIME;
    }
    return VARCHAR;
  }
  if (s.empty()) return VARCHAR;
  size_t pos = (s[0] == '+' || s[0] == '-') ? 1 : 0;
  bool integer = pos < s.size();
  for (size_t i = pos; i < s.size(); ++i) integer &= s[i] >= '0' && s[i] <= '9';
  char *end = nullptr;
  if (integer) {
    errno = 0;
    std::strtoll(s.c_str(), &end, 10);
    return errno == ERANGE ? VARCHAR : BIGINT;
  }
  // strtod accepts NaN, infinity, whitespace and hex; file SQL does not.
  if (s.find_first_not_of("+-0123456789.eE") != std::string::npos) return VARCHAR;
  errno = 0;
  const double value = std::strtod(s.c_str(), &end);
  return end == s.c_str() + s.size() && end != s.c_str() && errno != ERANGE && std::isfinite(value) ? DOUBLE : VARCHAR;
}

Type FileScan::widen(Type a, Type b)
{
  if (a == NULL_TYPE) return b;
  if (b == NULL_TYPE || a == b) return a;
  if ((a == BIGINT && b == DOUBLE) || (a == DOUBLE && b == BIGINT)) return DOUBLE;
  if ((a == DATE && b == DATETIME) || (a == DATETIME && b == DATE)) return DATETIME;
  return VARCHAR;
}

Error FileScan::open_file(const Options &options)
{
  close();
  const int fd = safe_open(options);
  if (fd < 0) return fail(DENIED, "cannot open regular file inside secure_file_priv");
  struct stat info {};
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) { ::close(fd); return fail(DENIED, "not a regular file"); }
  file_ = fdopen(fd, "rb");
  if (!file_) { ::close(fd); return fail(IO, "fdopen failed"); }
  line_ = 1;
  return OK;
}

Error FileScan::check_identity()
{
  struct stat current {}, named {};
  if (!file_ || fstat(fileno(file_), &current) != 0 || stat(definition_.options.path.c_str(), &named) != 0)
    return fail(CHANGED, "file disappeared");
  if (!same_file(definition_.identity, current) || !same_file(current, named)) return fail(CHANGED, "file changed during query");
  return OK;
}

Error FileScan::csv_row(std::vector<Cell> &row)
{
  row.clear();
  Cell field;
  bool quoted = false, was_quoted = false, after_quote = false, any = false;
  size_t bytes = 0;
  while (true) {
    const int c = fgetc(file_);
    if (c == EOF && ferror(file_)) return fail(IO, "file read failed");
    if (c != EOF && ++bytes > MAX_ROW) return fail(LIMIT, "row exceeds 8 MiB");
    if (c == EOF && !any && row.empty()) return END;
    if (c == EOF && quoted) return fail(INVALID, "unterminated quoted CSV field");
    if (quoted) {
      if (c == definition_.options.escape && c != definition_.options.quote) {
        const int escaped = fgetc(file_);
        if (escaped == EOF) return fail(INVALID, "truncated CSV escape");
        field.text.push_back(static_cast<char>(escaped));
        if (escaped == '\n') ++line_;
      } else if (c == definition_.options.quote) {
        if (definition_.options.escape == definition_.options.quote) {
          const int next = fgetc(file_);
          if (next == c) field.text.push_back(static_cast<char>(c));
          else { if (next != EOF) ungetc(next, file_); quoted = false; after_quote = true; }
        } else { quoted = false; after_quote = true; }
      } else {
        field.text.push_back(static_cast<char>(c));
        if (c == '\n') ++line_;
      }
      any = true;
      continue;
    }
    if (c == EOF || c == definition_.options.delimiter || c == '\n' || c == '\r') {
      if (!valid_utf8(field.text)) return fail(INVALID, "invalid UTF-8 in CSV field");
      field.is_null = !was_quoted && field.text == definition_.options.nullstr;
      row.push_back(std::move(field));
      if (row.size() > MAX_COLUMNS) return fail(LIMIT, "too many columns");
      field = Cell();
      was_quoted = after_quote = false;
      if (c == definition_.options.delimiter) { any = true; continue; }
      if (c == '\r') { const int next = fgetc(file_); if (next != '\n' && next != EOF) ungetc(next, file_); }
      ++line_;
      return OK;
    }
    if (after_quote) return fail(INVALID, "characters after closing CSV quote");
    if (c == definition_.options.quote) {
      if (!field.text.empty()) return fail(INVALID, "quote in unquoted CSV field");
      quoted = was_quoted = true;
    } else field.text.push_back(static_cast<char>(c));
    any = true;
  }
}

Error FileScan::json_row(std::vector<Cell> &row, bool inference)
{
  std::string line;
  int c;
  do {
    line.clear();
    while ((c = fgetc(file_)) != EOF && c != '\n') {
      if (line.size() >= MAX_ROW) return fail(LIMIT, "JSON line exceeds 8 MiB");
      line.push_back(static_cast<char>(c));
    }
    if (ferror(file_)) return fail(IO, "file read failed");
    if (c == EOF && line.empty()) return END;
    ++line_;
  } while (line.find_first_not_of(" \t\r") == std::string::npos);
  rapidjson::Document doc;
  bool in_string = false, escaped = false, opened = false;
  for (char byte : line) {
    if (in_string) {
      if (escaped) escaped = false;
      else if (byte == '\\') escaped = true;
      else if (byte == '"') in_string = false;
    } else if (byte == '"') in_string = true;
    else if (byte == '[' || (byte == '{' && opened)) return fail(INVALID, "nested JSON objects and arrays are unsupported");
    else if (byte == '{') opened = true;
  }
  doc.Parse<rapidjson::kParseValidateEncodingFlag>(line.c_str(), line.size());
  if (doc.HasParseError() || !doc.IsObject()) return fail(INVALID, "expected a flat JSON object");
  if (doc.MemberCount() > MAX_COLUMNS) return fail(LIMIT, "too many JSON fields");
  row.assign(definition_.columns.size(), Cell());
  std::vector<std::string> seen;
  for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
    const std::string name(it->name.GetString(), it->name.GetStringLength());
    if (name.empty() || name.size() > 256 || name.find('\0') != std::string::npos ||
        std::find(seen.begin(), seen.end(), name) != seen.end()) return fail(INVALID, "invalid or duplicate JSON key");
    seen.push_back(name);
    for (const auto &column : definition_.columns) {
      if (column.name != name && identifier_key(column.name) == identifier_key(name)) return fail(INVALID, "case-insensitive JSON column collision");
    }
    auto col = std::find_if(definition_.columns.begin(), definition_.columns.end(),
      [&](const Column &column) { return column.name == name; });
    if (col == definition_.columns.end()) {
      if (!inference) return fail(CHANGED, "JSON schema changed");
      if (definition_.columns.size() == MAX_COLUMNS) return fail(LIMIT, "too many union columns");
      definition_.columns.push_back({name, NULL_TYPE, json_rows_ != 0});
      row.push_back(Cell());
      col = definition_.columns.end() - 1;
    }
    Cell &cell = row[static_cast<size_t>(col - definition_.columns.begin())];
    const auto &value = it->value;
    Type type = NULL_TYPE;
    if (value.IsNull()) continue;
    cell.is_null = false;
    if (value.IsString()) {
      cell.text.assign(value.GetString(), value.GetStringLength());
      type = infer_type(cell.text);
      if (type != DATE && type != DATETIME) type = VARCHAR;
    } else if (value.IsBool()) { cell.text = value.GetBool() ? "true" : "false"; type = BOOLEAN; }
    else if (value.IsInt64()) { cell.text = std::to_string(value.GetInt64()); type = BIGINT; }
    else if (value.IsUint64()) { cell.text = std::to_string(value.GetUint64()); type = VARCHAR; }
    else if (value.IsDouble() && std::isfinite(value.GetDouble())) {
      char number[32]; std::snprintf(number, sizeof(number), "%.17g", value.GetDouble());
      cell.text = number; type = DOUBLE;
    } else return fail(INVALID, "nested JSON objects and arrays are unsupported");
    if (inference) {
      col->type = widen(col->type, type);
      if (type == DATETIME && cell.text.size() > 20)
        col->scale = std::max<uint8_t>(col->scale, cell.text.size() - 20);
    }
  }
  if (inference) {
    for (size_t i = 0; i < row.size(); ++i) definition_.columns[i].nullable |= row[i].is_null;
    ++json_rows_;
  }
  return OK;
}

Error FileScan::discover()
{
  glob_t matches {};
  int rc = ::glob(definition_.options.path.c_str(), GLOB_ERR | GLOB_NOSORT, nullptr, &matches);
  paths_.clear(); path_index_ = 0;
  if (rc != 0 && rc != GLOB_NOMATCH) { globfree(&matches); return fail(IO, "glob failed"); }
  if (matches.gl_pathc > MAX_FILES) { globfree(&matches); return fail(LIMIT, "too many matching files"); }
  for (size_t i = 0; i < matches.gl_pathc; ++i) {
    Options candidate = definition_.options;
    candidate.path = matches.gl_pathv[i];
    Options canonical;
    if (canonical_options(candidate, canonical, false) != OK) { globfree(&matches); return fail(DENIED, "glob escapes secure_file_priv"); }
    int fd = safe_open(canonical);
    struct stat info {};
    if (fd < 0) { globfree(&matches); return fail(DENIED, "unsafe glob entry"); }
    const bool regular = fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
    ::close(fd);
    if (regular) paths_.push_back(canonical.path);
  }
  globfree(&matches);
  std::sort(paths_.begin(), paths_.end());
  return OK;
}

Error FileScan::infer(const Options &options, Definition &definition)
{
  close(); error_.clear(); definition_ = Definition(); json_rows_ = 0;
  if (options.format != "csv" && options.format != "json" && options.format != "glob" && options.format != "parquet") return fail(UNSUPPORTED, "unsupported file format");
  if (options.delimiter == '\0' || options.delimiter == '\n' || options.delimiter == '\r' ||
      options.delimiter == options.quote || options.quote == '\0' || options.escape == '\0') return fail(INVALID, "invalid CSV options");
  Error rc = canonical_options(options, definition_.options, options.format == "glob");
  if (rc != OK) return fail(rc, "invalid file path or secure_file_priv");
  if (options.format == "glob") {
    definition_.columns.push_back({"file", VARCHAR});
    rc = discover();
  } else {
    if ((rc = open_file(definition_.options)) != OK) return rc;
    if (fstat(fileno(file_), &definition_.identity) != 0) return fail(IO, "fstat failed");
    if (options.format == "parquet") {
      parquet_.reset(new ParquetScan());
      rc = parquet_->open(file_, definition_.columns);
      if (rc != OK) return fail(rc, parquet_->error());
      if ((rc = check_identity()) != OK) return rc;
      definition = definition_;
      return rescan();
    }
    std::vector<Cell> row;
    if (options.format == "csv") {
      rc = csv_row(row);
      if (rc == END) return fail(INVALID, "CSV has no columns");
      if (rc != OK) return rc;
      std::vector<std::string> names;
      for (size_t i = 0; i < row.size(); ++i) {
        std::string name = options.header ? row[i].text : "column" + std::to_string(i);
        if (i == 0 && name.compare(0, 3, "\xef\xbb\xbf") == 0) name.erase(0, 3);
        if (name.empty()) name = "column" + std::to_string(i);
        if (name.size() > 256 || name.find('\0') != std::string::npos) return fail(INVALID, "invalid CSV column name");
        std::string unique = name;
        for (size_t suffix = 2; std::find(names.begin(), names.end(), identifier_key(unique)) != names.end(); ++suffix) unique = name + "__" + std::to_string(suffix);
        names.push_back(identifier_key(unique));
        definition_.columns.push_back({unique, options.header || row[i].is_null ? NULL_TYPE : infer_type(row[i].text), !options.header && row[i].is_null});
        if (!options.header && !row[i].is_null && definition_.columns.back().type == DATETIME && row[i].text.size() > 20)
          definition_.columns.back().scale = row[i].text.size() - 20;
      }
      while ((rc = csv_row(row)) == OK) {
        if (row.size() != definition_.columns.size()) return fail(INVALID, "CSV column count mismatch");
        for (size_t i = 0; i < row.size(); ++i) {
          if (!row[i].is_null) {
            const Type type = infer_type(row[i].text);
            definition_.columns[i].type = widen(definition_.columns[i].type, type);
            if (type == DATETIME && row[i].text.size() > 20)
              definition_.columns[i].scale = std::max<uint8_t>(definition_.columns[i].scale, row[i].text.size() - 20);
          }
          else definition_.columns[i].nullable = true;
        }
      }
    } else while ((rc = json_row(row, true)) == OK) {}
    if (rc != END) return rc;
    if ((rc = check_identity()) != OK) return rc;
    if (definition_.columns.empty()) return fail(INVALID, "file has no columns");
  }
  if (rc != OK) return rc;
  definition = definition_;
  return rescan();
}

Error FileScan::open(const Definition &definition)
{
  close(); error_.clear(); definition_ = definition;
  Options canonical;
  Error rc = canonical_options(definition.options, canonical, definition.options.format == "glob");
  if (rc != OK || canonical.path != definition.options.path) return fail(DENIED, "file path changed");
  definition_.options = canonical;
  if (definition.options.format == "glob") return discover();
  if ((rc = open_file(definition_.options)) != OK || (rc = check_identity()) != OK) return rc;
  if (definition.options.format == "parquet") {
    parquet_.reset(new ParquetScan());
    std::vector<Column> schema;
    rc = parquet_->open(file_, schema);
    if (rc != OK) return fail(rc, parquet_->error());
    if (schema.size() != definition.columns.size()) return fail(CHANGED, "Parquet schema changed");
    for (size_t i = 0; i < schema.size(); ++i) if (schema[i].name != definition.columns[i].name || schema[i].type != definition.columns[i].type) return fail(CHANGED, "Parquet schema changed");
  }
  return rescan();
}

Error FileScan::rescan()
{
  if (definition_.options.format == "glob") { path_index_ = 0; return OK; }
  Error rc = check_identity();
  if (rc != OK) return rc;
  if (parquet_) return parquet_->rescan();
  if (fseeko(file_, 0, SEEK_SET) != 0) return fail(IO, "file rewind failed");
  clearerr(file_); line_ = 1;
  if (definition_.options.format == "csv" && definition_.options.header) {
    std::vector<Cell> header;
    return csv_row(header);
  }
  return OK;
}

Error FileScan::next(std::vector<Cell> &row)
{
  if (definition_.options.format == "glob") {
    if (path_index_ == paths_.size()) return END;
    row.assign(1, Cell()); row[0].is_null = false; row[0].text = paths_[path_index_++]; return OK;
  }
  Error rc = check_identity();
  if (rc != OK) return rc;
  rc = parquet_ ? parquet_->next(row) : definition_.options.format == "csv" ? csv_row(row) : json_row(row, false);
  if (parquet_ && rc != OK && rc != END) return fail(rc, parquet_->error());
  if (rc == OK && row.size() != definition_.columns.size()) return fail(INVALID, "column count mismatch");
  if (rc == END) { const Error check = check_identity(); if (check != OK) return check; }
  return rc;
}

Error FileScan::project(const std::vector<int64_t> &columns)
{
  return parquet_ ? parquet_->project(columns) : OK;
}

bool FileScan::is_definition(const std::string &bytes) { return bytes.compare(0, sizeof(MAGIC)-1, MAGIC) == 0; }
std::string FileScan::encode(const Definition &d)
{
  std::string out(MAGIC);
  put(out, d.options.format); put(out, d.options.path); put(out, d.options.root); put(out, d.options.nullstr);
  out.push_back(d.options.delimiter); out.push_back(d.options.quote); out.push_back(d.options.escape); out.push_back(d.options.header);
  // The descriptor remains local to the server and is never persisted across architectures.
  put(out, std::string(reinterpret_cast<const char *>(&d.identity), sizeof(d.identity)));
  put(out, std::to_string(d.columns.size()));
  for (const auto &c : d.columns) { put(out, c.name); out.push_back(static_cast<char>(c.type)); out.push_back(c.nullable); out.push_back(c.scale); }
  return out;
}

Error FileScan::decode(const std::string &bytes, Definition &d)
{
  if (!is_definition(bytes) || bytes.size() > MAX_ROW) return INVALID;
  d = Definition(); size_t pos = sizeof(MAGIC)-1;
  if (!take(bytes,pos,d.options.format) || !take(bytes,pos,d.options.path) || !take(bytes,pos,d.options.root) || !take(bytes,pos,d.options.nullstr) || bytes.size()-pos < 4) return INVALID;
  d.options.delimiter = bytes[pos++]; d.options.quote = bytes[pos++]; d.options.escape = bytes[pos++];
  if (bytes[pos] != 0 && bytes[pos] != 1) return INVALID;
  d.options.header = bytes[pos++] != 0;
  std::string identity, count;
  if (!take(bytes,pos,identity) || identity.size() != sizeof(d.identity) || !take(bytes,pos,count) || count.empty() || count.size() > 4 || count.find_first_not_of("0123456789") != std::string::npos) return INVALID;
  memcpy(&d.identity, identity.data(), sizeof(d.identity));
  const size_t n = std::strtoul(count.c_str(),nullptr,10);
  if (n == 0 || n > MAX_COLUMNS) return INVALID;
  for (size_t i = 0; i < n; ++i) {
    Column column;
    if (!take(bytes,pos,column.name) || column.name.empty() || column.name.size() > 256 || pos == bytes.size()) return INVALID;
    const unsigned type = static_cast<unsigned char>(bytes[pos++]);
    if (type > DATETIME || pos == bytes.size() || (bytes[pos] != 0 && bytes[pos] != 1)) return INVALID;
    column.type = static_cast<Type>(type); column.nullable = bytes[pos++] != 0;
    if (pos == bytes.size() || static_cast<unsigned char>(bytes[pos]) > 6) return INVALID;
    column.scale = static_cast<uint8_t>(bytes[pos++]); d.columns.push_back(std::move(column));
  }
  if (pos != bytes.size() || (d.options.format != "csv" && d.options.format != "json" && d.options.format != "glob" && d.options.format != "parquet")) return INVALID;
  return OK;
}

CsvWriter::~CsvWriter() { close(); }
void CsvWriter::close()
{
  if (file_) { fclose(file_); file_ = nullptr; }
  if (directory_ >= 0) {
    if (!temporary_.empty()) unlinkat(directory_, temporary_.c_str(), 0);
    ::close(directory_); directory_ = -1;
  }
  temporary_.clear();
}

Error CsvWriter::open(const Options &options)
{
  close();
  Error rc = canonical_options(options, options_, true);
  if (rc != OK) return rc;
  if (!options_.delimiter || !options_.quote || !options_.escape || options_.delimiter == options_.quote ||
      options_.delimiter == '\r' || options_.delimiter == '\n') return INVALID;
  const size_t slash = options_.path.rfind('/');
  target_ = options_.path.substr(slash + 1);
  if (target_.empty() || target_ == "." || target_ == ".." || target_.find_first_of("*?[") != std::string::npos) return DENIED;
  Options parent = options_;
  parent.path = slash == 0 ? "/" : options_.path.substr(0, slash);
  directory_ = parent.path == parent.root ? ::open(parent.root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW) : safe_open(parent);
  struct stat info {};
  if (directory_ < 0 || fstat(directory_, &info) != 0 || !S_ISDIR(info.st_mode)) { close(); return DENIED; }
  if (fstatat(directory_, target_.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) { close(); return IO; }
  static std::atomic<uint64_t> sequence{0};
  int fd = -1;
  for (int attempt = 0; attempt < 64 && fd < 0; ++attempt) {
    temporary_ = ".seekdb-copy-" + std::to_string(getpid()) + "-" + std::to_string(sequence.fetch_add(1));
    fd = openat(directory_, temporary_.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0 && errno != EEXIST) { temporary_.clear(); close(); return IO; }
  }
  if (fd < 0) { temporary_.clear(); close(); return IO; }
  file_ = fdopen(fd, "wb");
  if (!file_) { ::close(fd); close(); return IO; }
  return OK;
}

Error CsvWriter::write(const std::vector<Cell> &row)
{
  if (!file_ || row.size() > MAX_COLUMNS) return INVALID;
  std::string line;
  for (size_t i = 0; i < row.size(); ++i) {
    if (i) line.push_back(options_.delimiter);
    if (row[i].is_null) line += options_.nullstr;
    else {
      const auto &text = row[i].text;
      bool quoted = text.empty() || text == options_.nullstr;
      for (char byte : text) quoted |= byte == options_.delimiter || byte == options_.quote || byte == '\r' || byte == '\n';
      if (quoted) line.push_back(options_.quote);
      for (char byte : text) {
        if (quoted && (byte == options_.quote || (options_.escape != options_.quote && byte == options_.escape))) line.push_back(options_.escape);
        line.push_back(byte);
        if (line.size() > 2 * MAX_ROW) return LIMIT;
      }
      if (quoted) line.push_back(options_.quote);
    }
    if (line.size() > 2 * MAX_ROW) return LIMIT;
  }
  line.push_back('\n');
  return fwrite(line.data(), 1, line.size(), file_) == line.size() ? OK : IO;
}

Error CsvWriter::finish()
{
  if (!file_ || directory_ < 0 || temporary_.empty()) return INVALID;
  if (fflush(file_) != 0 || fsync(fileno(file_)) != 0) return IO;
  const int rc = fclose(file_); file_ = nullptr;
  if (rc != 0) return IO;
  // linkat publishes the completed file atomically and never replaces a target.
  if (linkat(directory_, temporary_.c_str(), directory_, target_.c_str(), 0) != 0) return IO;
  if (unlinkat(directory_, temporary_.c_str(), 0) != 0) return IO;
  temporary_.clear(); ::close(directory_); directory_ = -1;
  return OK;
}

} } }
