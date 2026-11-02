/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SERVER
#include "config_bridge.h"
#include "share/config/ob_config_helper.h"
#include "ob_server_duty_task.h"
#include "share/rc/ob_server_runtime.h"
#include "sql/engine/ob_sql_memory_manager.h"
#include "observer/omt/ob_server_runtime.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "lib/string/ob_sql_string.h"
#include <string>
#include <vector>

using namespace oceanbase::common;

namespace oceanbase {
using namespace share;
using namespace share::schema;
using namespace sql;
namespace observer {

ObServerDutyTask::ObServerDutyTask()
  : allocator_(ObModIds::OB_DUTY_TASK)
{
}

void ObServerDutyTask::runTimerTask()
{
  allocator_.reset_remain_one_page();
  update_runtime_settings();
}

int ObServerDutyTask::schedule(common::ObTimer &timer)
{
  return timer.schedule(*this, SCHEDULE_PERIOD, true);
}

void ObServerDutyTask::update_runtime_settings()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(update_ctx_memory_throttle())) {
  }
}

int ObServerDutyTask::update_ctx_memory_throttle()
{
  int ret = OB_SUCCESS;
  {
    ObCtxMemoryLimitChecker checker;
    uint64_t ctx_id = 0;
    int64_t limit = 0;
    ObMallocAllocator *alloc = ObMallocAllocator::get_instance();
    rust::String limits = config::_ctx_memory_limit();
    if (!checker.check(limits.c_str(), ctx_id, limit)) {
      // do nothing
    } else {
      if (limits.empty()) {
        ctx_id = ObCtxIds::MAX_CTX_ID;
        limit = INT64_MAX; // empty str means no limit, and not care ctx_id.
      }
      for (int i = 0; i < ObCtxIds::MAX_CTX_ID; i++) {
        if (ObCtxIds::WORK_AREA == i ||
            ObCtxIds::META_OBJ_CTX_ID == i) {
          continue;
        }
        auto allocator = alloc->get_ctx_allocator(i);
        if (OB_NOT_NULL(allocator)) {
          if (OB_FAIL(allocator->set_limit(ctx_id == i ? limit : INT64_MAX))) {
          }
        }
      }
    }
  }
  return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////

int ObSqlMemoryTimerTask::schedule(common::ObTimer &timer)
{
  return timer.schedule(*this, SCHEDULE_PERIOD, true);
}

void ObSqlMemoryTimerTask::runTimerTask()
{
  int ret = OB_SUCCESS;
  {
    SERVER_MODULE_SCOPE {
      ObSqlMemoryManager *sql_mem_mgr = ::oceanbase::share::server_service<::oceanbase::sql::ObSqlMemoryManager>();
      if (OB_UNLIKELY(nullptr == sql_mem_mgr)) {
      } else if (OB_FAIL(sql_mem_mgr->calculate_global_bound_size())) {
      }
    }
  }
}

// Implementation staged for the seekdb server's AI column timer task.

namespace {

struct AiColumnWorkItem
{
  uint64_t table_id = 0;
  uint64_t column_id = 0;
  uint64_t request_id = 0;
  int64_t retry_count = 0;
  int64_t max_retries = 0;
  std::string rowkey;
  std::string database;
  std::string table;
  std::string column;
  std::string expression;
};

std::string ai_copy_string(const ObString &value)
{
  return std::string(value.ptr() == nullptr ? "" : value.ptr(), value.length());
}

std::string ai_typed_expression(const ObString &value)
{
  std::string expression = ai_copy_string(value);
  const char *system_prompt = nullptr;
  if (expression.find("Return exactly one valid JSON value") != std::string::npos) {
    system_prompt = "Return exactly one valid JSON value";
  } else if (expression.find("Return exactly one base-10 integer") != std::string::npos) {
    system_prompt = "Return exactly one base-10 integer";
  }
  if (system_prompt != nullptr) {
    const size_t comment = expression.find("/*");
    const size_t close = expression.rfind(')', comment);
    if (close != std::string::npos) {
      expression.insert(close, std::string(", JSON_OBJECT('__ai_column_system_prompt','")
                                + system_prompt + "')");
    }
  }
  return expression;
}

int ai_append_identifier(ObSqlString &sql, const std::string &name)
{
  int ret = sql.append("`");
  for (size_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    ret = sql.append(name[i] == '`' ? "``" : std::string(1, name[i]).c_str());
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("`");
  }
  return ret;
}

int ai_append_table(ObSqlString &sql, const AiColumnWorkItem &item)
{
  int ret = ai_append_identifier(sql, item.database);
  if (OB_SUCC(ret)) {
    ret = sql.append(".");
  }
  if (OB_SUCC(ret)) {
    ret = ai_append_identifier(sql, item.table);
  }
  return ret;
}

int ai_find_request(ObMySQLProxy &proxy, uint64_t &request_id)
{
  int ret = OB_SUCCESS;
  request_id = 0;
  SMART_VAR(ObMySQLProxy::MySQLResult, result) {
    if (OB_FAIL(proxy.read(result,
        "select request_id from oceanbase.__all_ai_column_task "
        "where ((state in (0,3) and eligible_ts <= now(6)) "
        "or (state=1 and lease_until < now(6))) "
        "order by eligible_ts, request_id limit 1"))) {
    } else if (result.get_result() == nullptr) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(result.get_result()->next())) {
      if (ret == OB_ITER_END) {
        ret = OB_SUCCESS;
      }
    } else {
      int64_t value = 0;
      if (OB_FAIL(result.get_result()->get_int("request_id", value))) {
      } else {
        request_id = static_cast<uint64_t>(value);
      }
    }
  }
  return ret;
}

int ai_load_group(ObMySQLProxy &proxy, const uint64_t request_id,
                  std::vector<AiColumnWorkItem> &items)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt(
      "select task.table_id,task.column_id,task.request_id,task.rowkey_predicate,"
      "task.retry_count,def.max_retries,def.column_name,def.normalized_expr,"
      "t.table_name,d.database_name "
      "from oceanbase.__all_ai_column_task task "
      "join oceanbase.__all_ai_column def on def.table_id=task.table_id "
      "and def.column_id=task.column_id and def.definition_version=task.definition_version "
      "join oceanbase.__all_table t on t.table_id=task.table_id "
      "join oceanbase.__all_database d on d.database_id=t.database_id "
      "where task.request_id=%lu order by task.column_id", request_id))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, result) {
      if (OB_FAIL(proxy.read(result, sql.ptr()))) {
      } else if (result.get_result() == nullptr) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        int next_ret = OB_SUCCESS;
        while (OB_SUCC(ret) && OB_SUCCESS == (next_ret = result.get_result()->next())) {
          AiColumnWorkItem item;
          int64_t table_id = 0, column_id = 0, req_id = 0;
          ObString rowkey, database, table, column, expression;
          if (OB_FAIL(result.get_result()->get_int("table_id", table_id))) {
          } else if (OB_FAIL(result.get_result()->get_int("column_id", column_id))) {
          } else if (OB_FAIL(result.get_result()->get_int("request_id", req_id))) {
          } else if (OB_FAIL(result.get_result()->get_int("retry_count", item.retry_count))) {
          } else if (OB_FAIL(result.get_result()->get_int("max_retries", item.max_retries))) {
          } else if (OB_FAIL(result.get_result()->get_varchar("rowkey_predicate", rowkey))) {
          } else if (OB_FAIL(result.get_result()->get_varchar("database_name", database))) {
          } else if (OB_FAIL(result.get_result()->get_varchar("table_name", table))) {
          } else if (OB_FAIL(result.get_result()->get_varchar("column_name", column))) {
          } else if (OB_FAIL(result.get_result()->get_varchar("normalized_expr", expression))) {
          } else {
            item.table_id = static_cast<uint64_t>(table_id);
            item.column_id = static_cast<uint64_t>(column_id);
            item.request_id = static_cast<uint64_t>(req_id);
            item.rowkey = ai_copy_string(rowkey);
            item.database = ai_copy_string(database);
            item.table = ai_copy_string(table);
            item.column = ai_copy_string(column);
            item.expression = ai_typed_expression(expression);
            items.push_back(item);
          }
        }
        if (OB_SUCC(ret) && next_ret != OB_ITER_END) {
          ret = next_ret;
        }
      }
    }
  }
  return ret;
}

