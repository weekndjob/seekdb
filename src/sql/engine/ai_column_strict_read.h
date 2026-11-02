/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#pragma once
namespace oceanbase { namespace sql {
class ObExecContext;
class ObPhysicalPlan;
class AiColumnStrictRead
{
public:
  static int check(ObExecContext &context, const ObPhysicalPlan &plan);
};
} }
