/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#pragma once
#include <cerrno>
#include "sql/engine/basic/file_scan_definition.h"

namespace oceanbase { namespace sql {
inline bool is_file_copy_packet(const common::ObString &packet)
{ return packet.length() > 7 && std::memcmp(packet.ptr(), "FSCOPY1", 7) == 0; }

class FileCopyWriter
{
public:
  FileCopyWriter() = default;
  FileCopyWriter(const FileCopyWriter &) = delete;
  FileCopyWriter &operator=(const FileCopyWriter &) = delete;
  ~FileCopyWriter() { abort(); }
  void abort()
  {
#ifndef _WIN32
    if (output_) { std::fclose(output_); output_ = nullptr; }
    if (temporary_[0] && directory_.stream()) {
      ::unlinkat(::fileno(directory_.stream()), temporary_, 0);
      temporary_[0] = 0;
    }
#endif
    directory_.close();
  }
  int open(const common::ObString &root, const common::ObString &path)
  {
    using namespace common;
    abort();
#ifdef _WIN32
    return OB_NOT_SUPPORTED;
#else
    if (path.empty() || path.length() >= PATH_MAX || std::memchr(path.ptr(), 0, path.length())) { return OB_INVALID_ARGUMENT; }
    char input[PATH_MAX] = {};
    std::memcpy(input, path.ptr(), path.length());
    char *slash = std::strrchr(input, '/');
    const char *name = slash ? slash+1 : input;
    if (*name == 0 || std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0
        || std::strlen(name) > NAME_MAX) { return OB_INVALID_ARGUMENT; }
    std::strcpy(target_, name);
    if (slash) { *slash = 0; }
    const char *parent = slash ? (slash == input ? "/" : input) : ".";
    int ret = directory_.open(root, ObString::make_string(parent), true);
    if (ret != OB_SUCCESS) { return ret; }
    const int dir = ::fileno(directory_.stream());
    struct stat exists = {};
    if (::fstatat(dir, target_, &exists, AT_SYMLINK_NOFOLLOW) == 0) { return OB_FILE_ALREADY_EXIST; }
    if (errno != ENOENT) { return OB_IO_ERROR; }
    unsigned char random[16];
    size_t count = 0;
    const int random_fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (random_fd < 0) { return OB_IO_ERROR; }
    while (count < sizeof(random)) {
      const ssize_t got = ::read(random_fd, random+count, sizeof(random)-count);
      if (got < 0 && errno == EINTR) { continue; }
      if (got <= 0) { ::close(random_fd); return OB_IO_ERROR; }
      count += got;
    }
    ::close(random_fd);
    std::strcpy(temporary_, ".seekdb-copy-");
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(random); ++i) {
      temporary_[13+i*2] = hex[random[i] >> 4];
      temporary_[14+i*2] = hex[random[i] & 15];
    }
    const int fd = ::openat(dir, temporary_, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { temporary_[0] = 0; return errno == EEXIST ? OB_FILE_ALREADY_EXIST : OB_IO_ERROR; }
    output_ = ::fdopen(fd, "wb");
    if (!output_) { ::close(fd); abort(); return OB_IO_ERROR; }
    return OB_SUCCESS;
#endif
  }
  int write(const common::ObIArray<CsvField> &fields, const FileScanDefinition &options)
  {
    using namespace common;
    if (!output_) { return OB_NOT_INIT; }
    int ret = OB_SUCCESS;
    for (int64_t i = 0; ret == OB_SUCCESS && i < fields.count(); ++i) {
      if (i > 0 && std::fputc(options.delimiter_, output_) == EOF) { return OB_IO_ERROR; }
      const CsvField &field = fields.at(i);
      if (field.value_.empty() && !field.quoted_) {
        if (!options.null_text_.empty() && std::fwrite(options.null_text_.ptr(), 1, options.null_text_.length(), output_)
            != static_cast<size_t>(options.null_text_.length())) { ret = OB_IO_ERROR; }
      } else {
        if (std::fputc(options.quote_, output_) == EOF) { return OB_IO_ERROR; }
        for (int64_t j = 0; j < field.value_.length(); ++j) {
          const unsigned char ch = field.value_[j];
          if ((ch == options.quote_ || (options.escape_ != options.quote_ && ch == options.escape_))
              && std::fputc(options.escape_, output_) == EOF) { return OB_IO_ERROR; }
          if (std::fputc(ch, output_) == EOF) { return OB_IO_ERROR; }
        }
        if (std::fputc(options.quote_, output_) == EOF) { return OB_IO_ERROR; }
      }
    }
    if (ret == OB_SUCCESS && std::fputc('\n', output_) == EOF) { ret = OB_IO_ERROR; }
    return ret;
  }
  int commit()
  {
    using namespace common;
#ifdef _WIN32
    return OB_NOT_SUPPORTED;
#else
    if (!output_ || !directory_.stream() || !temporary_[0]) { return OB_NOT_INIT; }
    if (std::fflush(output_) != 0 || ::fsync(::fileno(output_)) != 0) { return OB_IO_ERROR; }
    const int dir = ::fileno(directory_.stream());
    // linkat publishes a complete file atomically and fails if the target now
    // exists. No error path truncates or replaces an existing destination.
    if (::linkat(dir, temporary_, dir, target_, 0) != 0) { return errno == EEXIST ? OB_FILE_ALREADY_EXIST : OB_IO_ERROR; }
    if (::unlinkat(dir, temporary_, 0) != 0) { return OB_IO_ERROR; }
    temporary_[0] = 0;
    if (std::fclose(output_) != 0) { output_ = nullptr; return OB_IO_ERROR; }
    output_ = nullptr;
    return ::fsync(dir) == 0 ? OB_SUCCESS : OB_IO_ERROR;
#endif
  }
private:
  CsvFileTable directory_;
  FILE *output_ = nullptr;
#ifndef _WIN32
  char target_[NAME_MAX+1] = {};
  char temporary_[64] = {};
#endif
};

struct FileCopyState
{
  FileScanDefinition options_;
  FileCopyWriter writer_;
  common::ObArenaAllocator row_allocator_;
  bool done_ = false;
};
} }
