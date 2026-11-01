/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#pragma once
#include <cerrno>
#include "sql/engine/basic/file_csv_table.h"
#ifndef _WIN32
#include <dirent.h>
#include <fnmatch.h>
#endif

namespace oceanbase { namespace sql {
// File discovery streams directory entries; it never reads or buffers file data.
// Wildcards apply to the final path component, within a verified directory.
class FileGlobReader
{
public:
  FileGlobReader() = default;
  FileGlobReader(const FileGlobReader &) = delete;
  FileGlobReader &operator=(const FileGlobReader &) = delete;
  ~FileGlobReader() { close(); }
  void close()
  {
#ifndef _WIN32
    if (entries_) { ::closedir(entries_); entries_ = nullptr; }
#endif
    directory_.close();
  }
  int open(const common::ObString &root, const common::ObString &pattern)
  {
    using namespace common;
    close();
#ifdef _WIN32
    return OB_NOT_SUPPORTED;
#else
    if (root.empty() || root.length() >= PATH_MAX || pattern.empty() || pattern.length() >= PATH_MAX
        || std::memchr(root.ptr(), 0, root.length()) || std::memchr(pattern.ptr(), 0, pattern.length())) {
      return OB_INVALID_ARGUMENT;
    }
    std::memcpy(root_, root.ptr(), root.length()); root_[root.length()] = 0;
    std::memcpy(pattern_, pattern.ptr(), pattern.length()); pattern_[pattern.length()] = 0;
    char *slash = std::strrchr(pattern_, '/');
    const char *parent = ".";
    const char *name = pattern_;
    if (slash) {
      *slash = 0;
      name = slash+1;
      parent = slash == pattern_ ? "/" : pattern_;
    }
    if (*name == 0) { return OB_INVALID_ARGUMENT; }
    std::strcpy(match_, name);
    int ret = directory_.open(root, ObString::make_string(parent), true);
    if (ret != OB_SUCCESS) { return ret; }
    const int fd = ::dup(::fileno(directory_.stream()));
    if (fd < 0) { return OB_IO_ERROR; }
    entries_ = ::fdopendir(fd);
    if (!entries_) { ::close(fd); return OB_IO_ERROR; }
    return OB_SUCCESS;
#endif
  }
  int next(common::ObIAllocator &arena, common::ObString &path)
  {
    using namespace common;
#ifdef _WIN32
    return OB_NOT_SUPPORTED;
#else
    if (!entries_) { return OB_NOT_INIT; }
    for (;;) {
      errno = 0;
      struct dirent *entry = ::readdir(entries_);
      if (!entry) { return errno ? OB_IO_ERROR : OB_ITER_END; }
      if (::fnmatch(match_, entry->d_name, FNM_PERIOD) != 0) { continue; }
      struct stat status = {};
      if (::fstatat(::dirfd(entries_), entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) { return OB_IO_ERROR; }
      if (!S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode)) { continue; }
      ObSqlString full;
      int ret = full.append(directory_.canonical_path());
      if (ret == OB_SUCCESS) { ret = full.append("/"); }
      if (ret == OB_SUCCESS) { ret = full.append(entry->d_name); }
      if (ret != OB_SUCCESS) { return ret; }
      CsvFileTable candidate;
      ret = candidate.open(ObString::make_string(root_), full.string());
      if (ret == OB_ERR_NO_PRIVILEGE || ret == OB_FILE_NOT_EXIST || ret == OB_INVALID_ARGUMENT) { continue; }
      if (ret != OB_SUCCESS) { return ret; }
      return ob_write_string(arena, full.string(), path);
    }
#endif
  }
private:
  CsvFileTable directory_;
#ifndef _WIN32
  DIR *entries_ = nullptr;
  char root_[PATH_MAX] = {};
  char pattern_[PATH_MAX] = {};
  char match_[PATH_MAX] = {};
#endif
};
} }
