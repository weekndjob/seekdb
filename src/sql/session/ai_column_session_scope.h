/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#pragma once
#include "sql/session/ob_sql_session_info.h"
namespace oceanbase { namespace sql {
class AiColumnSessionScope
{
public:
  explicit AiColumnSessionScope(ObSQLSessionInfo &session,
      const common::ObString &system_prompt = common::ObString())
      : session_(session), internal_(session.is_ai_column_internal()),
        prompt_(session.get_ai_column_system_prompt()),
        plan_cache_(session.get_local_ob_enable_plan_cache())
  { session_.set_local_ob_enable_plan_cache(false);
    session_.set_ai_column_internal(true); session_.set_ai_column_system_prompt(system_prompt); }
  ~AiColumnSessionScope()
  { session_.set_ai_column_system_prompt(prompt_); session_.set_ai_column_internal(internal_);
    session_.set_local_ob_enable_plan_cache(plan_cache_); }
private:
  ObSQLSessionInfo &session_;
  bool internal_;
  common::ObString prompt_;
  bool plan_cache_;
  DISALLOW_COPY_AND_ASSIGN(AiColumnSessionScope);
};
} }
