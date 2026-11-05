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

#include "ob_table_modify_op.h"
#include "storage/access/ob_dml_param.h"
#include "storage/ob_table_dml_param.h"
#include "storage/tx_storage/ob_access_service.h"
#include "storage/ob_value_row_iterator.h"
#include "storage/blocksstable/ob_datum_row.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "storage/ob_common_id_utils.h"
#include "share/ob_autoincrement_service.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/engine/dml/ob_dml_service.h"

namespace oceanbase
{
using namespace common;
using namespace common::sqlclient;
using namespace storage;
using namespace share;
using namespace share::schema;
using namespace observer;
namespace sql
{
int ForeignKeyHandle::do_handle(ObTableModifyOp &op,
                                const ObDMLBaseCtDef &dml_ctdef,
                                ObDMLBaseRtDef &dml_rtdef)
{
  int ret = OB_SUCCESS;
  if (op.need_foreign_key_checks()) {
    const ObExprPtrIArray &old_row = dml_ctdef.old_row_;
    const ObExprPtrIArray &new_row = dml_ctdef.new_row_;
    const bool save_in_ignore_cascading = op.get_exec_ctx().get_das_ctx().in_ignore_cascading_;
    op.get_exec_ctx().get_das_ctx().is_fk_cascading_ = true;
    op.get_exec_ctx().get_das_ctx().in_ignore_cascading_ = dml_ctdef.das_base_ctdef_.is_ignore_;
    if (OB_FAIL(op.check_stack())) {
    }
    for (int i = 0; OB_SUCC(ret) && i < dml_ctdef.fk_args_.count(); i++) {
      const ObForeignKeyArg &fk_arg = dml_ctdef.fk_args_.at(i);
      ObForeignKeyChecker *fk_checker = dml_rtdef.fk_checker_array_.at(i);
      if (OB_SUCC(ret) && !new_row.empty()) {
        if (ACTION_CHECK_EXIST == fk_arg.ref_action_) {
          // insert or update.
          bool is_foreign_key_cascade = ObSQLUtils::is_fk_nested_sql(&op.get_exec_ctx());
          bool in_ignore_cascading = false;
          ObExecContext* parent_ctx = op.get_exec_ctx().get_parent_ctx();
          if (OB_NOT_NULL(parent_ctx)) {
            in_ignore_cascading = parent_ctx->get_das_ctx().in_ignore_cascading_;
          }

          if (is_foreign_key_cascade && !in_ignore_cascading) {
            // nested update can not check parent row.
          } else if (OB_FAIL(check_exist(op, fk_arg, new_row, fk_checker, false, fk_arg.use_das_scan_))) {
          }
        }
      }
      if (OB_SUCC(ret) && !old_row.empty()) {
        if (ACTION_RESTRICT == fk_arg.ref_action_ || ACTION_NO_ACTION == fk_arg.ref_action_) {
          // update or delete.
          bool has_changed = false;
          if (OB_FAIL(value_changed(op, fk_arg.columns_, old_row, new_row, has_changed))) {
          } else if (!has_changed) {
            // nothing.
          } else if (OB_FAIL(check_exist(op, fk_arg, old_row, fk_checker, true, fk_arg.use_das_scan_))) {
          }
        } else if (ACTION_CASCADE == fk_arg.ref_action_) {
          // update or delete.
          bool is_self_ref = false;
          if (OB_FAIL(is_self_ref_row(op.get_eval_ctx(), old_row, fk_arg, is_self_ref))) {
          } else if (new_row.empty() && is_self_ref && op.is_fk_nested_session()) {
            // delete self refercnced row should not cascade delete.
          } else if (OB_FAIL(check_exist_inner_sql(op, fk_arg, old_row, true, true))) {
          } else if (OB_FAIL(cascade(op, fk_arg, old_row, new_row))) {
          } else if (!new_row.empty() && is_self_ref) {
            // we got here only when:
            //  1. handling update operator and
            //  2. foreign key constraint is self reference
            // need to change %new_row and %old_row
            //   (see 
            // xxx_row_res_info helps to restore row. restore row before get_next_row()
            //op.fk_self_ref_row_res_infos_.reset();
            for (int64_t i = 0; OB_SUCC(ret) && i < fk_arg.columns_.count(); i++) {
              const int32_t val_idx = fk_arg.columns_.at(i).idx_;
              const int32_t name_idx = fk_arg.columns_.at(i).name_idx_;
              if (OB_SUCC(ret)) {
                const ObDatum &updated_value = new_row.at(val_idx)->
                                                    locate_expr_datum(op.get_eval_ctx());
                ObDatum &new_row_name_col = new_row.at(name_idx)->
                                                  locate_expr_datum(op.get_eval_ctx());
                ObDatum &old_row_name_col = old_row.at(name_idx)->
                                                  locate_expr_datum(op.get_eval_ctx());

                OX(new_row_name_col = updated_value);
                OX(old_row_name_col = updated_value);
              }
            }
          }
        } else if (ACTION_SET_NULL == fk_arg.ref_action_) {
          if (OB_FAIL(check_exist_inner_sql(op, fk_arg, old_row, true, true))) {
          } else if (OB_FAIL(set_null(op, fk_arg, old_row))) {
          }
        }
      } // if (old_row.is_valid())
    } // for
    //fk cascading end
    op.get_exec_ctx().get_das_ctx().is_fk_cascading_ = false;
    op.get_exec_ctx().get_das_ctx().in_ignore_cascading_ = save_in_ignore_cascading;
  } else {
  }
  return ret;
}

int ForeignKeyHandle::value_changed(ObTableModifyOp &op,
                                    const ObIArray<ObForeignKeyColumn> &columns,
                                    const ObExprPtrIArray &old_row,
                                    const ObExprPtrIArray &new_row,
                                    bool &has_changed)
{
  int ret = OB_SUCCESS;
  has_changed = false;
  if (!old_row.empty() && !new_row.empty()) {
    ObDatum *old_row_col = NULL;
    ObDatum *new_row_col = NULL;
    for (int64_t i = 0; OB_SUCC(ret) && !has_changed && i < columns.count(); ++i) {
      int64_t col_idx = columns.at(i).idx_;
      CK(col_idx < old_row.count());
      CK(col_idx < new_row.count());
      CK(OB_NOT_NULL(old_row.at(col_idx)));
      CK(OB_NOT_NULL(new_row.at(col_idx)));
      OZ(old_row.at(col_idx)->eval(op.get_eval_ctx(), old_row_col));
      OZ(new_row.at(col_idx)->eval(op.get_eval_ctx(), new_row_col));
      OX(has_changed = (false == ObDatum::binary_equal(*old_row_col, *new_row_col)));
    }
  } else {
    has_changed = true;
  }
  return ret;
}

int ForeignKeyHandle::check_exist(ObTableModifyOp &modify_op, const ObForeignKeyArg &fk_arg,
                         const ObExprPtrIArray &row, ObForeignKeyChecker *fk_checker, bool expect_zero, bool use_das_scan)
{
  int ret = OB_SUCCESS;
  DEBUG_SYNC(BEFORE_FOREIGN_KEY_CONSTRAINT_CHECK);
  // insertup and merge_into currently have a core problem when checking foreign keys through das_scan_task.
  // Because both insertup and merge_into have both INSERT and UPDATE semantics,
  // the data may come from the new_row of INSERT or the new_row of UPDATE during the operation.
  // Earlier versions did not consider this problem, so there is a possibility of core
  bool is_insertup = false;
  ObPhyOperatorType op_type = modify_op.get_spec().get_type();
  if (op_type == PHY_INSERT_ON_DUP) {
    is_insertup = true;
  }
  if (use_das_scan && !is_insertup) {
    ret = check_exist_scan_task(modify_op, fk_arg, row, fk_checker);
  } else {
    if (OB_FAIL(check_exist_inner_sql(modify_op, fk_arg, row, expect_zero, true))) {
    } else if (OB_FAIL(check_exist_inner_sql(modify_op, fk_arg, row, expect_zero, false))) {
    }
  }
  return ret;
}

int ForeignKeyHandle::check_exist_scan_task(ObTableModifyOp &modify_op, const ObForeignKeyArg &fk_arg,
                         const ObExprPtrIArray &row, ObForeignKeyChecker *fk_checker)
{
  int ret = OB_SUCCESS;
  bool has_result = false;
  if (OB_ISNULL(fk_checker)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!fk_arg.use_das_scan_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(fk_checker->do_fk_check_single_row(fk_arg.columns_, row, has_result))) {
  } else {
    if (!has_result) {
      ret = OB_ERR_NO_REFERENCED_ROW;
    } else {
      ret = OB_SUCCESS;
    }
  }
  return ret;
}

int ForeignKeyHandle::check_exist_inner_sql(ObTableModifyOp &op,
                                            const ObForeignKeyArg &fk_arg,
                                            const ObExprPtrIArray &row,
                                            bool expect_zero,
                                            bool iter_uncommitted_row)
{
  int ret = OB_SUCCESS;
  const char *select_fmt = "select /*+ no_parallel */ 1 from `%.*s`.`%.*s` where %.*s for update";
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR,
                          OB_MALLOC_NORMAL_BLOCK_SIZE);
  char *stmt_buf = nullptr;
  int64_t stmt_len = 0;
  char *where_buf = nullptr;
  int64_t where_len = 0;
  int64_t stmt_pos = 0;
  int64_t where_pos = 0;
  const ObString &database_name = fk_arg.database_name_;
  const ObString &table_name = fk_arg.table_name_;
  if (row.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(gen_where(op.get_eval_ctx(), where_buf, where_len, where_pos,
                               fk_arg.columns_, row, alloc, op.get_obj_print_params()))) {
    if (OB_LIKELY(OB_ERR_NULL_VALUE == ret)) {
      // skip check exist if any column in foreign key is NULL.
      ret = OB_SUCCESS;
      stmt_pos = 0;
    } else {
    }
  } else if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, select_fmt,
                                     database_name.length(), database_name.ptr(),
                                     table_name.length(), table_name.ptr(),
                                     static_cast<int>(where_pos), where_buf))) {
  } else {
    stmt_buf[stmt_pos++] = 0;
  }
  if (OB_SUCC(ret) && stmt_pos > 0) {
    if (iter_uncommitted_row) {
      op.get_exec_ctx().get_das_ctx().iter_uncommitted_row_ = true;
    }
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      if (OB_FAIL(op.begin_nested_session(fk_arg.is_self_ref_))) {
      } else {
        // must call end_nested_session() if begin_nested_session() success.
        bool is_zero = false;
        if (OB_FAIL(op.execute_read(stmt_buf, res))) {
        } else {
          // must call res.get_result()->close() if execute_read() success.
          if (OB_ISNULL(res.get_result())) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(res.get_result()->next())) {
            if (OB_ITER_END == ret) {
              is_zero = true;
              ret = OB_SUCCESS;
            } else {
            }
          }
          if (OB_SUCC(ret)) {
            if (is_zero != expect_zero) {
              // If it is a self-referencing foreign key new row references new row
              bool is_self_ref = false;
              // The FOR UPDATE check query does not use aggregate functions, so when is_zero is false
              // judge whether only one row is affected by whether the second record can be obtained.
              bool is_affect_only_one = false;
              if (!is_zero && OB_FAIL(res.get_result()->next())) {
                if (OB_ITER_END == ret) {
                  is_affect_only_one = true;
                  ret = OB_SUCCESS;
                } else {
                }
              }
              /**
               *  expect_zero is false when fk_arg.ref_action_ is equal to
               *  ACTION_CHECK_EXIST, if the is_zero != expect_zero condition is
               *  true, then is_zero is true, need to exclude the case of self reference.
               *  other cases return OB_ERR_NO_REFERENCED_ROW.
               *
               *  expect_zero is true when fk_arg.ref_action_ is equal to
               *  ACTION_RESTRICT or ACTION_NO_ACTION, if is_zero != expect_zero condition
               *  is true, then is_zero is false, need to exclude the case of self reference and
               *  only affect one row. other cases return OB_ERR_ROW_IS_REFERENCED.
               */
              if (!iter_uncommitted_row) {
                if (OB_FAIL(is_self_ref_row(op.get_eval_ctx(), row, fk_arg, is_self_ref))) {
                } else if (is_zero && !is_self_ref) {
                  ret = OB_ERR_NO_REFERENCED_ROW;
                } else if (!is_zero) {
                  ret = OB_ERR_ROW_IS_REFERENCED;
                }
              }
            }
          }
          int close_ret = res.get_result()->close();
          if (OB_SUCCESS != close_ret) {
            LOG_WARN("failed to close", K(close_ret));
            if (OB_SUCCESS == ret) {
              ret = close_ret;
            }
          }
        }
        int end_ret = op.end_nested_session();
        if (OB_SUCCESS != end_ret) {
          LOG_WARN("failed to end nested session", K(end_ret));
          if (OB_SUCCESS == ret) {
            ret = end_ret;
          }
        }
      }
    }
  }
  op.get_exec_ctx().get_das_ctx().iter_uncommitted_row_ = false;

  return ret;
}