int ai_claim_group(ObMySQLProxy &proxy, const uint64_t request_id,
                   const int64_t expected_rows, bool &claimed)
{
  int ret = OB_SUCCESS;
  int64_t affected = 0;
  ObSqlString sql;
  claimed = false;
  if (OB_FAIL(sql.assign_fmt(
      "update oceanbase.__all_ai_column_task set state=1,"
      "lease_until=date_add(now(6),interval 30 second) "
      "where request_id=%lu and ((state in (0,3) and eligible_ts<=now(6)) "
      "or (state=1 and lease_until<now(6)))", request_id))) {
  } else if (OB_FAIL(proxy.write(sql.ptr(), affected))) {
  } else {
    claimed = affected == expected_rows;
  }
  return ret;
}

int ai_append_guard(ObSqlString &sql, const std::vector<AiColumnWorkItem> &items)
{
  int ret = sql.append_fmt(" where (%s)", items[0].rowkey.c_str());
  for (size_t i = 0; OB_SUCC(ret) && i < items.size(); ++i) {
    ret = sql.append_fmt(" and `__ai_request_%lu`=%lu",
                         items[i].column_id, items[i].request_id);
  }
  return ret;
}

int ai_delete_group(ObMySQLTransaction &trans, const uint64_t request_id)
{
  int ret = OB_SUCCESS;
  int64_t affected = 0;
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt(
      "delete from oceanbase.__all_ai_column_task where request_id=%lu", request_id))) {
  } else if (OB_FAIL(trans.write(sql.ptr(), affected))) {
  }
  return ret;
}

