/**
 * Copyright (c) 2025 OceanBase.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_file_table_reader.h"
#include "lib/container/ob_fixed_array.h"

namespace oceanbase
{
namespace sql
{

// Physical spec of a file table function (read_csv / read_json / read_parquet / glob).
// Shape follows ObFunctionTableSpec rather than ObJsonTableSpec: there are no JSON
// paths or nested columns, only one path expression and the projected output columns.
class ObFileTableSpec : public ObOpSpec
{
  OB_UNIS_VERSION_V(1);
public:
  ObFileTableSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type),
      value_expr_(nullptr),
      column_exprs_(alloc),
      format_(0),
      describe_(false),
      has_correlated_expr_(false)
  {}
  ObExpr *value_expr_;                                // the file path, a const expr
  common::ObFixedArray<ObExpr *, common::ObIAllocator> column_exprs_;
  int64_t format_;                                    // ObFileTableFormat
  bool describe_;                                     // DESCRIBE SELECT ... shape
  bool has_correlated_expr_;
  common::ObString format_name() const;
};

class ObFileTableOp : public ObOperator
{
public:
  ObFileTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input),
      rows_alloc_(),
      schema_(),
      rows_(),
      cursor_(0)
  {}
  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  virtual int inner_close() override;
  virtual void destroy() override;
private:
  int load_rows();
  common::ObArenaAllocator rows_alloc_;
  common::ObSEArray<ObFileTableColumn, 16> schema_;
  common::ObSEArray<ObFileTableRow, 8> rows_;
  int64_t cursor_;
};

} // namespace sql
} // namespace oceanbase
