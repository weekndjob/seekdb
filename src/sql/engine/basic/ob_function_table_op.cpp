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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/engine/basic/file_value_conversion.h"
#include "sql/engine/basic/file_scan_definition.h"
#include "sql/engine/basic/file_jsonl_reader.h"
#include "sql/engine/basic/file_glob_reader.h"
#include "sql/engine/basic/file_parquet_reader.h"
#include "pl/ob_pl_user_type.h"


namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_,
                    file_scan_, file_column_indexes_, file_scan_definition_);

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (MY_SPEC.file_scan_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_file;
    ret = open_file_scan();
    if (ret != OB_SUCCESS) { close_file_scan(); }
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else if (MY_SPEC.file_scan_) {
    close_file_scan();
    ret = open_file_scan();
  } else {
    node_idx_ = 0;
    if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  close_file_scan();
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  return ret;
}

void ObFunctionTableOp::destroy()
{
  inner_close();
  file_row_allocator_.reset();
  file_scan_allocator_.reset();
  ObOperator::destroy();
}

void ObFunctionTableOp::close_file_scan()
{
  if (file_reader_) { file_reader_->~CsvRecordReader(); file_reader_ = nullptr; }
  if (json_reader_) { json_reader_->~JsonlRecordReader(); json_reader_ = nullptr; }
  if (glob_reader_) { glob_reader_->~FileGlobReader(); glob_reader_ = nullptr; }
  if (parquet_reader_) { parquet_reader_->~FileParquetReader(); parquet_reader_ = nullptr; }
  if (file_definition_) { file_definition_->~FileScanDefinition(); file_definition_ = nullptr; }
  file_source_.close();
  file_scan_allocator_.reuse();
  file_row_allocator_.reuse();
}

int ObFunctionTableOp::open_file_scan()
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  ObString root;
  ObDatum *path = nullptr;
  ObSQLSessionInfo *session = ctx_.get_my_session();
  if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(session->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
  } else if (!enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL is disabled");
  } else if (OB_FAIL(session->get_secure_file_priv(root))) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, path))) {
  } else if (OB_ISNULL(path) || path->is_null()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    void *memory = file_scan_allocator_.alloc(sizeof(FileScanDefinition));
    if (memory == nullptr) { return OB_ALLOCATE_MEMORY_FAILED; }
    file_definition_ = new (memory) FileScanDefinition();
    if (OB_FAIL(file_definition_->decode(MY_SPEC.file_scan_definition_))) { return ret; }
    node_idx_ = 0;
    file_column_count_ = file_definition_->describe_ ? 6 : file_definition_->columns_.count();
    file_row_allocator_.reuse();
    if (file_definition_->format_ == FileScanDefinition::GLOB) {
      memory = file_scan_allocator_.alloc(sizeof(FileGlobReader));
      if (!memory) { return OB_ALLOCATE_MEMORY_FAILED; }
      glob_reader_ = new (memory) FileGlobReader();
      return glob_reader_->open(root, path->get_string());
    }
    if (OB_FAIL(file_source_.open(root, path->get_string()))) { return ret; }
    if (OB_FAIL(file_source_.matches_identity(file_definition_->identity_))) { return ret; }
    if (file_definition_->describe_) { return file_source_.check_unchanged(); }
    if (file_definition_->format_ == FileScanDefinition::CSV) {
      file_reader_ = new (file_reader_storage_) CsvRecordReader(file_source_.stream(), file_row_allocator_,
          16*1024*1024, 64*1024*1024, 1024, file_definition_->delimiter_,
          file_definition_->quote_, file_definition_->escape_,
          file_definition_->header_ ? ObString() : file_definition_->null_text_);
      if (file_definition_->header_) {
        ObArray<CsvField> fields;
        if (OB_FAIL(file_reader_->next(fields))) {
        } else if (fields.count() != file_column_count_) { ret = OB_INVALID_DATA; }
        file_reader_->set_null_text(file_definition_->null_text_);
      }
    } else if (file_definition_->format_ == FileScanDefinition::JSONL) {
      memory = file_scan_allocator_.alloc(sizeof(JsonlRecordReader));
      if (!memory) { ret = OB_ALLOCATE_MEMORY_FAILED; }
      else { json_reader_ = new (memory) JsonlRecordReader(file_source_.stream(), file_row_allocator_); }
    } else if (file_definition_->format_ == FileScanDefinition::PARQUET) {
      memory = file_scan_allocator_.alloc(sizeof(FileParquetReader));
      if (!memory) { ret = OB_ALLOCATE_MEMORY_FAILED; }
      else {
        parquet_reader_ = new (memory) FileParquetReader();
        ret = parquet_reader_->open(file_source_, &MY_SPEC.file_column_indexes_);
      }
    } else { ret = OB_NOT_SUPPORTED; }
    if (OB_SUCC(ret)) {
      ret = file_source_.check_unchanged();
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_file()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  file_row_allocator_.reuse();
  ObArray<CsvField> fields;
  if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_ISNULL(file_definition_)
             || (!file_definition_->describe_ && file_reader_ == nullptr && json_reader_ == nullptr
                 && glob_reader_ == nullptr && parquet_reader_ == nullptr)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!glob_reader_ && OB_FAIL(file_source_.check_unchanged())) {
  } else {
    if (file_definition_->describe_) {
      if (node_idx_ >= file_definition_->columns_.count()) { ret = OB_ITER_END; }
      else {
        CsvField description[] = {
          {file_definition_->columns_.at(node_idx_), true},
          {ObString::make_string(FileValueConversion::type_name(static_cast<ObObjType>(file_definition_->types_.at(node_idx_)))), true},
          {ObString::make_string(file_definition_->nullable_.at(node_idx_) ? "YES" : "NO"), true},
          {ObString(), false}, {ObString(), false}, {ObString(), false}};
        for (const CsvField &field : description) { if (OB_FAIL(fields.push_back(field))) { break; } }
        ++node_idx_;
      }
    } else if (glob_reader_) {
      ObString name;
      if (OB_FAIL(glob_reader_->next(file_row_allocator_, name))) {
      } else { ret = fields.push_back(CsvField{name, true}); }
    } else if (parquet_reader_) { ret = parquet_reader_->next(file_row_allocator_, fields); }
    else if (file_reader_) { ret = file_reader_->next(fields); }
    else {
      ObArray<JsonFileField> record;
      if (OB_FAIL(json_reader_->next(record))) {
      } else if (OB_FAIL(fields.prepare_allocate(file_column_count_))) {
      } else {
        for (int64_t i = 0; i < file_column_count_; ++i) { fields.at(i) = CsvField{ObString(), false}; }
        for (int64_t i = 0; OB_SUCC(ret) && i < record.count(); ++i) {
          int64_t index = -1;
          for (int64_t j = 0; j < file_column_count_; ++j) {
            if (record.at(i).name_.case_compare(file_definition_->columns_.at(j)) == 0) { index = j; break; }
          }
          if (index < 0) { ret = OB_INVALID_DATA; }
          else { fields.at(index) = record.at(i).field_; }
        }
      }
    }
    if (ret == OB_SUCCESS && fields.count() != file_column_count_) { ret = OB_INVALID_DATA; }
    if (!glob_reader_ && (ret == OB_SUCCESS || ret == OB_ITER_END)) {
      const int check = file_source_.check_unchanged();
      if (check != OB_SUCCESS) { ret = check; }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
      const int64_t column = MY_SPEC.file_column_indexes_.at(i);
      if (column < 0 || column >= fields.count()) {
        ret = OB_INVALID_DATA;
      } else {
        ObExpr *expr = MY_SPEC.column_exprs_.at(i);
        ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
        const CsvField &field = fields.at(column);
        ObObj object;
        if (OB_FAIL(FileValueConversion::convert(field, expr->obj_meta_.get_type(), object))) {
        } else if (OB_FAIL(datum.from_obj(object, expr->obj_datum_map_))) {
        } else { expr->set_evaluated_projected(eval_ctx_); }
      }
    }
  }
  return ret;
}


int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}


} // end namespace sql
} // end namespace oceanbase