int ai_apply_group(ObMySQLProxy &proxy, const std::vector<AiColumnWorkItem> &items)
{
  int ret = OB_SUCCESS;
  int64_t affected = 0;
  ObSqlString sql;
  ObMySQLTransaction trans;
  if (items.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(sql.append("update "))) {
  } else if (OB_FAIL(ai_append_table(sql, items[0]))) {
  } else if (OB_FAIL(sql.append(" set "))) {
  }
  for (size_t i = 0; OB_SUCC(ret) && i < items.size(); ++i) {
    if (i > 0 && OB_FAIL(sql.append(","))) {
    } else if (OB_FAIL(ai_append_identifier(sql, items[i].column))) {
    } else if (OB_FAIL(sql.append_fmt("=(%s),`__ai_state_%lu`=2",
                                     items[i].expression.c_str(), items[i].column_id))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(ai_append_guard(sql, items))) {
  } else if (OB_SUCC(ret) && OB_FAIL(trans.start(&proxy))) {
  } else if (OB_SUCC(ret) && OB_FAIL(trans.write(sql.ptr(), affected))) {
  } else if (OB_SUCC(ret) && OB_FAIL(ai_delete_group(trans, items[0].request_id))) {
  }
  if (trans.is_started()) {
    const int end_ret = trans.end(OB_SUCC(ret));
    if (OB_SUCC(ret) && end_ret != OB_SUCCESS) {
      ret = end_ret;
    }
  }
  return ret;
}

int ai_mark_failure(ObMySQLProxy &proxy, const std::vector<AiColumnWorkItem> &items)
{
  int ret = OB_SUCCESS;
  int64_t affected = 0;
  ObSqlString sql;
  if (items.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    bool can_retry = true;
    for (size_t i = 0; i < items.size(); ++i) {
      can_retry = can_retry && items[i].retry_count < items[i].max_retries;
    }
    if (can_retry) {
      if (OB_FAIL(sql.assign_fmt(
          "update oceanbase.__all_ai_column_task set state=3,"
          "retry_count=retry_count+1,lease_until=null,"
          "eligible_ts=date_add(now(6),interval 500000 microsecond) "
          "where request_id=%lu and state=1", items[0].request_id))) {
      } else if (OB_FAIL(proxy.write(sql.ptr(), affected))) {
      }
    } else {
      ObMySQLTransaction trans;
      if (OB_FAIL(sql.append("update "))) {
      } else if (OB_FAIL(ai_append_table(sql, items[0]))) {
      } else if (OB_FAIL(sql.append(" set "))) {
      }
      for (size_t i = 0; OB_SUCC(ret) && i < items.size(); ++i) {
        if (i > 0 && OB_FAIL(sql.append(","))) {
        } else if (OB_FAIL(sql.append_fmt("`__ai_state_%lu`=3", items[i].column_id))) {
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(ai_append_guard(sql, items))) {
      } else if (OB_SUCC(ret) && OB_FAIL(trans.start(&proxy))) {
      } else if (OB_SUCC(ret) && OB_FAIL(trans.write(sql.ptr(), affected))) {
      } else if (OB_SUCC(ret) && OB_FAIL(ai_delete_group(trans, items[0].request_id))) {
      }
      if (trans.is_started()) {
        const int end_ret = trans.end(OB_SUCC(ret));
        if (OB_SUCC(ret) && end_ret != OB_SUCCESS) {
          ret = end_ret;
        }
      }
    }
  }
  return ret;
}

} // namespace

int ObAiColumnWorkerTask::schedule(common::ObTimer &timer)
{
  return timer.schedule(*this, SCHEDULE_PERIOD, true);
}

void ObAiColumnWorkerTask::runTimerTask()
{
  ObMySQLProxy *proxy = GCTX.sql_proxy_;
  if (proxy != nullptr) {
    uint64_t request_id = 0;
    int ret = ai_find_request(*proxy, request_id);
    if (OB_SUCC(ret) && request_id != 0) {
      std::vector<AiColumnWorkItem> items;
      if (OB_FAIL(ai_load_group(*proxy, request_id, items))) {
      } else if (items.empty()) {
        // The definition or table disappeared while a task was pending.
        int64_t affected = 0;
        ObSqlString sql;
        if (OB_FAIL(sql.assign_fmt("delete from oceanbase.__all_ai_column_task "
                                   "where request_id=%lu", request_id))) {
        } else if (OB_FAIL(proxy->write(sql.ptr(), affected))) {
        }
      } else {
        bool claimed = false;
        if (OB_FAIL(ai_claim_group(*proxy, request_id, items.size(), claimed))) {
        } else if (claimed) {
          const int apply_ret = ai_apply_group(*proxy, items);
          if (apply_ret != OB_SUCCESS) {
            LOG_WARN("AI column work failed", K(apply_ret), K(request_id));
            if (OB_FAIL(ai_mark_failure(*proxy, items))) {
            }
          }
        }
      }
    }
    if (ret != OB_SUCCESS) {
      LOG_WARN("AI column worker iteration failed", K(ret), K(request_id));
    }
  }
}

}  // observer
}  // oceanbase
