/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#pragma once
#include "share/ob_thread_pool.h"
#include "common/mysqlclient/ob_mysql_proxy.h"

namespace oceanbase { namespace observer {
class AiColumnWorker : public share::ObThreadPool
{
public:
  AiColumnWorker() : proxy_(nullptr), reconcile_table_(0), reconcile_column_(0) {}
  ~AiColumnWorker() { stop(); wait(); }
  int initialize(common::ObMySQLProxy &proxy);
private:
  void run1() override;
  int process_group();
  int reconcile_added_columns();
  common::ObMySQLProxy *proxy_;
  uint64_t reconcile_table_;
  uint64_t reconcile_column_;
};
} }
