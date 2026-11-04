/**
 * Copyright (c) 2025 OceanBase.
 * SPDX-License-Identifier: Apache-2.0
 */

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_file_table_op.h"
#include "sql/engine/ob_exec_context.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace sql
{

OB_SERIALIZE_MEMBER((ObFileTableSpec, ObOpSpec), value_expr_, column_exprs_,
                    format_, describe_, has_correlated_expr_);

ObString ObFileTableSpec::format_name() const
{
  ObString name;
  switch (format_) {
    case FILE_FMT_CSV: name = ObString("csv"); break;
    case FILE_FMT_JSONL: name = ObString("jsonl"); break;
    case FILE_FMT_PARQUET: name = ObString("parquet"); break;
    case FILE_FMT_GLOB: name = ObString("glob"); break;
    default: name = ObString(""); break;
  }
  return name;
}

int ObFileTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_open())) {
    LOG_WARN("fail to open base operator", K(ret));
  } else if (OB_FAIL(load_rows())) {
    LOG_WARN("fail to load file rows", K(ret));
  }
  return ret;
}

int ObFileTableOp::load_rows()
{
  int ret = OB_SUCCESS;
  const ObFileTableSpec &my_spec = static_cast<const ObFileTableSpec &>(spec_);
  rows_alloc_.reset();
  schema_.reset();
  rows_.reset();
  cursor_ = 0;
  ObEvalCtx &ctx = eval_ctx_;
  if (OB_ISNULL(my_spec.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file table has no path expr", K(ret));
  } else {
    ObDatum *path_datum = nullptr;
    if (OB_FAIL(my_spec.value_expr_->eval(ctx, path_datum))) {
      if (OB_ITER_END == ret) { ret = OB_SUCCESS; }
      else { LOG_WARN("fail to eval file path", K(ret)); }
    } else if (OB_ISNULL(path_datum)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const ObString path = path_datum->get_string();
      ObFileTableFormat fmt = static_cast<ObFileTableFormat>(my_spec.format_);
      if (FILE_FMT_UNKNOWN == fmt) {
        fmt = ObFileTableReader::guess_format(path.ptr(), path.length());
      }
      if (OB_FAIL(ObFileTableReader::load(path.ptr(), path.length(), fmt,
                                          -1 /* infer from the whole file */,
                                          my_spec.describe_,
                                          rows_alloc_, schema_, rows_))) {
        LOG_WARN("fail to read file", K(ret), K(path), K(my_spec.format_));
      }
    }
  }
  return ret;
}

int ObFileTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
    LOG_WARN("fail to rescan base operator", K(ret));
  } else if (OB_FAIL(load_rows())) {
    LOG_WARN("fail to reload file rows", K(ret));
  }
  return ret;
}

int ObFileTableOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  const ObFileTableSpec &my_spec = static_cast<const ObFileTableSpec &>(spec_);
  if (cursor_ >= rows_.count()) {
    ret = OB_ITER_END;
  } else if (OB_UNLIKELY(rows_.at(cursor_).count() != my_spec.column_exprs_.count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file row width does not match output columns", K(ret),
             K(cursor_), K(rows_.at(cursor_).count()), K(my_spec.column_exprs_.count()));
  } else {
    clear_evaluated_flag();
    for (int64_t i = 0; OB_SUCC(ret) && i < my_spec.column_exprs_.count(); ++i) {
      ObExpr *col = my_spec.column_exprs_.at(i);
      if (OB_ISNULL(col)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObDatum &datum = col->locate_datum_for_write(eval_ctx_);
        const ObObj &value = rows_.at(cursor_).at(i);
        if (value.is_null()) {
          datum.set_null();
        } else if (OB_FAIL(datum.from_obj(value))) {
          LOG_WARN("fail to write file column datum", K(ret), K(i), K(value));
        }
        col->set_evaluated_projected(eval_ctx_);
      }
    }
    if (OB_SUCC(ret)) { ++cursor_; }
  }
  return ret;
}

int ObFileTableOp::inner_close()
{
  rows_alloc_.reset();
  schema_.reset();
  rows_.reset();
  return OB_SUCCESS;
}

void ObFileTableOp::destroy()
{
  inner_close();
  ObOperator::destroy();
}

} // namespace sql
} // namespace oceanbase
