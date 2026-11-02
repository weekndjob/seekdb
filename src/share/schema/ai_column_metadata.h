/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#pragma once
#include "share/schema/ai_column_definition.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "share/ob_dml_sql_splicer.h"
#include "common/mysqlclient/ob_mysql_proxy.h"

namespace oceanbase { namespace share { namespace schema {
class AiColumnMetadata
{
public:
  static int store(common::ObISQLClient &client, const ObColumnSchemaV2 &column,
                   int64_t schema_version, bool history_only = false)
  {
    using namespace common;
    if (!column.is_ai_column()) { return OB_SUCCESS; }
    AiColumnDefinition definition;
    int ret = definition.decode(column.get_ai_column_info(), false);
    if (ret == OB_SUCCESS && definition.state_column_id_ == 0) { return OB_SUCCESS; }
    if (ret == OB_SUCCESS) { ret = definition.validate(); }
    ObDMLSqlSplicer row;
    ObDMLExecHelper executor(client);
    int64_t affected = 0;
    if (ret != OB_SUCCESS) { return ret; }
    if (OB_FAIL(row.add_pk_column("table_id", column.get_table_id()))) {
    } else if (OB_FAIL(row.add_pk_column("column_id", column.get_column_id()))) {
    } else if (OB_FAIL(row.add_column("column_name", ObHexEscapeSqlStr(column.get_column_name_str())))) {
    } else if (OB_FAIL(row.add_column("schema_version", schema_version))) {
    } else if (OB_FAIL(row.add_column("definition_version", definition.definition_version_))) {
    } else if (OB_FAIL(row.add_column("normalized_expr", ObHexEscapeSqlStr(definition.normalized_expr_)))) {
    } else if (OB_FAIL(row.add_column("definition", ObHexEscapeSqlStr(column.get_ai_column_info())))) {
    } else if (OB_FAIL(row.add_column("max_retries", definition.max_retries_))) {
    } else if (OB_FAIL(row.add_column("sync_mode", "async"))) {
    } else if (OB_FAIL(row.add_column("state_column_id", definition.state_column_id_))) {
    } else if (OB_FAIL(row.add_column("request_column_id", definition.request_column_id_))) {
    } else if (OB_FAIL(row.add_column("result_spec_column_id", definition.result_spec_column_id_))) {
    } else if (!history_only && OB_FAIL(executor.exec_replace("oceanbase.__all_ai_column", row, affected))) {
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(row.add_column("is_deleted", 0))) {
      } else { ret = executor.exec_replace("oceanbase.__all_ai_column_history", row, affected); }
    }
    return ret;
  }

  static int store_table(common::ObISQLClient &client, const ObTableSchema &table)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    if (table.is_index_table() || table.is_aux_lob_table()) { return ret; }
    for (auto it = table.column_begin(); ret == OB_SUCCESS && it != table.column_end(); ++it) {
      if (!*it) { return OB_ERR_UNEXPECTED; }
      if (!(*it)->is_ai_column()) { continue; }
      ObColumnSchemaV2 column;
      if (OB_FAIL(column.assign(**it))) {
      } else {
        column.set_table_id(table.get_table_id());
        ret = store(client, column, table.get_schema_version());
      }
    }
    return ret;
  }
  static int remove(common::ObISQLClient &client, uint64_t table, uint64_t column, int64_t version)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    ObDMLSqlSplicer row;
    ObDMLExecHelper executor(client);
    int64_t affected = 0;
    if (OB_FAIL(row.add_pk_column("table_id", table))) {
    } else if (OB_FAIL(row.add_pk_column("column_id", column))) {
    } else if (OB_FAIL(executor.exec_delete("oceanbase.__all_ai_column", row, affected))) {
    } else if (OB_FAIL(executor.exec_delete("oceanbase.__all_ai_column_task", row, affected))) {
    } else if (OB_FAIL(row.add_pk_column("schema_version", version))) {
    } else if (OB_FAIL(row.add_column("is_deleted", 1))) {
    } else { ret = executor.exec_replace("oceanbase.__all_ai_column_history", row, affected); }
    return ret;
  }

  // AI flags are stored in __all_column, so ordinary table schema refreshes
  // never query an AI metadata table. Historical schema loads use matching
  // historical definitions rather than silently attaching the current rule.
  static int load(common::ObISQLClient &client, ObTableSchema &table, int64_t version)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    if (table.is_index_table() || table.is_aux_lob_table()) { return ret; }
    for (auto it = table.column_begin(); ret == OB_SUCCESS && it != table.column_end(); ++it) {
      ObColumnSchemaV2 *column = *it;
      if (!column) { return OB_ERR_UNEXPECTED; }
      if (!column->is_ai_column()) { continue; }
      ObSqlString sql;
      ObMySQLProxy::MySQLResult result;
      ObString packet;
      int64_t deleted = 0;
      if (OB_FAIL(sql.append_fmt("SELECT definition,is_deleted FROM oceanbase.__all_ai_column_history "
                    "WHERE table_id=%lu AND column_id=%lu AND schema_version<=%ld "
                    "ORDER BY schema_version DESC LIMIT 1", table.get_table_id(), column->get_column_id(), version))) {
      } else if (OB_FAIL(client.read(result, sql.ptr()))) {
      } else if (!result.get_result()) { ret = OB_ERR_UNEXPECTED;
      } else if ((ret = result.get_result()->next()) == OB_ITER_END) {
        // CREATE/ALTER can refresh the column schema before the same DDL
        // transaction writes its AI metadata row. The writer still holds the
        // complete definition; a later committed refresh loads the history.
        ret = OB_SUCCESS;
      } else if (OB_FAIL(ret)) {
      } else if (OB_FAIL(result.get_result()->get_int("is_deleted", deleted))) {
      } else if (deleted != 0) { ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result.get_result()->get_varchar("definition", packet))) {
      } else if (OB_FAIL(column->set_ai_column_info(packet))) {
      } else {
        AiColumnDefinition check;
        ret = check.decode(column->get_ai_column_info());
      }
    }
    return ret;
  }
};
} } }