int ForeignKeyHandle::cascade(ObTableModifyOp &op,
                              const ObForeignKeyArg &fk_arg,
                              const ObExprPtrIArray &old_row,
                              const ObExprPtrIArray &new_row)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR,
                          OB_MALLOC_NORMAL_BLOCK_SIZE);
  char *stmt_buf = nullptr;
  int64_t stmt_len = 0;
  char *where_buf = nullptr;
  int64_t where_len = 0;
  int64_t stmt_pos = 0;
  int64_t where_pos = 0;
  const ObString &database_name = fk_arg.database_name_;
  const ObString &table_name = fk_arg.table_name_;
  if (OB_FAIL(op.get_exec_ctx().check_status())) {
  } else if (old_row.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(gen_where(op.get_eval_ctx(), where_buf, where_len, where_pos,
                               fk_arg.columns_, old_row, alloc, op.get_obj_print_params()))) {
    if (OB_LIKELY(OB_ERR_NULL_VALUE == ret)) {
      // skip cascade if any column in foreign key is NULL.
      ret = OB_SUCCESS;
      stmt_pos = 0;
    } else {
    }
  } else {
    if (!new_row.empty()) {
      // update.
      const char *update_fmt = "update `%.*s`.`%.*s` set %.*s where %.*s";
      char *set_buf = nullptr;
      int64_t set_len = 0;
      int64_t set_pos = 0;
      if (OB_FAIL(gen_set(op.get_eval_ctx(), set_buf, set_len, set_pos,
                          fk_arg.columns_, new_row, alloc, op.get_obj_print_params()))) {
      } else if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, update_fmt,
                                         database_name.length(), database_name.ptr(),
                                         table_name.length(), table_name.ptr(),
                                         static_cast<int>(set_pos), set_buf,
                                         static_cast<int>(where_pos), where_buf))) {
      } else {
        stmt_buf[stmt_pos++] = 0;
      }
    } else {
      // delete.
      const char *delete_fmt = "delete from `%.*s`.`%.*s` where %.*s";
      if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, delete_fmt,
                                  database_name.length(), database_name.ptr(),
                                  table_name.length(), table_name.ptr(),
                                  static_cast<int>(where_pos), where_buf))) {
      } else {
        stmt_buf[stmt_pos++] = 0;
      }
    }
  }
  if (OB_SUCC(ret) && stmt_pos > 0) {
    if (OB_FAIL(op.begin_nested_session(fk_arg.is_self_ref_))) {
    } else {
      if (OB_FAIL(op.execute_write(stmt_buf))) {
      }
      int end_ret = op.end_nested_session();
      if (OB_SUCCESS != end_ret) {
        LOG_WARN("failed to end nested session", K(end_ret));
        if (OB_SUCCESS == ret) {
          ret = end_ret;
        }
      }
    }
  }
  return ret;
}

int ForeignKeyHandle::set_null(ObTableModifyOp &op,
                              const ObForeignKeyArg &fk_arg,
                              const ObExprPtrIArray &old_row)
{
  int ret = OB_SUCCESS;
  
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR,
                          OB_MALLOC_NORMAL_BLOCK_SIZE);
  char *stmt_buf = nullptr;
  int64_t stmt_len = 0;
  char *where_buf = nullptr;
  int64_t where_len = 0;
  int64_t stmt_pos = 0;
  int64_t where_pos = 0;
  const ObString &database_name = fk_arg.database_name_;
  const ObString &table_name = fk_arg.table_name_;
  if (old_row.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(gen_where(op.get_eval_ctx(), where_buf, where_len, where_pos,
                               fk_arg.columns_, old_row, alloc, op.get_obj_print_params()))) {
    if (OB_LIKELY(OB_ERR_NULL_VALUE == ret)) {
      // skip cascade if any column in foreign key is NULL.
      ret = OB_SUCCESS;
      stmt_pos = 0;
    } else {
    }
  } else {
    const char *update_fmt = "update `%.*s`.`%.*s` set %.*s where %.*s";
    char *set_buf = nullptr;
    int64_t set_len = 0;
    int64_t set_pos = 0;
    if (OB_FAIL(gen_column_null_value(op.get_eval_ctx(), set_buf, set_len, set_pos, fk_arg.columns_, alloc, op.get_obj_print_params()))) {
    } else if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, update_fmt,
                                      database_name.length(), database_name.ptr(),
                                        table_name.length(), table_name.ptr(),
                                        static_cast<int>(set_pos), set_buf,
                                        static_cast<int>(where_pos), where_buf))) {
    } else {
      stmt_buf[stmt_pos++] = 0;
    }
  }
  
  if (OB_SUCC(ret) && stmt_pos > 0) {
    if (OB_FAIL(op.begin_nested_session(fk_arg.is_self_ref_))) {
    } else {
      if (OB_FAIL(op.execute_write(stmt_buf))) {
      }
      int end_ret = op.end_nested_session();
      if (OB_SUCCESS != end_ret) {
        LOG_WARN("failed to end nested session", K(end_ret));
        if (OB_SUCCESS == ret) {
          ret = end_ret;
        }
      }
    }
  }

  return ret;
}

int ForeignKeyHandle::gen_set(ObEvalCtx &eval_ctx, char *&buf, int64_t &len, int64_t &pos,
                              const ObIArray<ObForeignKeyColumn> &columns,
                              const ObExprPtrIArray &row,
                              ObIAllocator &alloc,
                              const ObObjPrintParams &print_params)
{
  return gen_column_value(eval_ctx, buf, len, pos, columns, row, ", ", alloc, print_params, false);
}

int ForeignKeyHandle::gen_where(ObEvalCtx &eval_ctx, char *&buf, int64_t &len, int64_t &pos,
                                const ObIArray<ObForeignKeyColumn> &columns,
                                const ObExprPtrIArray &row,
                                ObIAllocator &alloc,
                                const ObObjPrintParams &print_params)
{
  return gen_column_value(eval_ctx, buf, len, pos, columns, row, " and ", alloc, print_params, true);
}

