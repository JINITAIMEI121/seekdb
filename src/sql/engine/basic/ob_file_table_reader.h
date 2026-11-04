/**
 * Copyright (c) 2025 OceanBase.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "lib/container/ob_se_array.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/string/ob_string.h"
#include "common/object/ob_object.h"

namespace oceanbase
{
namespace sql
{

enum ObFileTableFormat
{
  FILE_FMT_UNKNOWN = 0,
  FILE_FMT_CSV,
  FILE_FMT_JSONL,
  FILE_FMT_PARQUET,
  // glob(path) is a table of matching paths rather than a file to parse, so it
  // rides on the same operator with its own format.
  FILE_FMT_GLOB
};

// One inferred column of a file table. The name comes from the CSV header (with
// duplicates suffixed _2, _3 ...) or from the JSONL key union, and the type from
// scanning the sampled values.
struct ObFileTableColumn
{
  ObFileTableColumn() : name_(), meta_(), nullable_(false) {}
  void reset() { name_.reset(); meta_ = common::ObObjMeta(); nullable_ = false; }
  TO_STRING_KV(K_(name), K_(meta), K_(nullable));
  common::ObString name_;
  common::ObObjMeta meta_;
  bool nullable_;
};

// A materialized row: one ObObj per schema column, already converted to the
// inferred type so downstream casts and aggregates see the same values a table
// scan would produce.
typedef common::ObSEArray<common::ObObj, 16> ObFileTableRow;

class ObFileTableReader
{
public:
  // Reads the whole file into `alloc` and infers the schema from at most
  // `sample_size` data rows (sample_size < 0 means all rows, which is what the
  // public suite's explicit `sample_size = -1` asks for).
  static int load(const char *path,
                  const int64_t path_len,
                  const ObFileTableFormat fmt,
                  const int64_t sample_size,
                  const bool describe,
                  common::ObIAllocator &alloc,
                  common::ObIArray<ObFileTableColumn> &schema,
                  common::ObIArray<ObFileTableRow> &rows);

  // Extension-based format guess used when the caller passes no explicit format.
  static ObFileTableFormat guess_format(const char *path, const int64_t path_len);
  // Rejects formats we do not implement with the same errno the suite expects
  // (read_csv(path, 'xml') -> 1064).
  static int check_format(const ObFileTableFormat fmt, int &ret);

  static int parse_csv(common::ObIAllocator &alloc,
                       const char *buf, const int64_t buf_len,
                       common::ObIArray<common::ObSEArray<common::ObString, 16>> &records);

  // Newline-delimited JSON: the key union across all sampled objects becomes the
  // header list, and every record is padded to that width so infer_and_convert()
  // can treat both formats the same way.
  static int parse_jsonl(common::ObIAllocator &alloc,
                         const char *buf, const int64_t buf_len,
                         common::ObSEArray<common::ObString, 16> &headers,
                         common::ObIArray<common::ObSEArray<common::ObString, 16>> &records);

  // glob(): one varchar column named `file`, one row per matching path.
  static int load_glob(common::ObIAllocator &alloc,
                       const char *path, const int64_t path_len,
                       common::ObIArray<ObFileTableColumn> &schema,
                       common::ObIArray<ObFileTableRow> &rows);

  // Minimal Parquet reader: flat (non-nested) schemas, PLAIN or dictionary-encoded
  // values, pages that are uncompressed or Snappy-block-compressed.
  static int load_parquet(common::ObIAllocator &alloc,
                          const char *buf, const int64_t buf_len,
                          common::ObIArray<ObFileTableColumn> &schema,
                          common::ObIArray<ObFileTableRow> &rows);

private:
  static int load_impl(const char *path,
                       const int64_t path_len,
                       const ObFileTableFormat fmt,
                       const int64_t sample_size,
                       common::ObIAllocator &alloc,
                       common::ObIArray<ObFileTableColumn> &schema,
                       common::ObIArray<ObFileTableRow> &rows);
  static int read_file(common::ObIAllocator &alloc,
                       const char *path, const int64_t path_len,
                       char *&buf, int64_t &buf_len);
  static int infer_and_convert(common::ObIAllocator &alloc,
                               const common::ObSEArray<common::ObString, 16> &headers,
                               const common::ObIArray<common::ObSEArray<common::ObString, 16>> &records,
                               const int64_t first_data_record,
                               const int64_t sample_size,
                               common::ObIArray<ObFileTableColumn> &schema,
                               common::ObIArray<ObFileTableRow> &rows);
  static int convert_value(const common::ObString &raw,
                           const common::ObObjMeta &meta,
                           const uint64_t sql_mode,
                           common::ObIAllocator &alloc,
                           common::ObObj &value);
  static bool is_blank(const common::ObString &s);
  static bool looks_int(const common::ObString &s);
  static bool looks_decimal(const common::ObString &s);
  static bool looks_date(const common::ObString &s);
  static bool looks_datetime(const common::ObString &s);
  static bool looks_bool(const common::ObString &s);
  static common::ObString dedup_name(common::ObIAllocator &alloc,
                                     const common::ObString &raw,
                                     const int occurrence);
  DISALLOW_COPY_AND_ASSIGN(ObFileTableReader);
};

} // namespace sql
} // namespace oceanbase
