/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include "lib/container/ob_array.h"
#include "share/ob_errno.h"
#include "sql/engine/basic/file_csv_reader.h"
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace oceanbase { namespace sql {

class CsvFileTable
{
public:
  CsvFileTable() : stream_(nullptr) {}
  CsvFileTable(const CsvFileTable &) = delete;
  CsvFileTable &operator=(const CsvFileTable &) = delete;
  ~CsvFileTable() { close(); }
  void close()
  {
    if (stream_ != nullptr) { std::fclose(stream_); stream_ = nullptr; }
  }

  // Every component is opened relative to a previously opened directory. A
  // symlink replacement after realpath cannot redirect traversal outside it.
  int open(const common::ObString &root, const common::ObString &path, bool directory = false)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    close();
#ifdef _WIN32
    ret = OB_NOT_SUPPORTED;
#else
    char root_input[PATH_MAX] = {}, input[PATH_MAX] = {}, allowed[PATH_MAX] = {}, resolved[PATH_MAX] = {};
    if (root.empty() || root.case_compare("NULL") == 0 || path.empty()
        || root.length() >= PATH_MAX || path.length() >= PATH_MAX
        || std::memchr(root.ptr(), 0, root.length()) || std::memchr(path.ptr(), 0, path.length())) {
      ret = OB_ERR_NO_PRIVILEGE;
    } else {
      std::memcpy(root_input, root.ptr(), root.length());
      std::memcpy(input, path.ptr(), path.length());
      struct stat allowed_status = {};
      if (::realpath(root_input, allowed) == nullptr || ::stat(allowed, &allowed_status) != 0
          || !S_ISDIR(allowed_status.st_mode)) {
        ret = OB_ERR_NO_PRIVILEGE;
      } else if (::realpath(input, resolved) == nullptr) {
        ret = OB_FILE_NOT_EXIST;
      } else {
        const size_t length = std::strlen(allowed);
        if (std::strncmp(allowed, resolved, length) != 0
            || (length != 1 && resolved[length] != 0 && resolved[length] != '/')) {
          ret = OB_ERR_NO_PRIVILEGE;
        } else {
          std::memcpy(canonical_, resolved, std::strlen(resolved) + 1);
          int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
          char *cursor = resolved + 1;
          if (fd < 0) { ret = OB_IO_ERROR; }
          while (ret == OB_SUCCESS && *cursor) {
            char *slash = std::strchr(cursor, '/');
            if (slash != nullptr) { *slash = 0; }
            int next = ::openat(fd, cursor, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (slash ? O_DIRECTORY : O_NONBLOCK));
            ::close(fd);
            fd = next;
            if (fd < 0) {
              ret = OB_ERR_NO_PRIVILEGE;
              break;
            }
            if (slash == nullptr) { break; }
            cursor = slash + 1;
          }
          struct stat status = {};
          if (ret == OB_SUCCESS && (::fstat(fd, &status) != 0
              || (directory ? !S_ISDIR(status.st_mode) : !S_ISREG(status.st_mode)))) {
            ret = OB_INVALID_ARGUMENT;
          }
          if (ret == OB_SUCCESS) {
            status_ = status;
            stream_ = ::fdopen(fd, "rb");
            if (stream_ == nullptr) { ret = OB_IO_ERROR; }
          }
          if (ret != OB_SUCCESS && fd >= 0) { ::close(fd); }
        }
      }
    }
#endif
    return ret;
  }

  int check_unchanged() const
  {
#ifdef _WIN32
    return common::OB_NOT_SUPPORTED;
#else
    struct stat current = {}, named = {};
    if (stream_ == nullptr || ::fstat(::fileno(stream_), &current) != 0 || ::stat(canonical_, &named) != 0) {
      return common::OB_IO_ERROR;
    }
    const auto modified = modification(current), before_modified = modification(status_);
    const auto changed = change(current), before_changed = change(status_);
    const bool same = current.st_dev == status_.st_dev && current.st_ino == status_.st_ino
        && current.st_size == status_.st_size && modified.tv_sec == before_modified.tv_sec
        && modified.tv_nsec == before_modified.tv_nsec && changed.tv_sec == before_changed.tv_sec
        && changed.tv_nsec == before_changed.tv_nsec
        && named.st_dev == status_.st_dev && named.st_ino == status_.st_ino;
    return same ? common::OB_SUCCESS : common::OB_INVALID_DATA;
#endif
  }
  FILE *stream() { return stream_; }
  const char *canonical_path() const
  {
#ifdef _WIN32
    return "";
#else
    return canonical_;
#endif
  }
  int identity(common::ObIArray<int64_t> &values) const
  {
    using namespace common;
    values.reset();
#ifdef _WIN32
    return OB_NOT_SUPPORTED;
#else
    if (stream_ == nullptr) { return OB_NOT_INIT; }
    int ret = OB_SUCCESS;
    const auto modified = modification(status_), changed = change(status_);
    const int64_t items[] = {static_cast<int64_t>(status_.st_dev), static_cast<int64_t>(status_.st_ino),
      status_.st_size, modified.tv_sec, modified.tv_nsec, changed.tv_sec, changed.tv_nsec};
    for (int64_t item : items) {
      if (OB_FAIL(values.push_back(item))) { break; }
    }
    return ret;
#endif
  }
  int matches_identity(const common::ObIArray<int64_t> &expected) const
  {
    using namespace common;
    ObArray<int64_t> current;
    int ret = identity(current);
    if (ret == OB_SUCCESS && expected.count() != current.count()) { ret = OB_INVALID_DATA; }
    for (int64_t i = 0; ret == OB_SUCCESS && i < expected.count(); ++i) {
      if (expected.at(i) != current.at(i)) { ret = OB_INVALID_DATA; }
    }
    return ret;
  }
private:
  FILE *stream_;
#ifndef _WIN32
  static struct timespec modification(const struct stat &value)
  {
#ifdef __APPLE__
    return value.st_mtimespec;
#else
    return value.st_mtim;
#endif
  }
  static struct timespec change(const struct stat &value)
  {
#ifdef __APPLE__
    return value.st_ctimespec;
#else
    return value.st_ctim;
#endif
  }
  struct stat status_ = {};
  char canonical_[PATH_MAX] = {};
#endif
};
} }