int ForeignKeyHandle::gen_column_value(ObEvalCtx &eval_ctx, char *&buf, int64_t &len, int64_t &pos,
                                       const ObIArray<ObForeignKeyColumn> &columns,
                                       const ObExprPtrIArray &row,
                                       const char *delimiter,
                                       ObIAllocator &alloc,
                                       const ObObjPrintParams &print_params,
                                       bool forbid_null)
{
  const char *column_fmt = "`%.*s` = ";
  int ret = OB_SUCCESS;
  ObDatum *col_datum = NULL;
  if (OB_ISNULL(delimiter) || OB_ISNULL(print_params.tz_info_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (columns.count() <= 0) {
    ret= OB_ERR_UNEXPECTED;
    LOG_ERROR("columns count of fk is zero or less than zero", K(ret), K(columns.count()), K(columns));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); i++) {
    OZ(row.at(columns.at(i).idx_)->eval(eval_ctx, col_datum));
    if (OB_SUCC(ret)) {
      const ObString &col_name = columns.at(i).name_;
      const ObObjMeta &col_obj_meta = row.at(columns.at(i).idx_)->obj_meta_;
      ObObj col_obj;
      if (forbid_null && col_datum->is_null()) {
        ret = OB_ERR_NULL_VALUE;
        // NO LOG.
      } else if (OB_FAIL(databuff_printf(buf, len, pos, alloc, column_fmt,
                                        col_name.length(), col_name.ptr()))) {
      } else if (OB_FAIL(col_datum->to_obj(col_obj, col_obj_meta))) {
      } else if (OB_FAIL(col_obj.print_sql_literal(buf, len, pos, alloc, print_params))) {
      } else if (OB_FAIL(databuff_printf(buf, len, pos, alloc, "%s", delimiter))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    pos -= STRLEN(delimiter);
    buf[pos++] = 0;
  }
  return ret;
}

int  ForeignKeyHandle::gen_column_null_value(ObEvalCtx &ctx, char *&buf, int64_t &len, int64_t &pos,
                              const common::ObIArray<ObForeignKeyColumn> &columns,
                              common::ObIAllocator &alloc,
                              const common::ObObjPrintParams &print_params)
{
  int ret = OB_SUCCESS;
  const char *column_fmt = "`%.*s` = ";
  const char *delimiter = ", ";
  if (OB_ISNULL(delimiter) || OB_ISNULL(print_params.tz_info_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (columns.count() <= 0) {
    ret= OB_ERR_UNEXPECTED;
    LOG_ERROR("columns count of fk is zero or less than zero", K(ret), K(columns.count()), K(columns));
  }
  ObObj col_obj;
  col_obj.set_null();
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); i++) {
    if (OB_SUCC(ret)) {
      const ObString &col_name = columns.at(i).name_;
      if (OB_FAIL(databuff_printf(buf, len, pos, alloc, column_fmt,
                                        col_name.length(), col_name.ptr()))) {
      } else if (OB_FAIL(col_obj.print_sql_literal(buf, len, pos, alloc, print_params))) {
      } else if (OB_FAIL(databuff_printf(buf, len, pos, alloc, "%s", delimiter))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    pos -= STRLEN(delimiter);
    buf[pos++] = 0;
  }
  return ret;
}

int ForeignKeyHandle::is_self_ref_row(ObEvalCtx &eval_ctx,
                                      const ObExprPtrIArray &row,
                                      const ObForeignKeyArg &fk_arg,
                                      bool &is_self_ref)
{
  int ret = OB_SUCCESS;
  const common::ObDatumAccessContext *access_ctx = nullptr;
  is_self_ref = fk_arg.is_self_ref_;
  ObDatum *name_col = NULL;
  ObDatum *val_col = NULL;
  if (OB_FAIL(eval_ctx.get_datum_access_ctx(access_ctx))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && is_self_ref && i < fk_arg.columns_.count(); i++) {
    const int32_t name_idx = fk_arg.columns_.at(i).name_idx_;
    const int32_t val_idx = fk_arg.columns_.at(i).idx_;
    ObExprCmpFuncType cmp_func = row.at(name_idx)->basic_funcs_->null_first_cmp_;
    // TODO qubin.qb: uncomment below block revert the defensive check
    //OZ((cmp_func = (row.at(name_idx)->basic_funcs_->null_first_cmp_))
    //   != row.at(val_idx)->basic_funcs_->null_first_cmp_);
    OZ(row.at(name_idx)->eval(eval_ctx, name_col));
    OZ(row.at(val_idx)->eval(eval_ctx, val_col));
    int cmp_ret = 0;
    if (OB_FAIL(cmp_func(*name_col, *val_col, cmp_ret, access_ctx))) {
    } else {
      is_self_ref = (0 == cmp_ret);
    }
  }
  return ret;
}

// make MY_SPEC macro available.
OB_INLINE static const ObTableModifySpec &get_my_spec(const ObTableModifyOp &op)
{
  return op.get_spec();
}

ObTableModifySpec::ObTableModifySpec(common::ObIAllocator &alloc,
                                     const ObPhyOperatorType type)
  : ObOpSpec(alloc, type),
    expr_frame_info_(NULL),
    ab_stmt_id_(nullptr),
    flags_(0),
    das_dop_(0)
{
}

OB_DEF_SERIALIZE(ObTableModifySpec)
{
  int ret = OB_SUCCESS;
  BASE_SER((ObTableModifySpec, ObOpSpec));
  OB_UNIS_ENCODE(flags_);
  OB_UNIS_ENCODE(ab_stmt_id_);
  OB_UNIS_ENCODE(das_dop_);
  return ret;
}

OB_DEF_DESERIALIZE(ObTableModifySpec)
{
  int ret = OB_SUCCESS;
  BASE_DESER((ObTableModifySpec, ObOpSpec));
  OB_UNIS_DECODE(flags_);
  OB_UNIS_DECODE(ab_stmt_id_);
  OB_UNIS_DECODE(das_dop_);
  return ret;
}

OB_DEF_SERIALIZE_SIZE(ObTableModifySpec)
{
  int64_t len = 0;
  BASE_ADD_LEN((ObTableModifySpec, ObOpSpec));
  OB_UNIS_ADD_LEN(flags_);
  OB_UNIS_ADD_LEN(ab_stmt_id_);
  OB_UNIS_ADD_LEN(das_dop_);
  return len;
}

OB_SERIALIZE_MEMBER(ObTableModifyOpInput);

ObTableModifyOp::ObTableModifyOp(ObExecContext &ctx,
                                 const ObOpSpec &spec,
                                 ObOpInput *input)
  : ObOperator(ctx, spec, input),
    sql_proxy_(NULL),
    inner_conn_guard_(),
    inner_conn_(NULL),
    saved_conn_(),
    need_foreign_key_check_(false),
    need_close_conn_(false),
    iter_end_(false),
    dml_rtctx_(eval_ctx_, ctx, *this),
    execute_single_row_(false),
    dml_modify_rows_(ctx.get_allocator()),
    last_store_row_(),
    saved_session_(NULL)
{
  obj_print_params_ = CREATE_OBJ_PRINT_PARAM(ctx_.get_my_session());
  obj_print_params_.need_cast_expr_ = true;
  // bugfix:
  // in NO_BACKSLASH_ESCAPES, obj_print_sql<ObVarcharType> won't escape.
  // We use skip_escape_ to indicate this case. It will finally be passed to ObHexEscapeSqlStr.
  GET_SQL_MODE_BIT(IS_NO_BACKSLASH_ESCAPES, ctx_.get_my_session()->get_sql_mode(), obj_print_params_.skip_escape_);
}

int ObTableModifyOp::inner_open()
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *my_session = GET_MY_SESSION(ctx_);
  if (OB_FAIL(init_foreign_key_operation())) {
  } else if (MY_SPEC.plan_->has_nested_sql() && OB_FAIL(open_inner_conn())) {
  } else if (OB_FAIL(calc_single_table_loc())) {
  } else if (OB_FAIL(check_need_exec_single_row())) {
  } else {
    init_das_dml_ctx();
  }
  return ret;
}

void ObTableModifyOp::clear_dml_evaluated_flag(ObExpr *clear_expr)
{
  if (clear_expr->is_batch_result()) {
    clear_expr->get_evaluated_flags(eval_ctx_).unset(eval_ctx_.get_batch_idx());
  } else {
    clear_expr->get_eval_info(eval_ctx_).clear_evaluated_flag();
  }
  if (0 < clear_expr->parent_cnt_) {
    clear_dml_evaluated_flag(clear_expr->parent_cnt_, clear_expr->parents_);
  }
}

void ObTableModifyOp::clear_dml_evaluated_flag(int64_t parent_cnt, ObExpr **parent_exprs)
{
  for (int64_t i = 0; i < parent_cnt; ++i) {
    clear_dml_evaluated_flag(parent_exprs[i]);
  }
}

void ObTableModifyOp::clear_dml_evaluated_flag()
{
  // Clear the parent expr because the assignment expr may be changed
  for (int64_t i = 0; i < trigger_clear_exprs_.count(); ++i) {
    ObExpr *expr = trigger_clear_exprs_.at(i);
    clear_dml_evaluated_flag(expr);
  }
}

ObDasParallelType ObTableModifyOp::check_das_parallel_type()
{
  ObDasParallelType type = DAS_SERIALIZATION;
  // Local DAS modification runs serially.
  return type;
}

OB_INLINE int ObTableModifyOp::init_das_dml_ctx()
{
  ObSQLSessionInfo *session = GET_MY_SESSION(ctx_);
  const char *label = nullptr;
  switch(MY_SPEC.type_) {
  case PHY_INSERT:
    label = "InsDASCtx";
    break;
  case PHY_UPDATE:
    label = "UpdDASCtx";
    break;
  case PHY_DELETE:
    label = "DelDASCtx";
    break;
  case PHY_LOCK:
    label = "LockDASCtx";
    break;
  default:
    label = "DmlDASCtx";
    break;
  }
  ObMemAttr memattr(label, ObCtxIds::EXECUTE_CTX_ID);
  dml_rtctx_.das_ref_.set_expr_frame_info(nullptr != MY_SPEC.expr_frame_info_
                                        ? MY_SPEC.expr_frame_info_
                                        : &MY_SPEC.plan_->get_expr_frame_info());
  dml_rtctx_.das_ref_.set_mem_attr(memattr);
  dml_rtctx_.das_ref_.get_das_parallel_ctx().set_das_parallel_type(check_das_parallel_type());
  dml_rtctx_.das_ref_.get_das_parallel_ctx().set_das_dop(ctx_.get_das_ctx().get_real_das_dop());
  if (check_das_parallel_type() != DAS_SERIALIZATION) {
    LOG_TRACE("this sql use das parallel submit", K(check_das_parallel_type()));
  }
  return OB_SUCCESS;
}

int ObTableModifyOp::replace_implict_cursor(int64_t affected_rows,
                                            int64_t found_rows,
                                            int64_t matched_rows,
                                            int64_t duplicated_rows)
{
  int ret = OB_SUCCESS;
  bool is_ins_val_opt = ctx_.get_sql_ctx()->is_do_insert_batch_opt();
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
  if (MY_SPEC.ab_stmt_id_ != nullptr && !is_ins_val_opt) {
    ObDatum *stmt_id_datum = nullptr;
    ObImplicitCursorInfo implicit_cursor;
    if (OB_FAIL(prepare_implict_cursor(affected_rows,
                                       found_rows,
                                       matched_rows,
                                       duplicated_rows,
                                       implicit_cursor))) {
    } else if (OB_FAIL(plan_ctx->replace_implicit_cursor_info(implicit_cursor))) {
    }
  }
  return ret;
}

int ObTableModifyOp::prepare_implict_cursor(int64_t affected_rows,
                                            int64_t found_rows,
                                            int64_t matched_rows,
                                            int64_t duplicated_rows,
                                            ObImplicitCursorInfo &implicit_cursor)
{
  int ret = OB_SUCCESS;
  ObDatum *stmt_id_datum = nullptr;
  if (OB_FAIL(MY_SPEC.ab_stmt_id_->eval(eval_ctx_, stmt_id_datum))) {
  } else {
    ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
    int64_t stmt_id = stmt_id_datum->get_int();
    implicit_cursor.stmt_id_ = stmt_id;
    implicit_cursor.found_rows_ += found_rows;
    implicit_cursor.matched_rows_ += matched_rows;
    implicit_cursor.affected_rows_ += affected_rows;
    implicit_cursor.duplicated_rows_ += duplicated_rows;
    implicit_cursor.last_insert_id_ = plan_ctx->get_autoinc_col_value();
  }
  return ret;
}

int ObTableModifyOp::merge_implict_cursor(int64_t affected_rows,
                                          int64_t found_rows,
                                          int64_t matched_rows,
                                          int64_t duplicated_rows)
{
  int ret = OB_SUCCESS;
  bool is_ins_val_opt = ctx_.get_sql_ctx()->is_do_insert_batch_opt();
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
  if (MY_SPEC.ab_stmt_id_ != nullptr && !is_ins_val_opt) {
    ObDatum *stmt_id_datum = nullptr;
    ObImplicitCursorInfo implicit_cursor;
    if (OB_FAIL(prepare_implict_cursor(affected_rows,
                                       found_rows,
                                       matched_rows,
                                       duplicated_rows,
                                       implicit_cursor))) {
    } else if (OB_FAIL(plan_ctx->merge_implicit_cursor_info(implicit_cursor))) {
    }
  }
  return ret;
}

int ObTableModifyOp::inner_switch_iterator()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_switch_iterator())) {
  }

  return ret;
}

int ObTableModifyOp::inner_close()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = ctx_.get_physical_plan_ctx();
  // release cache_handle for auto-increment
  share::ObAutoincrementService &auto_service = share::ObAutoincrementService::get_instance();
  ObIArray<share::AutoincParam> &autoinc_params = plan_ctx->get_autoinc_params();
  for (int64_t i = 0; i < autoinc_params.count(); ++i) {
    if (NULL != autoinc_params.at(i).cache_handle_) {
      auto_service.release_handle(autoinc_params.at(i).cache_handle_);
    }
  }
  close_inner_conn();
  if (dml_rtctx_.das_ref_.has_task()) {
    if (OB_FAIL(dml_rtctx_.das_ref_.close_all_task())) {
    } else {
      dml_rtctx_.das_ref_.reset();
    }
  }
  dml_modify_rows_.clear();
  return ret;
}

