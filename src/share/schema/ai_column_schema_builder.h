/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#pragma once
#include "share/schema/ai_column_definition.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_table_schema.h"

namespace oceanbase { namespace share { namespace schema {
class AiColumnSchemaBuilder
{
public:
  // Called only after the result column has its final physical column ID.
  // ALTER callers persist the returned auxiliary columns in the same DDL
  // transaction; CREATE callers include them in their ordinary schema insert.
  static int complete(ObTableSchema &table, ObColumnSchemaV2 &result,
                      common::ObIAllocator &arena,
                      common::ObIArray<ObColumnSchemaV2> *added = nullptr)
  {
    using namespace common;
    if (!result.is_ai_column()) { return OB_SUCCESS; }
    AiColumnDefinition definition;
    int ret = definition.decode(result.get_ai_column_info(), false);
    if (ret != OB_SUCCESS) { return ret; }
    if (definition.state_column_id_ != 0) { return definition.validate(); }
    if (definition.model_.empty() || definition.input_expr_.empty() || definition.result_type_.empty()
        || result.get_column_id() == OB_INVALID_ID || result.get_column_id() < OB_APP_MIN_COLUMN_ID) {
      return OB_INVALID_ARGUMENT;
    }
    // ALTER assigns final user IDs in the DDL service. Resolve dependencies by
    // their schema names after every user column in the action has been added.
    for (int64_t i = 0; i < definition.dependencies_.count(); ++i) {
      const auto *dependency = table.get_column_schema(definition.dependency_names_.at(i));
      if (!dependency || dependency->is_ai_column() || dependency->is_hidden()
          || dependency->is_generated_column()) { return OB_NOT_SUPPORTED; }
      definition.dependencies_.at(i) = dependency->get_column_id();
    }
    // Keep the serialized ID/name pairs ordered together after ALTER remapping.
    for (int64_t i = 1; i < definition.dependencies_.count(); ++i) {
      for (int64_t j = i; j > 0 && definition.dependencies_.at(j) < definition.dependencies_.at(j-1); --j) {
        std::swap(definition.dependencies_.at(j), definition.dependencies_.at(j-1));
        std::swap(definition.dependency_names_.at(j), definition.dependency_names_.at(j-1));
      }
    }
    uint64_t next = table.get_max_used_column_id();
    for (auto it = table.column_begin(); it != table.column_end(); ++it) {
      if (!*it) { return OB_ERR_UNEXPECTED; }
      next = std::max(next, (*it)->get_column_id());
    }
    next = std::max(next, result.get_column_id());
    if (next > OB_MAX_TMP_COLUMN_ID - 3) { return OB_SIZE_OVERFLOW; }
    const char *suffixes[] = {"state", "request", "spec"};
    uint64_t ids[3] = {};
    for (int64_t i = 0; ret == OB_SUCCESS && i < 3; ++i) {
      ObColumnSchemaV2 auxiliary;
      ObSqlString name;
      if (OB_FAIL(name.append_fmt("__ai_%lu_%s", result.get_column_id(), suffixes[i]))) { break; }
      if (table.get_column_schema(name.string()) != nullptr) { return OB_ERR_COLUMN_DUPLICATE; }
      auxiliary.set_table_id(table.get_table_id());
      auxiliary.set_schema_version(result.get_schema_version());
      auxiliary.set_column_id(++next);
      ids[i] = next;
      auxiliary.set_data_type(ObVarcharType);
      auxiliary.set_data_length(i == 0 ? 16 : 64);
      auxiliary.set_charset_type(CHARSET_UTF8MB4);
      auxiliary.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      auxiliary.set_nullable(true);
      auxiliary.set_is_hidden(true);
      auxiliary.add_column_flag(AI_COLUMN_AUX_FLAG);
      if (i == 0) {
        ObObj pending;
        pending.set_varchar("PENDING");
        pending.set_collation_type(CS_TYPE_UTF8MB4_BIN);
        if (OB_FAIL(auxiliary.set_cur_default_value(pending, false))) { break; }
        if (OB_FAIL(auxiliary.set_orig_default_value(pending))) { break; }
      }
      if (OB_FAIL(auxiliary.set_column_name(name.string()))) {
      } else if (OB_FAIL(table.add_column(auxiliary))) {
      } else if (added) { ret = added->push_back(auxiliary); }
    }
    if (ret != OB_SUCCESS) { return ret; }
    table.set_max_used_column_id(next);
    definition.state_column_id_ = ids[0];
    definition.request_column_id_ = ids[1];
    definition.result_spec_column_id_ = ids[2];
    // The version is scoped by table ID and final result-column ID. MODIFY
    // preserves those IDs and must advance the previous definition's version.
    if (definition.definition_version_ == 0) { definition.definition_version_ = 1; }
    ObString packet;
    if (OB_FAIL(definition.encode(arena, packet))) { return ret; }
    if (OB_FAIL(result.set_ai_column_info(packet))) { return ret; }
    ObColumnSchemaV2 *stored = table.get_column_schema(result.get_column_id());
    if (!stored) { return OB_ERR_UNEXPECTED; }
    if (stored != &result) { ret = stored->set_ai_column_info(packet); }
    return ret;
  }
};
} } }
