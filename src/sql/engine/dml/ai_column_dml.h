/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#pragma once
#include "sql/engine/dml/ob_dml_ctx_define.h"

namespace oceanbase { namespace sql {
class ObTableModifyOp;
class AiColumnDml
{
public:
  static int before(const ObDMLBaseCtDef &definition, ObTableModifyOp &op, bool inserting);
  static int after(const ObDMLBaseCtDef &definition, ObTableModifyOp &op, int event);
};
} }