OB_INLINE int ObTableModifyOp::init_foreign_key_operation()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *phy_plan_ctx = GET_PHY_PLAN_CTX(ctx_);
  if (OB_ISNULL(phy_plan_ctx)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!phy_plan_ctx->need_foreign_key_checks()) {
    // set fk check even if ret != OB_SUCCESS. see ObTableModifyOp::inner_close()
    need_foreign_key_check_ = false;
  } else {
    need_foreign_key_check_ = MY_SPEC.need_foreign_key_check_;
  }
  return ret;
}

int ObTableModifyOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    iter_end_ = false;
    if (dml_rtctx_.das_ref_.has_task()) {
      if (OB_FAIL(dml_rtctx_.das_ref_.close_all_task())) {
      } else {
        dml_rtctx_.reuse();
      }
    }
  }
  if (OB_SUCC(ret)) {
    dml_modify_rows_.clear();
    if (OB_FAIL(calc_single_table_loc())) {
    }
  }
  return ret;
}

namespace
{
// the hook's own invalidate UPDATE is a DML on the same AI table, so without this guard it
// re-enters the hook and recurses until the stack is exhausted
static thread_local bool g_ai_enqueue_in_progress = false;
class AiEnqueueGuard
{
public:
  AiEnqueueGuard() { g_ai_enqueue_in_progress = true; }
  ~AiEnqueueGuard() { g_ai_enqueue_in_progress = false; }
  static bool in_progress() { return g_ai_enqueue_in_progress; }
private:

  DISALLOW_COPY_AND_ASSIGN(AiEnqueueGuard);
};


} // namespace


// The task row of one AI column of one data row is addressed by a slot id derived from
// (table, column, row key text): a second invalidation of the same slot overwrites the
// pending task instead of stacking up, and a DELETE can remove its own task row without
// scanning the task table.
int64_t ai_task_slot_id(const uint64_t table_id, const uint64_t column_id,
                        const common::ObString &predicate)
{
  uint64_t h = 1469598103934665603ULL;
  const uint64_t parts[3] = { table_id, column_id,
                              static_cast<uint64_t>(predicate.length()) };
  for (int64_t p = 0; p < 3; ++p) {
    for (int64_t b = 0; b < 8; ++b) {
      h = (h ^ ((parts[p] >> (b * 8)) & 0xffULL)) * 1099511628211ULL;
    }
  }
  for (int64_t i = 0; i < predicate.length(); ++i) {
    h = (h ^ static_cast<uint8_t>(predicate.ptr()[i])) * 1099511628211ULL;
  }
  h &= (~(1ULL << 63));  // task_id is a signed column
  return static_cast<int64_t>(h);
}

// Writes one row into oceanbase.__all_ai_column_task through the storage layer, on the
// user transaction's own ObTxDesc. A nested SQL statement is not usable here: the plan is
// not built for nested sql and re-entering the executor crashes the observer.
int ObTableModifyOp::insert_ai_column_task_row(const int64_t task_id,
                                              const uint64_t table_id,
                                              const uint64_t column_id,
                                              const ObString &rowkey_predicate,
                                              const int64_t request_id)
{
  int ret = OB_SUCCESS;
  // every heavy object is arena allocated: an ObSchemaGetterGuard plus the row/iterator
  // parameters do not fit the worker stack, which aborts with a stack overflow warning
  common::ObArenaAllocator alloc("AiTaskDml");
  share::schema::ObSchemaGetterGuard *schema_guard = NULL;
  share::schema::ObTableDMLParam *table_dml_param = NULL;
  storage::ObDMLBaseParam *dml_param = NULL;
  storage::ObStoreCtxGuard *store_ctx_guard = NULL;
  storage::ObValueRowIterator *row_iter = NULL;
  blocksstable::ObDatumRow *datum_row = NULL;
  common::ObSEArray<uint64_t, 16> column_ids;
  ObSQLSessionInfo *session = GET_MY_SESSION(ctx_);
  transaction::ObTxDesc *tx_desc = OB_ISNULL(session) ? NULL : session->get_tx_desc();
  transaction::ObTxReadSnapshot &snapshot = ctx_.get_das_ctx().get_snapshot();
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
  const int64_t timeout_us = OB_ISNULL(plan_ctx) ? INT64_MAX : plan_ctx->get_ps_timeout_timestamp();
  storage::ObAccessService *oas =
      ::oceanbase::share::server_service<::oceanbase::storage::ObAccessService>();
  const share::schema::ObTableSchema *task_schema = NULL;
  const common::ObTabletID task_tablet_id(share::OB_ALL_AI_COLUMN_TASK_TID);
  int64_t idx_task_id = -1, idx_table_id = -1, idx_column_id = -1, idx_pred = -1;
  int64_t idx_state = -1, idx_retry = -1, idx_request = -1, idx_eligible = -1;
  int64_t idx_gmt_create = -1, idx_gmt_modified = -1;
  if (OB_ISNULL(oas) || OB_ISNULL(tx_desc) || !tx_desc->is_valid() || !snapshot.is_valid()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("no usable transaction for ai task", K(ret));
  } else if (OB_ISNULL(schema_guard = OB_NEWx(share::schema::ObSchemaGetterGuard, &alloc))
             || OB_ISNULL(table_dml_param = OB_NEWx(share::schema::ObTableDMLParam, &alloc, alloc))
             || OB_ISNULL(dml_param = OB_NEWx(storage::ObDMLBaseParam, &alloc))
             || OB_ISNULL(store_ctx_guard = OB_NEWx(storage::ObStoreCtxGuard, &alloc))
             || OB_ISNULL(row_iter = OB_NEWx(storage::ObValueRowIterator, &alloc))
             || OB_ISNULL(datum_row = OB_NEWx(blocksstable::ObDatumRow, &alloc))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to alloc ai task write buffers", K(ret));
  } else if (OB_FAIL(share::schema::ObMultiVersionSchemaService::get_instance()
                     .get_runtime_schema_guard(*schema_guard))) {
    LOG_WARN("fail to get schema guard", K(ret));
  } else if (OB_FAIL(schema_guard->get_table_schema(share::OB_ALL_AI_COLUMN_TASK_TID,
                                                    task_schema))) {
    LOG_WARN("fail to get task table schema", K(ret));
  } else if (OB_ISNULL(task_schema)) {
    ret = OB_TABLE_NOT_EXIST;
  } else if (OB_FAIL(task_schema->get_all_column_ids(column_ids))) {
    LOG_WARN("fail to get task table columns", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < column_ids.count(); ++i) {
      const share::schema::ObColumnSchemaV2 *col =
          task_schema->get_column_schema(column_ids.at(i));
      if (OB_ISNULL(col)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        const ObString name = col->get_column_name_str();
        if (0 == name.case_compare("task_id")) { idx_task_id = i; }
        else if (0 == name.case_compare("table_id")) { idx_table_id = i; }
        else if (0 == name.case_compare("column_id")) { idx_column_id = i; }
        else if (0 == name.case_compare("rowkey_predicate")) { idx_pred = i; }
        else if (0 == name.case_compare("state")) { idx_state = i; }
        else if (0 == name.case_compare("retry_count")) { idx_retry = i; }
        else if (0 == name.case_compare("request_id")) { idx_request = i; }
        else if (0 == name.case_compare("eligible_ts")) { idx_eligible = i; }
        else if (0 == name.case_compare("gmt_create")) { idx_gmt_create = i; }
        else if (0 == name.case_compare("gmt_modified")) { idx_gmt_modified = i; }
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (idx_task_id < 0 || idx_table_id < 0 || idx_column_id < 0 || idx_pred < 0
             || idx_state < 0 || idx_retry < 0 || idx_request < 0 || idx_eligible < 0
             || idx_gmt_create < 0 || idx_gmt_modified < 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai task table columns are not as expected", K(ret));
  } else if (OB_FAIL(table_dml_param->convert(task_schema,
                                              task_schema->get_schema_version(),
                                              column_ids))) {
    LOG_WARN("fail to convert task table param", K(ret));
  } else if (OB_FAIL(datum_row->init(column_ids.count()))) {
    LOG_WARN("fail to init task datum row", K(ret));
  } else {
    const int64_t now_ts = ObTimeUtility::current_time();
    datum_row->reuse();
    for (int64_t i = 0; i < column_ids.count(); ++i) {
      const share::schema::ObColumnSchemaV2 *col =
          task_schema->get_column_schema(column_ids.at(i));
      if (OB_NOT_NULL(col) && !col->is_nullable()) {
        // a column this writer does not fill still needs a defined value, the row is not
        // zeroed anywhere else and an uninitialised datum reads back as garbage
        datum_row->storage_datums_[i].set_int(0);
      } else {
        datum_row->storage_datums_[i].set_null();
      }
    }
    datum_row->storage_datums_[idx_task_id].set_int(task_id);
    datum_row->storage_datums_[idx_table_id].set_int(static_cast<int64_t>(table_id));
    datum_row->storage_datums_[idx_column_id].set_int(static_cast<int64_t>(column_id));
    datum_row->storage_datums_[idx_pred].set_string(rowkey_predicate.ptr(),
                                                    rowkey_predicate.length());
    datum_row->storage_datums_[idx_state].set_int(0);
    datum_row->storage_datums_[idx_retry].set_int(0);
    datum_row->storage_datums_[idx_request].set_int(request_id);
    datum_row->storage_datums_[idx_eligible].set_timestamp(now_ts);
    datum_row->storage_datums_[idx_gmt_create].set_timestamp(now_ts);
    datum_row->storage_datums_[idx_gmt_modified].set_timestamp(now_ts);
    dml_param->sql_mode_ = SMO_DEFAULT;
    dml_param->write_flag_.reset();
    dml_param->table_param_ = table_dml_param;
    dml_param->timeout_ = timeout_us;
    dml_param->schema_version_ = task_schema->get_schema_version();
    dml_param->check_schema_version_ = false;
    dml_param->tz_info_ = NULL;
    dml_param->branch_id_ = 0;
    dml_param->dml_allocator_ = &alloc;
    dml_param->store_ctx_guard_ = store_ctx_guard;
    if (OB_FAIL(row_iter->init())) {
      LOG_WARN("fail to init task row iterator", K(ret));
    } else if (OB_FAIL(row_iter->add_row(*datum_row))) {
      LOG_WARN("fail to add task row", K(ret));
    } else if (OB_FAIL(dml_param->snapshot_.assign(snapshot))) {
      LOG_WARN("fail to assign snapshot", K(ret));
    } else if (!dml_param->is_valid()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("ai task dml param is invalid", K(ret), K(timeout_us));
    } else if (OB_FAIL(oas->get_write_store_ctx_guard(timeout_us, *tx_desc, snapshot, 0,
                                                     dml_param->write_flag_, *store_ctx_guard))) {
      LOG_WARN("fail to get write store ctx for ai task", K(ret));
    } else {
      int64_t affected = 0;
      if (OB_FAIL(oas->put_rows(task_tablet_id, *tx_desc, *dml_param, column_ids, row_iter,
                                affected))) {
        LOG_WARN("fail to write ai task row", K(ret), K(task_id), K(table_id),
                 K(column_id));
      }
      store_ctx_guard->reset();
    }
    row_iter->reset();
  }
  // the arena does not run destructors, and these objects own inner allocators
  if (OB_NOT_NULL(datum_row)) { datum_row->~ObDatumRow(); }
  if (OB_NOT_NULL(row_iter)) { row_iter->~ObValueRowIterator(); }
  if (OB_NOT_NULL(store_ctx_guard)) { store_ctx_guard->~ObStoreCtxGuard(); }
  if (OB_NOT_NULL(dml_param)) { dml_param->~ObDMLBaseParam(); }
  if (OB_NOT_NULL(table_dml_param)) { table_dml_param->~ObTableDMLParam(); }
  if (OB_NOT_NULL(schema_guard)) { schema_guard->~ObSchemaGetterGuard(); }
  return ret;
}


// Removes the pending task row of one AI column of one data row, on the user's transaction,
// so a DELETE already hides the task from the statements which follow it.
int ObTableModifyOp::delete_ai_column_task_row(const int64_t task_id)
{
  int ret = OB_SUCCESS;
  common::ObArenaAllocator alloc("AiTaskDel");
  share::schema::ObSchemaGetterGuard *schema_guard = NULL;
  share::schema::ObTableDMLParam *table_dml_param = NULL;
  storage::ObDMLBaseParam *dml_param = NULL;
  storage::ObStoreCtxGuard *store_ctx_guard = NULL;
  storage::ObValueRowIterator *row_iter = NULL;
  blocksstable::ObDatumRow *datum_row = NULL;
  common::ObSEArray<uint64_t, 16> all_column_ids;
  common::ObSEArray<uint64_t, 2> column_ids;
  ObSQLSessionInfo *session = GET_MY_SESSION(ctx_);
  transaction::ObTxDesc *tx_desc = OB_ISNULL(session) ? NULL : session->get_tx_desc();
  transaction::ObTxReadSnapshot &snapshot = ctx_.get_das_ctx().get_snapshot();
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
  const int64_t timeout_us = OB_ISNULL(plan_ctx) ? INT64_MAX : plan_ctx->get_ps_timeout_timestamp();
  storage::ObAccessService *oas =
      ::oceanbase::share::server_service<::oceanbase::storage::ObAccessService>();
  const share::schema::ObTableSchema *task_schema = NULL;
  const common::ObTabletID task_tablet_id(share::OB_ALL_AI_COLUMN_TASK_TID);
  int64_t idx_task_id = -1;
  if (OB_ISNULL(oas) || OB_ISNULL(tx_desc) || !tx_desc->is_valid() || !snapshot.is_valid()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("no usable transaction for ai task delete", K(ret));
  } else if (OB_ISNULL(schema_guard = OB_NEWx(share::schema::ObSchemaGetterGuard, &alloc))
             || OB_ISNULL(table_dml_param = OB_NEWx(share::schema::ObTableDMLParam, &alloc, alloc))
             || OB_ISNULL(dml_param = OB_NEWx(storage::ObDMLBaseParam, &alloc))
             || OB_ISNULL(store_ctx_guard = OB_NEWx(storage::ObStoreCtxGuard, &alloc))
             || OB_ISNULL(row_iter = OB_NEWx(storage::ObValueRowIterator, &alloc))
             || OB_ISNULL(datum_row = OB_NEWx(blocksstable::ObDatumRow, &alloc))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to alloc ai task delete buffers", K(ret));
  } else if (OB_FAIL(share::schema::ObMultiVersionSchemaService::get_instance()
                     .get_runtime_schema_guard(*schema_guard))) {
    LOG_WARN("fail to get schema guard for ai task delete", K(ret));
  } else if (OB_FAIL(schema_guard->get_table_schema(share::OB_ALL_AI_COLUMN_TASK_TID,
                                                    task_schema))) {
    LOG_WARN("fail to get task table schema", K(ret));
  } else if (OB_ISNULL(task_schema)) {
    ret = OB_TABLE_NOT_EXIST;
  } else if (OB_FAIL(task_schema->get_all_column_ids(all_column_ids))) {
    LOG_WARN("fail to get task table columns", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < all_column_ids.count(); ++i) {
      const share::schema::ObColumnSchemaV2 *col =
          task_schema->get_column_schema(all_column_ids.at(i));
      if (OB_ISNULL(col)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (0 == col->get_column_name_str().case_compare("task_id")) {
        idx_task_id = i;
      }
    }
    if (OB_FAIL(ret)) {
    } else if (idx_task_id < 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai task table has no task_id column", K(ret));
    } else if (OB_FAIL(column_ids.push_back(all_column_ids.at(idx_task_id)))) {
      LOG_WARN("fail to build ai task delete column list", K(ret));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(table_dml_param->convert(task_schema, task_schema->get_schema_version(),
                                              column_ids))) {
    LOG_WARN("fail to convert task table param", K(ret));
  } else if (OB_FAIL(datum_row->init(column_ids.count()))) {
    LOG_WARN("fail to init ai task delete row", K(ret));
  } else {
    datum_row->storage_datums_[0].set_int(task_id);
    dml_param->sql_mode_ = SMO_DEFAULT;
    dml_param->write_flag_.reset();
    dml_param->table_param_ = table_dml_param;
    dml_param->timeout_ = timeout_us;
    dml_param->schema_version_ = task_schema->get_schema_version();
    dml_param->check_schema_version_ = false;
    dml_param->tz_info_ = NULL;
    dml_param->branch_id_ = 0;
    dml_param->dml_allocator_ = &alloc;
    dml_param->store_ctx_guard_ = store_ctx_guard;
    if (OB_FAIL(row_iter->init())) {
      LOG_WARN("fail to init ai task delete iterator", K(ret));
    } else if (OB_FAIL(row_iter->add_row(*datum_row))) {
      LOG_WARN("fail to add ai task delete row", K(ret));
    } else if (OB_FAIL(dml_param->snapshot_.assign(snapshot))) {
      LOG_WARN("fail to assign snapshot", K(ret));
    } else if (!dml_param->is_valid()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("ai task delete dml param is invalid", K(ret), K(timeout_us));
    } else if (OB_FAIL(oas->get_write_store_ctx_guard(timeout_us, *tx_desc, snapshot, 0,
                                                    dml_param->write_flag_, *store_ctx_guard))) {
      LOG_WARN("fail to get write store ctx for ai task delete", K(ret));
    } else {
      int64_t affected = 0;
      if (OB_FAIL(oas->delete_rows(task_tablet_id, *tx_desc, *dml_param, column_ids, row_iter,
                                   affected))) {
        LOG_WARN("fail to delete ai task row", K(ret), K(task_id), K(affected));
      } else if (affected != 1) {
        LOG_TRACE("no pending ai task row for this slot", K(task_id), K(affected));
      }
      store_ctx_guard->reset();
    }
    row_iter->reset();
  }
  if (OB_NOT_NULL(datum_row)) { datum_row->~ObDatumRow(); }
  if (OB_NOT_NULL(row_iter)) { row_iter->~ObValueRowIterator(); }
  if (OB_NOT_NULL(store_ctx_guard)) { store_ctx_guard->~ObStoreCtxGuard(); }
  if (OB_NOT_NULL(dml_param)) { dml_param->~ObDMLBaseParam(); }
  if (OB_NOT_NULL(table_dml_param)) { table_dml_param->~ObTableDMLParam(); }
  if (OB_NOT_NULL(schema_guard)) { schema_guard->~ObSchemaGetterGuard(); }
  return ret;
}

bool ObTableModifyOp::need_after_row_process(const ObDMLBaseCtDef &dml_ctdef)

{
  bool needed = need_foreign_key_check(dml_ctdef) || has_after_row_trigger(dml_ctdef);
  if (sql::AiEnqueueGuard::in_progress()) {
    return needed;
  }
  if (!needed) {
    // an async AI column also needs the after-row pass, to enqueue its task row in this
    // transaction; cache the scan per table id since the row loop re-asks for every row
    const uint64_t loc_id = dml_ctdef.das_base_ctdef_.table_id_;
    const uint64_t ref_id = dml_ctdef.das_base_ctdef_.index_tid_;
    if (ai_after_row_cached_ && loc_id == ai_after_row_table_id_) {
      needed = ai_after_row_result_;
    } else {
      bool has_async = false;
      (void)AiColumnHandle::has_async_ai_column(ctx_, loc_id, has_async);
      if (!has_async) {
        (void)AiColumnHandle::has_async_ai_column(ctx_, ref_id, has_async);
      }
      ai_after_row_cached_ = true;
      ai_after_row_table_id_ = loc_id;
      ai_after_row_result_ = has_async;
      needed = has_async;
    }
  }
  return needed;
}

int ObTableModifyOp::check_need_exec_single_row() {
  int ret = OB_SUCCESS;
  if (MY_SPEC.is_returning_ && (need_foreign_key_checks() || MY_SPEC.need_trigger_fire_)) {
    execute_single_row_ = true;
  }
  // the after-row AI column hook only runs on the single-row path, and an async AI column
  // must enqueue its task row inside this transaction, row by row
  if (OB_SUCC(ret) && !execute_single_row_ && !sql::AiEnqueueGuard::in_progress()) {
    ObTableID table_loc_id = OB_INVALID_ID;
    ObTableID ref_table_id = OB_INVALID_ID;
    bool has_async = false;
    if (OB_FAIL(MY_SPEC.get_single_table_loc_id(table_loc_id, ref_table_id))) {
      ret = OB_SUCCESS; // a multi table statement has no single AI column target
    } else if (OB_FAIL(AiColumnHandle::has_async_ai_column(ctx_, table_loc_id, has_async))) {
      LOG_WARN("fail to detect async ai columns", K(ret), K(table_loc_id));
    } else if (!has_async
               && OB_FAIL(AiColumnHandle::has_async_ai_column(ctx_, ref_table_id, has_async))) {
      LOG_WARN("fail to detect async ai columns", K(ret), K(ref_table_id));
    } else if (has_async) {
      execute_single_row_ = true;
    }
  }
  return ret;
}

int ObTableModifyOp::get_gi_task()
{
  int ret = OB_SUCCESS;
  ObGranuleTaskInfo gi_task_info;
  GIPrepareTaskMap *gi_prepare_map = nullptr;
  ObDASCtx &das_ctx = ctx_.get_das_ctx();
  ObTableID table_loc_id = OB_INVALID_ID;
  ObTableID ref_table_id = OB_INVALID_ID;
  ObTabletID tablet_id;
  if (OB_FAIL(MY_SPEC.get_single_table_loc_id(table_loc_id, ref_table_id))) {
  } else if (OB_FAIL(ctx_.get_gi_task_map(gi_prepare_map))) {
  } else if (OB_FAIL(gi_prepare_map->get_refactored(MY_SPEC.id_, gi_task_info))) {
    if (ret != OB_HASH_NOT_EXIST) {
    } else {
      LOG_DEBUG("no prepared task info, set table modify to end",
        K(MY_SPEC.get_id()), K(this), K(lbt()));
      // Current DML operator cannot obtain task from GI, indicating current DML operator iter end
      iter_end_ = true;
      ret = OB_SUCCESS;
    }
  } else if (OB_ISNULL(get_input()->table_loc_) &&
      OB_ISNULL(get_input()->table_loc_ = das_ctx.get_table_loc_by_id(table_loc_id, ref_table_id))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    get_input()->tablet_loc_ = gi_task_info.tablet_loc_;
    LOG_DEBUG("DML operator consume a task", K(ret),
              K(gi_task_info), KPC(get_input()), K(MY_SPEC.id_), K(get_input()));
  }
  return ret;
}

int ObTableModifyOp::calc_single_table_loc()
{
  int ret = OB_SUCCESS;
  if (!MY_SPEC.use_dist_das_) {
    ObTableID table_loc_id = OB_INVALID_ID;
    ObTableID ref_table_id = OB_INVALID_ID;
    ObDASCtx &das_ctx = ctx_.get_das_ctx();
    ObDASTableLoc *table_loc = nullptr;
    if (OB_UNLIKELY(MY_SPEC.gi_above_)) {
      if (OB_FAIL(get_gi_task())) {
      }
    } else if (OB_FAIL(MY_SPEC.get_single_table_loc_id(table_loc_id, ref_table_id))) {
    } else if (OB_ISNULL(table_loc = das_ctx.get_table_loc_by_id(table_loc_id, ref_table_id))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      get_input()->table_loc_ = table_loc;
      get_input()->tablet_loc_ = table_loc->get_first_tablet_loc();
    }
  }
  return ret;
}

int ObTableModifyOp::open_inner_conn()
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = NULL;
  if (OB_ISNULL(sql_proxy_ = ctx_.get_sql_proxy())) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(session = ctx_.get_my_session())) {
    ret = OB_NOT_INIT;
  } else if (NULL != session->get_inner_conn()) {
    // do nothing.
  } else if (OB_FAIL(
                 query::ObInnerSQLConnectionAccess::
                     create_connection_with_external_session(
                         session, inner_conn_guard_))) {
  } else if (OB_ISNULL(inner_conn_guard_.get_ptr())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    /**
     * session is the only data struct which can pass through multi layer nested sql,
     * so we put inner conn in session to share it within multi layer nested sql.
     */
    session->set_inner_conn(inner_conn_guard_.get_ptr());
    need_close_conn_ = true;
  }
  if (OB_SUCC(ret)) {
    inner_conn_ = as_inner_sql_connection(session->get_inner_conn());
    if (OB_ISNULL(inner_conn_)) {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

int ObTableModifyOp::close_inner_conn()
{
  /**
   * we can call it even if open_inner_conn() failed, because only the one who call
   * open_inner_conn() succeed first will do close job by "if (need_close_conn_)".
   */
  int ret = OB_SUCCESS;
  if (need_close_conn_) {
    ObSQLSessionInfo *session = ctx_.get_my_session();
    if (OB_ISNULL(sql_proxy_) || OB_ISNULL(session)) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      OX(session->set_inner_conn(NULL));
      OX(inner_conn_guard_.reset());
    }
    need_close_conn_ = false;
  }
  sql_proxy_ = NULL;
  inner_conn_guard_.reset();
  inner_conn_ = NULL;
  return ret;
}

int ObTableModifyOp::begin_nested_session(bool skip_cur_stmt_tables)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(inner_conn_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(inner_conn_->begin_nested_session(get_saved_session(), saved_conn_,
                                                       skip_cur_stmt_tables))) {
  }
  return ret;
}

int ObTableModifyOp::end_nested_session()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(inner_conn_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(inner_conn_->end_nested_session(get_saved_session(), saved_conn_))) {
  }
  return ret;
}
int ObTableModifyOp::execute_write(const char *sql)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  if (OB_ISNULL(inner_conn_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(sql)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(inner_conn_->execute_write(sql, affected_rows))) {
  }
  return ret;
}

int ObTableModifyOp::execute_read(const char *sql, ObMySQLProxy::MySQLResult &res)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(inner_conn_) || OB_ISNULL(sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(sql)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(inner_conn_->execute_read(sql, res))) {
  }
 return ret;
}

int ObTableModifyOp::check_stack()
{
  int ret = OB_SUCCESS;
  const int max_stack_deep = 16;
  bool is_stack_overflow = false;
  ObSQLSessionInfo *session = ctx_.get_my_session();
  if (OB_ISNULL(session)) {
    ret = OB_ERR_NULL_VALUE;
  } else if (session->get_nested_count() > max_stack_deep) {
    ret = OB_SIZE_OVERFLOW;
  } else if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
  }
  return ret;
}
OperatorOpenOrder ObTableModifyOp::get_operator_open_order() const
{
  OperatorOpenOrder open_order = OPEN_CHILDREN_FIRST;
  if (spec_.plan_->is_use_px()) {
    // If PX is enabled in DML, there are two scenarios:
    // 1. PDML: in pdml, pdml-op completely does not need its corresponding input to provide runtime parameters, so it will directly return
    // open_order = OPEN_CHILDREN_FIRST
    // 2. DML+PX: In this case, the parameters of the dml run are completely fed by the GI operator above it, for example, such a plan:
    //   PX COORD
    //    PX TRANSMIT
    //      GI (FULL PARTITION WISE)
    //       DELETE
    //        TSC
    //  Such a plan requires GI to drive the deletion, GI rescan delete each time a new Task is iterated,
    //  Pass the information of the corresponding Task to the delete operator.
    open_order = OPEN_CHILDREN_FIRST;
  }
  return open_order;
}

int ObTableModifyOp::submit_all_dml_task()
{
  int ret = OB_SUCCESS;
  if (dml_rtctx_.das_ref_.has_task()) {
    if (dml_rtctx_.need_pick_del_task_first() &&
                OB_FAIL(dml_rtctx_.das_ref_.pick_del_task_to_first())) {
    } else if (OB_FAIL(dml_rtctx_.das_ref_.execute_all_task())) {
    } else if (OB_FAIL(dml_rtctx_.das_ref_.close_all_task())) {
    } else if (OB_FAIL(ObDMLService::handle_after_row_processing(this, &get_dml_modify_row_list()))) {
    } else {
      dml_modify_rows_.clear();
      dml_rtctx_.reuse();
    }
  }
  return ret;
}

//The data to be written by DML will be buffered in the DAS Write Buffer
//When the buffer data exceeds 6M,
//needs to be written to the storage to release the memory.
int ObTableModifyOp::discharge_das_write_buffer()
{
  int ret = OB_SUCCESS;
  if (dml_rtctx_.need_submit_all_tasks()) {
    LOG_INFO("DASWriteBuffer full, now to write storage",
             "buffer memory", dml_rtctx_.das_ref_.get_das_alloc().used(),
             K(dml_rtctx_.get_das_task_memory_size()), K(dml_rtctx_.get_das_parallel_task_size()));
    ret = submit_all_dml_task();
  } else if (execute_single_row_) {
    if (REACH_COUNT_INTERVAL(100)) { // print log per 100 times.
    }
    ret = submit_all_dml_task();
  }
  return ret;
}

int ObTableModifyOp::get_next_row_from_child()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (OB_FAIL(child_->get_next_row())) {
    if (OB_ITER_END != ret) {
    }
  } else {
    LOG_DEBUG("child output row", "row", ROWEXPR2STR(eval_ctx_, child_->get_spec().output_));
  }
  return ret;
}

int ObTableModifyOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  if (iter_end_) {
    ret = OB_ITER_END;
  } else {
    int64_t row_count = 0;
    while (OB_SUCC(ret)) {
      if (OB_FAIL(try_check_status())) {
      } else if (OB_FAIL(get_next_row_from_child())) {
        if (OB_ITER_END != ret) {
        } else {
          iter_end_ = true;
          ret = OB_SUCCESS;
          break;
        }
      } else if (OB_FAIL(write_row_to_das_buffer())) {
      } else if (OB_FAIL(discharge_das_write_buffer())) {
      } else if (MY_SPEC.is_returning_) {
        break;
      }
      row_count ++;
    }

    if (OB_FAIL(ret)) {
      record_err_for_load_data(ret, row_count);
    }

    if (OB_SUCC(ret) && iter_end_ && dml_rtctx_.das_ref_.has_task()) {
      //DML operator reach iter end,
      //now submit the remaining rows in the DAS Write Buffer to the storage
      if (OB_FAIL(submit_all_dml_task())) {
      }
    }
    //to post process the DML info after writing all data to the storage or returning one row
    ret = write_rows_post_proc(ret);
    if (OB_SUCC(ret) && iter_end_) {
      ret = OB_ITER_END;
    }
  }
  return ret;
}

int ObTableModifyOp::perform_batch_fk_check()
{
  DEBUG_SYNC(BEFORE_FOREIGN_KEY_CONSTRAINT_CHECK);
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < fk_checkers_.count(); ++i) {
    bool all_has_result = false;
    ObForeignKeyChecker *fk_checker = fk_checkers_.at(i);
    if (OB_ISNULL(fk_checker)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(fk_checker->do_fk_check_batch(all_has_result))) {
    } else if (!all_has_result) {
      ret = OB_ERR_NO_REFERENCED_ROW;
    }
  }
  return ret;
}
// --------------------------- AI COLUMN async task enqueue ---------------------------
namespace
{
const char AI_SPEC_PREFIX[] = "ai_column|";
const char AI_SPEC_ASYNC[] = "sync_mode=async";
const char AI_EXPR_KEY[] = "normalized_expr=";
const char AI_REQUEST_PREFIX[] = "__ai_request_";
const int64_t AI_REQUEST_COL_PREFIX_LEN = sizeof(AI_REQUEST_PREFIX) - 1;

bool contains_ci(const ObString &hay, const char *needle)
{
  bool found = false;
  const int64_t needle_len = strlen(needle);
  for (int64_t i = 0; !found && i + needle_len <= hay.length(); ++i) {
    found = (0 == strncasecmp(hay.ptr() + i, needle, needle_len));
  }
  return found;
}

int get_expr_text(const ObString &comment, ObString &expr_text)
{
  int ret = OB_SUCCESS;
  const int64_t key_len = strlen(AI_EXPR_KEY);
  int64_t pos = -1;
  for (int64_t i = 0; i + key_len <= comment.length() && pos < 0; ++i) {
    if (0 == strncmp(comment.ptr() + i, AI_EXPR_KEY, key_len)) {
      pos = i + key_len;
    }
  }
  if (pos < 0 || pos >= comment.length()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    expr_text = ObString(static_cast<int32_t>(comment.length() - pos), comment.ptr() + pos);
  }
  return ret;
}

bool is_word_char(const char c)
{
  return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9') || '_' == c;
}

// "concat('Classify: ', content)" mentions content, so content is a dependency: an UPDATE
// that does not assign it must not enqueue a task (asserted by the public t1_dml case).
int find_word(const ObString &text, const ObString &word, bool &found)
{
  int ret = OB_SUCCESS;
  found = false;
  for (int64_t i = 0; !found && i + word.length() <= text.length(); ++i) {
    bool match = true;
    for (int64_t k = 0; match && k < word.length(); ++k) {
      const char a = text.ptr()[i + k];
      const char b = word.ptr()[k];
      match = (a == b || (is_word_char(a) && is_word_char(b)
                         && ::tolower(a) == ::tolower(b)));
    }
    if (match && i > 0 && is_word_char(text.ptr()[i - 1])) {
      match = false;
    }
    if (match && i + word.length() < text.length() && is_word_char(text.ptr()[i + word.length()])) {
      match = false;
    }
    found = match;
  }
  return ret;
}

bool has_ai_companion(const share::schema::ObTableSchema *table_schema)
{
  bool found = false;
  if (OB_NOT_NULL(table_schema)) {
    for (share::schema::ObTableSchema::const_column_iterator it = table_schema->column_begin();
         !found && it != table_schema->column_end(); ++it) {
      const share::schema::ObColumnSchemaV2 *col = *it;
      if (OB_NOT_NULL(col) && col->get_column_name_str().prefix_match(AI_REQUEST_PREFIX)
          && contains_ci(col->get_comment_str(), AI_SPEC_ASYNC)) {
        found = true;
      }
    }
  }
  return found;
}

int fetch_task_id(int64_t &task_id)
{
  int ret = OB_SUCCESS;
  share::ObCommonID id;
  if (OB_FAIL(storage::ObCommonIDUtils::gen_unique_id(id))) {
    LOG_WARN("fail to gen ai task id", K(ret));
  } else {
    task_id = id.id();
  }
  return ret;
}
} // namespace

int AiColumnHandle::has_async_ai_column(ObExecContext &ctx, const uint64_t table_id, bool &has_async)
{
  int ret = OB_SUCCESS;
  has_async = false;
  share::schema::ObSchemaGetterGuard *schema_guard = NULL;
  const share::schema::ObTableSchema *table_schema = NULL;
  if (OB_INVALID_ID == table_id) {
    // nothing to key on
  } else if (OB_ISNULL(schema_guard = ctx.get_sql_ctx()->schema_guard_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(schema_guard->get_table_schema(table_id, table_schema))) {
    LOG_WARN("fail to get table schema", K(ret), K(table_id));
  } else {
    has_async = has_ai_companion(table_schema);
  }
  return ret;
}

int AiColumnHandle::do_handle_after_row(ObTableModifyOp &op,
                                        const ObDMLBaseCtDef &dml_ctdef,
                                        ObDMLBaseRtDef &dml_rtdef,
                                       const bool is_update,
                                       const bool is_delete)
{
  int ret = OB_SUCCESS;
  UNUSED(dml_rtdef);
  AiEnqueueGuard ai_guard;
  ObExecContext &ctx = op.get_exec_ctx();
  ObSQLSessionInfo *session = ctx.get_my_session();
  share::schema::ObSchemaGetterGuard *schema_guard = NULL;
  const share::schema::ObTableSchema *table_schema = NULL;
  // das_base_ctdef_.table_id_ is the table *loc* id and index_tid_ the reference id;
  // which one is the schema that carries the AI companions depends on the table type
  const uint64_t candidates[2] = { dml_ctdef.das_base_ctdef_.table_id_,
                                   dml_ctdef.das_base_ctdef_.index_tid_ };
  uint64_t table_id = OB_INVALID_ID;
  if (OB_ISNULL(session) || OB_ISNULL(schema_guard = ctx.get_sql_ctx()->schema_guard_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    for (int64_t c = 0; OB_SUCC(ret) && c < 2 && OB_INVALID_ID == table_id; ++c) {
      const share::schema::ObTableSchema *try_schema = NULL;
      if (OB_INVALID_ID == candidates[c]) {
      } else if (OB_FAIL(schema_guard->get_table_schema(candidates[c], try_schema))) {
        LOG_WARN("fail to get table schema for ai column", K(ret), "candidate", candidates[c]);
      } else if (OB_NOT_NULL(try_schema) && has_ai_companion(try_schema)) {
        table_schema = try_schema;
        table_id = candidates[c];
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(table_schema)) {
    // this table has no async AI columns, or the schema vanished
  } else {
    common::ObArenaAllocator alloc("AiColEnq");
    int64_t request_id = 0;
    bool have_request_id = false;
    common::ObSqlString db_name;
    const share::schema::ObDatabaseSchema *db_schema = NULL;
    if (OB_FAIL(schema_guard->get_database_schema(table_schema->get_database_id(), db_schema))) {
      LOG_WARN("fail to get database schema", K(ret));
    } else if (OB_ISNULL(db_schema)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(db_name.assign(db_schema->get_database_name_str()))) {
      LOG_WARN("fail to copy database name", K(ret));
    }
    // the row key predicate, as text, is what the worker later re-runs. For INSERT the
    // values live in new_row_ (full_row_ is empty there), so use whichever array is the
    // one paired with column_ids_
    const ExprFixedArray &row = is_delete
        ? dml_ctdef.old_row_
        : ((dml_ctdef.new_row_.count() > 0
            && dml_ctdef.new_row_.count() == dml_ctdef.column_ids_.count())
           ? dml_ctdef.new_row_ : dml_ctdef.full_row_);
    common::ObSqlString predicate;
    for (int64_t i = 0; OB_SUCC(ret) && i < row.count()
         && i < dml_ctdef.column_ids_.count(); ++i) {
      const share::schema::ObColumnSchemaV2 *col = NULL;
      const ObExpr *expr = row.at(i);
      if (OB_ISNULL(expr)
          || OB_ISNULL(col = table_schema->get_column_schema(dml_ctdef.column_ids_.at(i)))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (!col->is_rowkey_column()) {
        // not part of the row key
      } else {
        ObDatum *datum = NULL;
        ObObj obj;
        char *buf = NULL;
        int64_t buf_len = 0;
        int64_t pos = 0;
        if (OB_FAIL(expr->eval(op.get_eval_ctx(), datum))) {
          LOG_WARN("fail to eval rowkey expr", K(ret));
        } else if (OB_ISNULL(datum) || datum->is_null()) {
          ret = OB_ERR_NULL_VALUE;
        } else if (OB_FAIL(datum->to_obj(obj, col->get_meta_type()))) {
          LOG_WARN("fail to cast rowkey datum", K(ret));
        } else if (OB_FAIL(obj.print_sql_literal(buf, buf_len, pos, alloc,
                                                 op.get_obj_print_params()))) {
          LOG_WARN("fail to print rowkey literal", K(ret));
        } else if (predicate.length() > 0 && OB_FAIL(predicate.append(" AND "))) {
          LOG_WARN("fail to append predicate", K(ret));
        } else if (OB_FAIL(predicate.append_fmt("`%.*s` = %s", col->get_column_name_str().length(),
                                                col->get_column_name_str().ptr(), buf))) {
          LOG_WARN("fail to build rowkey predicate", K(ret));
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (predicate.length() <= 0) {
      // must never fail the user's DML: an AI column whose task could not be enqueued only
      // leaves its result column NULL until the enqueue source is fixed
      ret = OB_SUCCESS;
      LOG_WARN("ai column task skipped: no row key predicate", K(table_id),
               "full_row_count", dml_ctdef.full_row_.count(),
               "new_row_count", dml_ctdef.new_row_.count(),
               "old_row_count", dml_ctdef.old_row_.count(),
               "column_ids_count", dml_ctdef.column_ids_.count());
    } else {
      for (share::schema::ObTableSchema::const_column_iterator it = table_schema->column_begin();
           OB_SUCC(ret) && it != table_schema->column_end(); ++it) {
        const share::schema::ObColumnSchemaV2 *req_col = *it;
        ObString comment;
        ObString expr_text;
        const share::schema::ObColumnSchemaV2 *result_col = NULL;
        bool dep_touched = !is_update;
        if (OB_ISNULL(req_col) || !req_col->get_column_name_str().prefix_match(AI_REQUEST_PREFIX)) {
          // not an AI request companion column
        } else if (!contains_ci(req_col->get_comment_str(), AI_SPEC_PREFIX)) {
          // not an AI column companion
        } else if (!contains_ci(req_col->get_comment_str(), AI_SPEC_ASYNC)) {
          // synchronous AI columns are answered inline, no task row
        } else if (OB_FAIL(get_expr_text(req_col->get_comment_str(), expr_text))) {
          LOG_WARN("fail to find ai expression", K(ret), K(req_col->get_column_name_str()));
        } else if (FALSE_IT(comment = req_col->get_comment_str())) {
        } else if (OB_ISNULL(result_col = table_schema->get_column_schema(
                       ObString(req_col->get_column_name_str().length() - AI_REQUEST_COL_PREFIX_LEN,
                                req_col->get_column_name_str().ptr() + AI_REQUEST_COL_PREFIX_LEN)))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("ai column result column is missing", K(ret));
        } else if (is_update) {
          // Only the columns the statement assigns may invalidate a cached answer. The full
          // column_ids_ array also carries the untouched columns, and the AI worker writes its
          // own answer back with an UPDATE, which would otherwise re-enqueue the task forever.
          const bool is_plain_update =
              (DAS_OP_TABLE_UPDATE == dml_ctdef.das_base_ctdef_.op_type_);
          if (!is_plain_update) {
            dep_touched = true;
          } else {
            const ObUpdCtDef &upd_ctdef = static_cast<const ObUpdCtDef &>(dml_ctdef);
            for (int64_t k = 0;
                 OB_SUCC(ret) && !dep_touched && k < upd_ctdef.assign_columns_.count(); ++k) {
              const uint64_t idx = upd_ctdef.assign_columns_.at(k).projector_index_;
              const share::schema::ObColumnSchemaV2 *assigned = NULL;
              bool mentions = false;
              if (idx >= static_cast<uint64_t>(dml_ctdef.column_ids_.count())) {
                ret = OB_ERR_UNEXPECTED;
                LOG_WARN("unexpected ai column projector index", K(ret), K(idx),
                         "column_ids_count", dml_ctdef.column_ids_.count());
              } else if (OB_ISNULL(assigned = table_schema->get_column_schema(
                             dml_ctdef.column_ids_.at(idx)))) {
                ret = OB_ERR_UNEXPECTED;
                LOG_WARN("fail to get assigned column schema", K(ret), "column_id",
                         dml_ctdef.column_ids_.at(idx));
              } else if (assigned->get_column_id() == result_col->get_column_id()) {
                // writing NULL into the answer column on purpose (ALTER TABLE ... RETRY AI
                // COLUMN does exactly that) is an invalidation; a real answer arriving is the
                // worker finishing, and must not queue another request
                ObDatum *cell_datum = NULL;
                const ObExpr *cell = (idx < static_cast<uint64_t>(row.count()))
                    ? row.at(idx) : NULL;
                if (OB_ISNULL(cell)) {
                  ret = OB_ERR_UNEXPECTED;
                  LOG_WARN("no expression for the ai answer cell", K(ret), K(idx));
                } else if (OB_FAIL(cell->eval(op.get_eval_ctx(), cell_datum))) {
                  LOG_WARN("fail to eval the ai answer cell", K(ret));
                } else if (OB_NOT_NULL(cell_datum) && cell_datum->is_null()) {
                  dep_touched = true;
                }
              } else if (assigned->is_rowkey_column()) {
                // the row moved to a new key, its answer has to be regenerated under that key
                dep_touched = true;
              } else if (OB_FAIL(find_word(expr_text, assigned->get_column_name_str(), mentions))) {
                LOG_WARN("fail to scan ai expression", K(ret));
              } else if (mentions) {
                dep_touched = true;
              }
            }
          }
        }
        if (OB_FAIL(ret) || OB_ISNULL(result_col) || !dep_touched) {
        } else {
          int64_t task_id = 0;
          if (!have_request_id) {
            if (OB_FAIL(fetch_task_id(request_id))) {
              LOG_WARN("fail to gen ai request id", K(ret));
            } else {
              have_request_id = true;
            }
          }
          const ObString pred_str(predicate.length(), predicate.ptr());
          task_id = ai_task_slot_id(table_id, result_col->get_column_id(), pred_str);
          if (OB_FAIL(ret)) {
          } else if (is_delete) {
            // the row is going away, and so is the task which was pending for it
            if (OB_FAIL(op.delete_ai_column_task_row(task_id))) {
              LOG_WARN("fail to remove the pending ai column task", K(ret), K(table_id),
                       "column_id", result_col->get_column_id(), K(predicate));
              ret = OB_SUCCESS;
            }
          } else if (OB_FAIL(op.insert_ai_column_task_row(task_id, table_id,
                                                          result_col->get_column_id(),
                                                          pred_str,
                                                          request_id))) {
            LOG_WARN("fail to enqueue ai column task", K(ret), K(task_id), K(table_id),
                     K(predicate), K(request_id));
            ret = OB_SUCCESS;
          } else if (!is_update) {
            // a new row has no stored answer to hide
          } else {
            // the plan already wrote the NULL into the cached answer, on this same row
          }
        }
      }
    }
  }
  return ret;
}

}  // namespace sql
}  // namespace oceanbase
