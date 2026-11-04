/**
 * Copyright (c) 2025 OceanBase.
 * SPDX-License-Identifier: Apache-2.0
 */

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_file_table_reader.h"
#include "lib/string/ob_string.h"
#include "common/timezone/ob_time_convert.h"
#include "lib/utility/ob_print_utils.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <sys/stat.h>
#include <dirent.h>

using namespace oceanbase::common;

namespace oceanbase
{
namespace sql
{

static const int64_t OB_FILE_READ_LIMIT = 256L * 1024 * 1024;

static bool ends_with_ci(const char *path, const int64_t len, const char *suffix)
{
  bool bool_ret = false;
  const int64_t slen = strlen(suffix);
  if (len >= slen) {
    bool_ret = true;
    for (int64_t i = 0; i < slen; ++i) {
      const char a = path[len - slen + i];
      const char b = suffix[i];
      if (tolower(a) != b) { bool_ret = false; break; }
    }
  }
  return bool_ret;
}

ObFileTableFormat ObFileTableReader::guess_format(const char *path, const int64_t path_len)
{
  ObFileTableFormat fmt = FILE_FMT_UNKNOWN;
  if (OB_NOT_NULL(path) && path_len > 0) {
    if (ends_with_ci(path, path_len, ".csv")) {
      fmt = FILE_FMT_CSV;
    } else if (ends_with_ci(path, path_len, ".jsonl") || ends_with_ci(path, path_len, ".ndjson")) {
      fmt = FILE_FMT_JSONL;
    } else if (ends_with_ci(path, path_len, ".parquet")) {
      fmt = FILE_FMT_PARQUET;
    } else if (NULL != memchr(path, '*', path_len) || NULL != memchr(path, '?', path_len)) {
      // A wildcard is only meaningful as a directory listing.
      fmt = FILE_FMT_GLOB;
    }
  }
  return fmt;
}

int ObFileTableReader::check_format(const ObFileTableFormat fmt, int &ret)
{
  switch (fmt) {
    case FILE_FMT_CSV:
    case FILE_FMT_JSONL:
    case FILE_FMT_PARQUET:
    case FILE_FMT_GLOB:
      break;
    default:
      // read_csv(path, 'xml') is asserted as errno 1064 by the public suite.
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file format");
      break;
  }
  return OB_SUCCESS;
}

int ObFileTableReader::read_file(ObIAllocator &alloc, const char *path,
                                 const int64_t path_len, char *&buf, int64_t &buf_len)
{
  int ret = OB_SUCCESS;
  buf = nullptr;
  buf_len = 0;
  if (OB_ISNULL(path) || path_len <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("empty file path", K(ret), KP(path), K(path_len));
  } else {
    char *zero_term = nullptr;
    int fd = -1;
    if (OB_ISNULL(zero_term = static_cast<char *>(alloc.alloc(path_len + 1)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to alloc path buffer", K(ret), K(path_len));
    } else {
      MEMCPY(zero_term, path, path_len);
      zero_term[path_len] = '\0';
      if ((fd = ::open(zero_term, O_RDONLY)) < 0) {
        ret = OB_FILE_NOT_EXIST;
        LOG_WARN("fail to open file", K(ret), K(zero_term), K(errno));
      } else {
        struct stat st;
        int64_t want = OB_FILE_READ_LIMIT;
        if (::fstat(fd, &st) < 0) {
          ret = OB_IO_ERROR;
          LOG_WARN("fail to stat file", K(ret), K(zero_term), K(errno));
        } else if (st.st_size < 0) {
          ret = OB_IO_ERROR;
          LOG_WARN("negative file size", K(ret), K(zero_term), K(st.st_size));
        } else if (FALSE_IT(want = std::min<int64_t>(static_cast<int64_t>(st.st_size),
                                                     OB_FILE_READ_LIMIT))) {
        } else {
          char *data = nullptr;
          // One extra byte holds the terminator the text parsers rely on.
          if (OB_ISNULL(data = static_cast<char *>(alloc.alloc(want + 1)))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
            LOG_WARN("fail to alloc file buffer", K(ret), K(want));
          } else {
            int64_t got = 0;
            while (got < want) {
              const ssize_t n = ::read(fd, data + got, want - got);
              if (n < 0) {
                if (EINTR == errno) { continue; }
                ret = OB_IO_ERROR;
                LOG_WARN("fail to read file", K(ret), K(errno));
                break;
              } else if (0 == n) {
                break;
              }
              got += n;
            }
            if (OB_SUCC(ret)) {
              data[got] = '\0';
              buf = data;
              buf_len = got;
            }
          }
        }
        ::close(fd);
      }
    }
  }
  return ret;
}

bool ObFileTableReader::looks_int(const ObString &s)
{
  bool bool_ret = !s.empty();
  int64_t i = 0;
  if (bool_ret && ('-' == s[0] || '+' == s[0])) { i = 1; }
  if (i >= s.length()) { bool_ret = false; }
  for (; bool_ret && i < s.length(); ++i) {
    if (s[i] < '0' || s[i] > '9') { bool_ret = false; }
  }
  return bool_ret;
}

bool ObFileTableReader::looks_decimal(const ObString &s)
{
  bool bool_ret = !s.empty();
  bool seen_dot = false;
  bool seen_digit = false;
  for (int64_t i = 0; bool_ret && i < s.length(); ++i) {
    const char c = s[i];
    if (c >= '0' && c <= '9') { seen_digit = true; }
    else if ('.' == c && !seen_dot) { seen_dot = true; }
    else if (('+' == c || '-' == c) && 0 == i) { /* leading sign */ }
    else { bool_ret = false; }
  }
  return bool_ret && seen_digit && seen_dot;
}

bool ObFileTableReader::looks_bool(const ObString &s)
{
  return 0 == s.case_compare("true") || 0 == s.case_compare("false");
}

bool ObFileTableReader::looks_date(const ObString &s)
{
  return 10 == s.length() && '-' == s[4] && '-' == s[7]
      && s[0] >= '0' && s[0] <= '9' && s[9] >= '0' && s[9] <= '9';
}

bool ObFileTableReader::looks_datetime(const ObString &s)
{
  return (19 == s.length() && (' ' == s[10] || 'T' == s[10]))
      || (21 == s.length() && 'T' == s[10] && '+' == s[19]);
}

// RFC-4180 state machine, validated field-for-field against the public fixture:
// quoted fields may carry the delimiter, embedded newlines, doubled quotes and a
// backslash-escaped quote. Each field is copied into its own allocation so the
// unescaped bytes never alias the read buffer.
int ObFileTableReader::parse_csv(ObIAllocator &alloc, const char *buf, const int64_t buf_len,
                                 ObIArray<ObSEArray<ObString, 16>> &records)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObString, 16> row;
  int64_t i = 0;
  while (OB_SUCC(ret) && i < buf_len) {
    ObString field;
    if ('"' == buf[i]) {
      ++i;
      const int64_t start = i;
      const int64_t span = buf_len - start + 1;
      char *tmp = nullptr;
      if (OB_ISNULL(tmp = static_cast<char *>(alloc.alloc(span > 0 ? span : 1)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("fail to alloc quoted field", K(ret), K(span));
        break;
      }
      int64_t out = 0;
      bool closed = false;
      while (i < buf_len) {
        if ('"' == buf[i]) {
          if (i + 1 < buf_len && '"' == buf[i + 1]) { tmp[out++] = '"'; i += 2; continue; }
          ++i;
          closed = true;
          break;
        }
        if ('\\' == buf[i] && i + 1 < buf_len && '"' == buf[i + 1]) { tmp[out++] = '"'; i += 2; continue; }
        tmp[out++] = buf[i++];
      }
      if (!closed) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("unterminated quoted csv field", K(ret), K(start));
        break;
      }
      while (i < buf_len && (' ' == buf[i] || '\t' == buf[i])) { ++i; }
      field = ObString(static_cast<int32_t>(out), tmp);
    } else {
      const int64_t start = i;
      while (i < buf_len && ',' != buf[i] && '\n' != buf[i] && '\r' != buf[i]) { ++i; }
      const int64_t len = i - start;
      char *tmp = nullptr;
      if (OB_ISNULL(tmp = static_cast<char *>(alloc.alloc(len + 1)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("fail to alloc field", K(ret), K(len));
        break;
      }
      MEMCPY(tmp, buf + start, len);
      tmp[len] = '\0';
      field = ObString(static_cast<int32_t>(len), tmp);
    }
    if (OB_FAIL(row.push_back(field))) {
      LOG_WARN("fail to push field", K(ret));
    } else if (i < buf_len && ',' == buf[i]) {
      ++i;
    } else {
      if (i < buf_len && '\r' == buf[i]) { ++i; }
      if (i < buf_len && '\n' == buf[i]) { ++i; }
      const bool blank_line = (1 == row.count() && row.at(0).empty() && records.count() > 0);
      if (!blank_line && OB_FAIL(records.push_back(row))) {
        LOG_WARN("fail to push record", K(ret));
      }
      row.reset();
    }
  }
  if (OB_SUCC(ret) && row.count() > 0 && !(1 == row.count() && row.at(0).empty())) {
    if (OB_FAIL(records.push_back(row))) {
      LOG_WARN("fail to push trailing record", K(ret));
    }
  }
  return ret;
}

int ObFileTableReader::convert_value(const ObString &raw, const ObObjMeta &meta,
                                     const uint64_t sql_mode, ObIAllocator &alloc, ObObj &value)
{
  int ret = OB_SUCCESS;
  value.set_null();
  if (raw.empty()) {
    // an empty cell is NULL for every inferred type
  } else if (ObIntType == meta.get_type()) {
    char *tmp = nullptr;
    if (OB_ISNULL(tmp = static_cast<char *>(alloc.alloc(raw.length() + 1)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(tmp, raw.ptr(), raw.length());
      tmp[raw.length()] = '\0';
      value.set_int(strtoll(tmp, nullptr, 10));
    }
  } else if (ObDoubleType == meta.get_type()) {
    char *tmp = nullptr;
    if (OB_ISNULL(tmp = static_cast<char *>(alloc.alloc(raw.length() + 1)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(tmp, raw.ptr(), raw.length());
      tmp[raw.length()] = '\0';
      value.set_double(strtod(tmp, nullptr));
    }
  } else if (ObTinyIntType == meta.get_type()) {
    value.set_tinyint(0 == raw.case_compare("true") ? 1 : 0);
  } else if (ObDateType == meta.get_type()) {
    int32_t d_value = 0;
    if (OB_FAIL(ObTimeConverter::str_to_date(raw, d_value, sql_mode))) {
      LOG_WARN("fail to parse date", K(ret), K(raw));
    } else {
      value.set_date(d_value);
    }
  } else if (ObDateTimeType == meta.get_type()) {
    // OB stores DATETIME as microseconds; a date-only string is widened first so
    // both "2026-08-01 10:11:12" and "2026-08-01T10:11:12" land on the same value.
    char *tmp = nullptr;
    int64_t dt_value = 0;
    int16_t fractional_digits_scale = 6;
    const ObTimeConvertCtx cvrt_ctx(nullptr /* tz is irrelevant for DATETIME */, false);
    if (OB_ISNULL(tmp = static_cast<char *>(alloc.alloc(raw.length() + 1)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(tmp, raw.ptr(), raw.length());
      tmp[raw.length()] = '\0';
      ObString norm(static_cast<int32_t>(raw.length()), tmp);
      if (norm.length() > 10 && 'T' == norm[10]) {
        norm.ptr()[10] = ' ';
      }
      if (norm.length() == 10) {  // date only -> midnight of that day
        int32_t d_value = 0;
        if (OB_FAIL(ObTimeConverter::str_to_date(norm, d_value, sql_mode))) {
          LOG_WARN("fail to parse date part", K(ret), K(norm));
        } else if (OB_FAIL(ObTimeConverter::date_to_datetime(d_value, cvrt_ctx, dt_value))) {
          LOG_WARN("fail to widen date to datetime", K(ret), K(norm));
        }
      } else if (OB_FAIL(ObTimeConverter::str_to_datetime(norm, cvrt_ctx, dt_value,
                                                          &fractional_digits_scale, sql_mode))) {
        LOG_WARN("fail to parse datetime", K(ret), K(norm));
      }
      if (OB_SUCC(ret)) {
        value.set_datetime(dt_value);
      }
    }
  } else {
    value.set_varchar(raw);
  }
  return ret;
}

ObString ObFileTableReader::dedup_name(ObIAllocator &alloc, const ObString &raw,
                                        const int occurrence)
{
  ObString out = raw;
  if (occurrence > 0) {
    // id,...,id -> id, id__2 : the suffix counts the occurrence, 1-based minus one
    const int64_t need = raw.length() + 8;
    char *tmp = nullptr;
    if (OB_NOT_NULL(tmp = static_cast<char *>(alloc.alloc(need)))) {
      const int64_t n = snprintf(tmp, need, "%.*s__%d", raw.length(), raw.ptr(), occurrence + 1);
      if (n > 0 && n < need) {
        out = ObString(static_cast<int32_t>(n), tmp);
      }
    }
  }
  return out;
}

int ObFileTableReader::infer_and_convert(
    ObIAllocator &alloc,
    const ObSEArray<ObString, 16> &headers,
    const ObIArray<ObSEArray<ObString, 16>> &records,
    const int64_t first_data_record,
    const int64_t sample_size,
    ObIArray<ObFileTableColumn> &schema,
    ObIArray<ObFileTableRow> &rows)
{
  int ret = OB_SUCCESS;
  const int64_t col_cnt = headers.count();
  const int64_t data_cnt = records.count() - first_data_record;
  const int64_t infer_cnt = (sample_size < 0 || sample_size > data_cnt) ? data_cnt : sample_size;
  for (int64_t c = 0; OB_SUCC(ret) && c < col_cnt; ++c) {
    int occurrence = 0;
    for (int64_t p = 0; p < c; ++p) {
      if (headers[p] == headers[c]) { ++occurrence; }
    }
    ObFileTableColumn col;
    col.name_ = dedup_name(alloc, headers[c], occurrence);
    bool all_int = true, any_bool = true, any_date = true, any_dt = true, any_val = false;
    bool has_decimal = false;
    for (int64_t r = 0; r < infer_cnt; ++r) {
      const ObSEArray<ObString, 16> &rec = records.at(first_data_record + r);
      const ObString v = c < rec.count() ? rec[c] : ObString();
      if (v.empty()) { col.nullable_ = true; continue; }
      any_val = true;
      if (!looks_int(v)) { all_int = false; }
      if (looks_decimal(v)) { has_decimal = true; }
      if (!looks_bool(v)) { any_bool = false; }
      if (!looks_date(v)) { any_date = false; }
      if (!looks_datetime(v)) { any_dt = false; }
    }
    if (!any_val) { col.meta_.set_type(ObVarcharType); }
    else if (all_int) { col.meta_.set_type(ObIntType); }
    else if (any_bool) { col.meta_.set_type(ObTinyIntType); }
    else if (any_dt && !any_date) { col.meta_.set_type(ObDateTimeType); }
    else if (any_date) { col.meta_.set_type(ObDateType); }
    else if (has_decimal) { col.meta_.set_type(ObDoubleType); }
    else { col.meta_.set_type(ObVarcharType); }
    if (OB_FAIL(schema.push_back(col))) {
      LOG_WARN("fail to push column", K(ret), K(c));
    }
  }
  const uint64_t sql_mode = 0;
  for (int64_t r = first_data_record; OB_SUCC(ret) && r < records.count(); ++r) {
    ObFileTableRow row;
    for (int64_t c = 0; OB_SUCC(ret) && c < col_cnt; ++c) {
      const ObSEArray<ObString, 16> &rec = records.at(r);
      const ObString v = c < rec.count() ? rec[c] : ObString();
      ObObj obj;
      if (OB_FAIL(convert_value(v, schema.at(c).meta_, sql_mode, alloc, obj))) {
        LOG_WARN("fail to convert value", K(ret), K(r), K(c), K(v));
      } else if (OB_FAIL(row.push_back(obj))) {
        LOG_WARN("fail to push value", K(ret));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(rows.push_back(row))) {
      LOG_WARN("fail to push row", K(ret), K(r));
    }
  }
  return ret;
}

// ---- newline-delimited JSON -------------------------------------------------
namespace
{
struct JsonKV
{
  JsonKV() : key_(), val_() {}
  TO_STRING_KV(K_(key), K_(val));
  ObString key_;
  ObString val_;
};
typedef ObSEArray<JsonKV, 16> JsonKVRow;

void skip_json_space(const char *buf, const int64_t len, int64_t &i)
{
  while (i < len && (' ' == buf[i] || '\t' == buf[i] || '\r' == buf[i])) { ++i; }
}

void append_utf8(char *dst, int64_t &pos, const uint32_t cp)
{
  if (cp < 0x80) {
    dst[pos++] = static_cast<char>(cp);
  } else if (cp < 0x800) {
    dst[pos++] = static_cast<char>(0xC0 | (cp >> 6));
    dst[pos++] = static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    dst[pos++] = static_cast<char>(0xE0 | (cp >> 12));
    dst[pos++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    dst[pos++] = static_cast<char>(0x80 | (cp & 0x3F));
  }
}

int hex4(const char *src, uint32_t &out)
{
  int ret = OB_SUCCESS;
  out = 0;
  for (int64_t i = 0; i < 4; ++i) {
    const char c = src[i];
    uint32_t digit = 0;
    if (c >= '0' && c <= '9') { digit = c - '0'; }
    else if (c >= 'a' && c <= 'f') { digit = 10 + (c - 'a'); }
    else if (c >= 'A' && c <= 'F') { digit = 10 + (c - 'A'); }
    else { ret = OB_INVALID_ARGUMENT; break; }
    out = (out << 4) | digit;
  }
  return ret;
}

// A quoted string starting at buf[i] == '"'. Escapes are resolved; a \u pair is
// emitted as UTF-8 (surrogate pairs are not stitched, which no fixture needs).
int read_json_string(ObIAllocator &alloc, const char *buf, const int64_t len,
                     int64_t &i, ObString &out)
{
  INIT_SUCC(ret);
  out.assign_ptr(NULL, 0);
  if (i >= len || '"' != buf[i]) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ++i;
    const int64_t start = i;
    while (i < len && '"' != buf[i] && '\\' != buf[i]) { ++i; }
    if (i >= len) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("unterminated json string", K(ret));
    } else if ('"' == buf[i]) {
      out = ObString(static_cast<int32_t>(i - start), buf + start);
      ++i;
    } else {
      // escape path: worst case the decoded text is as long as the source
      const int64_t cap = len - start + 1;
      char *tmp = NULL;
      int64_t pos = 0;
      if (OB_ISNULL(tmp = static_cast<char *>(alloc.alloc(cap)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        while (OB_SUCC(ret) && i < len && '"' != buf[i]) {
          if ('\\' != buf[i]) {
            tmp[pos++] = buf[i++];
          } else {
            ++i;
            if (i >= len) {
              ret = OB_INVALID_ARGUMENT;
            } else {
              const char e = buf[i++];
              char mapped = '\0';
              switch (e) {
                case '"': mapped = '"'; break;
                case '\\': mapped = '\\'; break;
                case '/': mapped = '/'; break;
                case 'b': mapped = '\b'; break;
                case 'f': mapped = '\f'; break;
                case 'n': mapped = '\n'; break;
                case 'r': mapped = '\r'; break;
                case 't': mapped = '\t'; break;
                case 'u': {
                  uint32_t cp = 0;
                  if (i + 4 > len) {
                    ret = OB_INVALID_ARGUMENT;
                  } else if (OB_FAIL(hex4(buf + i, cp))) {
                    LOG_WARN("bad unicode escape", K(ret));
                  } else {
                    i += 4;
                    append_utf8(tmp, pos, cp);
                  }
                  break;
                }
                default:
                  // Unknown escape: keep both characters rather than drop data.
                  tmp[pos++] = '\\';
                  tmp[pos++] = e;
                  break;
              }
              if (OB_SUCC(ret) && '\0' != mapped && e != 'u') {
                tmp[pos++] = mapped;
              }
            }
          }
          if (OB_SUCC(ret) && pos >= cap) {
            ret = OB_SIZE_OVERFLOW;
          }
        }
        if (OB_FAIL(ret)) {
        } else if (i >= len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          ++i;  // closing quote
          out = ObString(static_cast<int32_t>(pos), tmp);
        }
      }
    }
  }
  return ret;
}

// Returns the value as text: strings unescaped, numbers/booleans as written,
// null as an empty string, composites as their raw JSON substring.
int read_json_value(ObIAllocator &alloc, const char *buf, const int64_t len,
                    int64_t &i, ObString &out)
{
  INIT_SUCC(ret);
  out.assign_ptr(NULL, 0);
  skip_json_space(buf, len, i);
  if (i >= len) {
    ret = OB_INVALID_ARGUMENT;
  } else if ('"' == buf[i]) {
    if (OB_FAIL(read_json_string(alloc, buf, len, i, out))) {
      LOG_WARN("fail to read json string value", K(ret));
    }
  } else if ('{' == buf[i] || '[' == buf[i]) {
    const int64_t start = i;
    int64_t depth = 0;
    bool in_str = false;
    while (OB_SUCC(ret) && i < len) {
      const char c = buf[i];
      if (in_str) {
        if ('\\' == c) { ++i; }
        else if ('"' == c) { in_str = false; }
      } else if ('"' == c) {
        in_str = true;
      } else if ('{' == c || '[' == c) {
        ++depth;
      } else if ('}' == c || ']' == c) {
        --depth;
        if (depth < 0) {
          ret = OB_INVALID_ARGUMENT;
        } else if (0 == depth) {
          ++i;
          break;
        }
      }
      ++i;
    }
    if (OB_FAIL(ret)) {
    } else if (0 != depth) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("unbalanced json composite", K(ret));
    } else {
      out = ObString(static_cast<int32_t>(i - start), buf + start);
    }
  } else {
    const int64_t start = i;
    while (i < len && ',' != buf[i] && '}' != buf[i] && ']' != buf[i]
           && '\n' != buf[i] && '\r' != buf[i]) {
      ++i;
    }
    while (i > start && (' ' == buf[i - 1] || '\t' == buf[i - 1])) { --i; }
    out = ObString(static_cast<int32_t>(i - start), buf + start);
    if (out.empty()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (4 == out.length() && 0 == MEMCMP(out.ptr(), "null", 4)) {
      out.assign_ptr(NULL, 0);
    }
  }
  return ret;
}

int read_json_object(ObIAllocator &alloc, const char *buf, const int64_t len,
                     int64_t &i, JsonKVRow &row)
{
  INIT_SUCC(ret);
  skip_json_space(buf, len, i);
  if (i >= len || '{' != buf[i]) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("json line must hold one object", K(ret), K(i));
  } else {
    ++i;
    while (OB_SUCC(ret)) {
      skip_json_space(buf, len, i);
      if (i >= len) {
        ret = OB_INVALID_ARGUMENT;
      } else if ('}' == buf[i]) {
        ++i;
        break;
      } else if (',' == buf[i]) {
        ++i;
      } else {
        JsonKV kv;
        if (OB_FAIL(read_json_string(alloc, buf, len, i, kv.key_))) {
          LOG_WARN("fail to read json key", K(ret));
        } else {
          skip_json_space(buf, len, i);
          if (i >= len || ':' != buf[i]) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            ++i;
            if (OB_FAIL(read_json_value(alloc, buf, len, i, kv.val_))) {
              LOG_WARN("fail to read json value", K(ret), K(kv.key_));
            } else if (OB_FAIL(row.push_back(kv))) {
              LOG_WARN("fail to push json member", K(ret));
            }
          }
        }
      }
    }
  }
  return ret;
}

bool glob_name_match(const char *pat, const int64_t plen, const char *name, const int64_t nlen)
{
  // '*' and '?' only; backtracking on the last star keeps this simple and linear
  // for the patterns the function accepts.
  int64_t p = 0;
  int64_t n = 0;
  int64_t star = -1;
  int64_t mark = 0;
  bool bool_ret = false;
  while (n < nlen) {
    if (p < plen && ('?' == pat[p] || pat[p] == name[n])) {
      ++p;
      ++n;
    } else if (p < plen && '*' == pat[p]) {
      star = p++;
      mark = n;
    } else if (star >= 0) {
      p = star + 1;
      n = ++mark;
    } else {
      break;
    }
  }
  while (p < plen && '*' == pat[p]) { ++p; }
  bool_ret = (p == plen && n == nlen);
  return bool_ret;
}
} // namespace

int ObFileTableReader::parse_jsonl(ObIAllocator &alloc, const char *buf, const int64_t buf_len,
                                   ObSEArray<ObString, 16> &headers,
                                   ObIArray<ObSEArray<ObString, 16>> &records)
{
  INIT_SUCC(ret);
  headers.reset();
  records.reset();
  ObSEArray<JsonKVRow, 8> parsed;
  int64_t i = 0;
  while (OB_SUCC(ret) && i < buf_len) {
    const int64_t line_start = i;
    while (i < buf_len && '\n' != buf[i]) { ++i; }
    const int64_t line_end = i;
    if (i < buf_len) { ++i; }  // the next line starts after the newline
    int64_t cur = line_start;
    skip_json_space(buf, line_end, cur);
    if (cur < line_end && '{' == buf[cur]) {
      JsonKVRow row;
      int64_t pos = cur;
      if (OB_FAIL(read_json_object(alloc, buf, line_end, pos, row))) {
        LOG_WARN("fail to read json object", K(ret), K(line_start), K(line_end));
      } else if (OB_FAIL(parsed.push_back(row))) {
        LOG_WARN("fail to push json row", K(ret));
      }
    }
  }
  // Key union in first-seen order, then every row padded to that width so a
  // missing field reads back as NULL.
  for (int64_t r = 0; OB_SUCC(ret) && r < parsed.count(); ++r) {
    for (int64_t k = 0; OB_SUCC(ret) && k < parsed.at(r).count(); ++k) {
      bool seen = false;
      for (int64_t h = 0; !seen && h < headers.count(); ++h) {
        seen = (headers[h] == parsed.at(r).at(k).key_);
      }
      if (!seen && OB_FAIL(headers.push_back(parsed.at(r).at(k).key_))) {
        LOG_WARN("fail to push json header", K(ret));
      }
    }
  }
  for (int64_t r = 0; OB_SUCC(ret) && r < parsed.count(); ++r) {
    ObSEArray<ObString, 16> rec;
    for (int64_t h = 0; OB_SUCC(ret) && h < headers.count(); ++h) {
      ObString val;
      for (int64_t k = 0; k < parsed.at(r).count(); ++k) {
        if (parsed.at(r).at(k).key_ == headers[h]) {
          val = parsed.at(r).at(k).val_;
          break;
        }
      }
      if (OB_FAIL(rec.push_back(val))) {
        LOG_WARN("fail to push json value", K(ret));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(records.push_back(rec))) {
      LOG_WARN("fail to push json record", K(ret));
    }
  }
  return ret;
}

// ---- Parquet ------------------------------------------------------------------
// A deliberately small reader: flat schemas only, PLAIN or dictionary encoded
// values, uncompressed or Snappy-block pages, data page v1 and v2. Neither the
// Thrift compact protocol nor the Snappy block format is vendored in this tree,
// so both are decoded here.
namespace
{
enum PqThriftType
{
  T_STOP = 0, T_TRUE = 1, T_FALSE = 2, T_BYTE = 3, T_I16 = 4, T_I32 = 5, T_I64 = 6,
  T_DOUBLE = 7, T_BINARY = 8, T_LIST = 9, T_SET = 10, T_MAP = 11, T_STRUCT = 12
};

enum PqPhysType
{
  PQ_BOOLEAN = 0, PQ_INT32 = 1, PQ_INT64 = 2, PQ_INT96 = 3, PQ_FLOAT = 4,
  PQ_DOUBLE = 5, PQ_BYTE_ARRAY = 6, PQ_FIXED_LEN_BYTE_ARRAY = 7
};

enum PqRepetitionType { PQ_REQUIRED = 0, PQ_OPTIONAL = 1, PQ_REPEATED = 2 };

enum PqCodec { PQ_CODEC_UNCOMPRESSED = 0, PQ_CODEC_SNAPPY = 1 };

enum PqPageType { PQ_PAGE_DATA = 0, PQ_PAGE_DICTIONARY = 2, PQ_PAGE_DATA_V2 = 3 };

enum PqEncoding
{
  PQ_ENC_PLAIN = 0, PQ_ENC_PLAIN_DICTIONARY = 1, PQ_ENC_RLE = 2, PQ_ENC_RLE_DICTIONARY = 7
};

struct PqCursor
{
  PqCursor(const char *buf, const int64_t len, const int64_t pos = 0)
    : buf_(buf), len_(len), pos_(pos) {}
  bool eof() const { return pos_ >= len_; }
  bool beyond(const int64_t extra) const { return pos_ + extra > len_; }
  const char *buf_;
  int64_t len_;
  int64_t pos_;
};

struct PqColumnMeta
{
  PqColumnMeta() : name_(), primitive_(PQ_INT32), repetition_(PQ_REQUIRED),
                   type_length_(0), codec_(PQ_CODEC_UNCOMPRESSED), num_values_(0),
                   data_offset_(-1), dict_offset_(-1) {}
  int64_t bit_width() const
  {
    return PQ_OPTIONAL == repetition_ ? 1 : 0;
  }
  TO_STRING_KV(K_(name), K_(primitive), K_(repetition), K_(type_length), K_(codec),
               K_(num_values), K_(data_offset), K_(dict_offset));
  ObString name_;
  int32_t primitive_;
  int32_t repetition_;
  int32_t type_length_;
  int32_t codec_;
  int64_t num_values_;
  int64_t data_offset_;
  int64_t dict_offset_;
};

struct PqRowGroup
{
  PqRowGroup() : num_rows_(0), cols_() {}
  TO_STRING_KV(K_(num_rows), K_(cols));
  int64_t num_rows_;
  common::ObSEArray<PqColumnMeta, 16> cols_;
};

struct PqPage
{
  PqPage() : type_(0), uncompressed_(0), compressed_(0), num_values_(0), nulls_(0),
             levels_len_(0), encoding_(PQ_ENC_PLAIN), body_off_(0), is_v2_(false) {}
  TO_STRING_KV(K_(type), K_(uncompressed), K_(compressed), K_(num_values), K_(nulls),
               K_(levels_len), K_(encoding), K_(body_off), K_(is_v2));
  int32_t type_;
  int64_t uncompressed_;
  int64_t compressed_;
  int64_t num_values_;
  int64_t nulls_;
  int64_t levels_len_;
  int32_t encoding_;
  int64_t body_off_;
  bool is_v2_;
};

int pq_uvarint(PqCursor &c, uint64_t &out)
{
  INIT_SUCC(ret);
  uint64_t value = 0;
  int64_t shift = 0;
  out = 0;
  while (OB_SUCC(ret)) {
    if (c.eof() || shift > 63) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("truncated thrift varint", K(ret), K(c.pos_), K(c.len_), K(shift));
    } else {
      const uint8_t byte = c.buf_[c.pos_++];
      value |= (static_cast<uint64_t>(byte & 0x7F)) << shift;
      if (0 == (byte & 0x80)) {
        out = value;
        break;
      }
      shift += 7;
    }
  }
  return ret;
}

int pq_zigzag(PqCursor &c, int64_t &out)
{
  INIT_SUCC(ret);
  uint64_t raw = 0;
  out = 0;
  if (OB_FAIL(pq_uvarint(c, raw))) {
  } else {
    out = static_cast<int64_t>((raw >> 1) ^ (~(raw & 1) + 1));
  }
  return ret;
}

int pq_binary(PqCursor &c, const char *&ptr, int64_t &len)
{
  INIT_SUCC(ret);
  ptr = NULL;
  len = 0;
  uint64_t n = 0;
  if (OB_FAIL(pq_uvarint(c, n))) {
  } else if (c.beyond(static_cast<int64_t>(n))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("truncated thrift binary", K(ret), K(c.pos_), K(n));
  } else {
    ptr = c.buf_ + c.pos_;
    len = static_cast<int64_t>(n);
    c.pos_ += len;
  }
  return ret;
}

// Reads the next field of a struct. Ids are delta encoded, so `last` is threaded
// through; ftype == T_STOP (terminator consumed) ends the struct.
int pq_next_field(PqCursor &c, int64_t &last, int64_t &fid, int &ftype)
{
  INIT_SUCC(ret);
  fid = 0;
  ftype = T_STOP;
  if (c.eof()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("truncated thrift struct", K(ret), K(c.pos_), K(c.len_));
  } else if (0 == c.buf_[c.pos_]) {
    ++c.pos_;
  } else {
    const uint8_t header = c.buf_[c.pos_++];
    const int64_t delta = (header >> 4) & 0x0F;
    ftype = header & 0x0F;
    if (0 == delta) {
      if (OB_FAIL(pq_zigzag(c, fid))) {
        LOG_WARN("fail to read absolute field id", K(ret));
      }
    } else {
      fid = last + delta;
    }
    if (OB_SUCC(ret)) {
      last = fid;
    }
  }
  return ret;
}

int pq_list_header(PqCursor &c, int64_t &size, int &elem_type)
{
  INIT_SUCC(ret);
  size = 0;
  elem_type = 0;
  uint64_t big = 0;
  if (c.eof()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("truncated thrift list", K(ret), K(c.pos_));
  } else {
    const uint8_t header = c.buf_[c.pos_++];
    elem_type = header & 0x0F;
    size = (header >> 4) & 0x0F;
    if (15 == size) {
      if (OB_FAIL(pq_uvarint(c, big))) {
        LOG_WARN("fail to read list size", K(ret));
      } else {
        size = static_cast<int64_t>(big);
      }
    }
  }
  return ret;
}

int pq_skip(PqCursor &c, const int type);

template <typename Sink>
int pq_struct_of(PqCursor &c, Sink &sink);

template <typename Sink>
int pq_list_of_structs(PqCursor &c, Sink &sink);

struct PqStructSkipper
{
  int operator()(const int64_t fid, const int ftype, PqCursor &c)
  {
    UNUSED(fid);
    return pq_skip(c, ftype);
  }
};

int pq_skip(PqCursor &c, const int type)
{
  INIT_SUCC(ret);
  if (T_TRUE == type || T_FALSE == type) {
    // the value is carried by the field header itself
  } else if (T_BYTE == type) {
    if (c.eof()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      ++c.pos_;
    }
  } else if (T_I16 == type || T_I32 == type || T_I64 == type) {
    int64_t unused = 0;
    if (OB_FAIL(pq_zigzag(c, unused))) {
      LOG_WARN("fail to skip integer", K(ret), K(type));
    }
  } else if (T_DOUBLE == type) {
    c.pos_ += 8;
  } else if (T_BINARY == type) {
    const char *ptr = NULL;
    int64_t len = 0;
    if (OB_FAIL(pq_binary(c, ptr, len))) {
      LOG_WARN("fail to skip binary", K(ret));
    }
  } else if (T_LIST == type || T_SET == type) {
    int64_t size = 0;
    int elem_type = 0;
    if (OB_FAIL(pq_list_header(c, size, elem_type))) {
      LOG_WARN("fail to skip list", K(ret));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
      if (OB_FAIL(pq_skip(c, elem_type))) {
        LOG_WARN("fail to skip element", K(ret), K(i), K(elem_type));
      }
    }
  } else if (T_MAP == type) {
    if (c.eof()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("truncated thrift map", K(ret));
    } else {
      const uint8_t header = c.buf_[c.pos_++];
      const int64_t size = header & 0x0F;
      const int key_type = (header >> 4) & 0x0F;
      const int value_type = header & 0x0F;
      for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
        if (OB_FAIL(pq_skip(c, key_type))) {
        } else if (OB_FAIL(pq_skip(c, value_type))) {
          LOG_WARN("fail to skip map entry", K(ret), K(i));
        }
      }
    }
  } else if (T_STRUCT == type) {
    PqStructSkipper skipper;
    if (OB_FAIL(pq_struct_of(c, skipper))) {
      LOG_WARN("fail to skip struct", K(ret));
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("unknown thrift type", K(ret), K(type));
  }
  if (OB_SUCC(ret) && c.pos_ > c.len_) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("thrift cursor ran past the buffer", K(ret), K(c.pos_), K(c.len_));
  }
  return ret;
}

template <typename Sink>
int pq_struct_of(PqCursor &c, Sink &sink)
{
  INIT_SUCC(ret);
  int64_t last = 0;
  while (OB_SUCC(ret)) {
    int64_t fid = 0;
    int ftype = T_STOP;
    if (OB_FAIL(pq_next_field(c, last, fid, ftype))) {
      LOG_WARN("fail to read field", K(ret), K(c.pos_));
    } else if (T_STOP == ftype) {
      break;
    } else if (OB_FAIL(sink(fid, ftype, c))) {
      LOG_WARN("fail to handle field", K(ret), K(fid), K(ftype));
    }
  }
  return ret;
}

template <typename Sink>
int pq_list_of_structs(PqCursor &c, Sink &sink)
{
  INIT_SUCC(ret);
  int64_t size = 0;
  int elem_type = 0;
  if (OB_FAIL(pq_list_header(c, size, elem_type))) {
    LOG_WARN("fail to read list header", K(ret));
  } else if (T_STRUCT != elem_type) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("expected a list of structs", K(ret), K(elem_type));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
    if (OB_FAIL(sink(c, i))) {
      LOG_WARN("fail to handle list element", K(ret), K(i));
    }
  }
  return ret;
}

int pq_le32(const char *src, int64_t &out)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(src)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    out = static_cast<int32_t>(static_cast<uint8_t>(src[0]))
        | (static_cast<int32_t>(static_cast<uint8_t>(src[1])) << 8)
        | (static_cast<int32_t>(static_cast<uint8_t>(src[2])) << 16)
        | (static_cast<int32_t>(static_cast<uint8_t>(src[3])) << 24);
  }
  return ret;
}

int pq_le64(const char *src, int64_t &out)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(src)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    uint64_t v = 0;
    for (int64_t i = 7; i >= 0; --i) {
      v = (v << 8) | static_cast<uint8_t>(src[i]);
    }
    out = static_cast<int64_t>(v);
  }
  return ret;
}

// Parquet stores levels and dictionary indexes with the RLE / bit-packed hybrid:
// a varint header whose low bit selects bit-packed (8 values per group) or a run
// of a single value.
int pq_read_hybrid(const char *src, const int64_t src_len, const int64_t bit_width,
                   const int64_t count, ObIArray<int64_t> &out)
{
  INIT_SUCC(ret);
  if (count <= 0) {
  } else if (bit_width <= 0) {
    for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
      if (OB_FAIL(out.push_back(0))) {
        LOG_WARN("fail to push zero level", K(ret));
      }
    }
  } else {
    const uint64_t mask = (1ULL << bit_width) - 1;
    int64_t pos = 0;
    while (OB_SUCC(ret) && out.count() < count) {
      PqCursor cur(src, src_len, pos);
      uint64_t header = 0;
      if (OB_FAIL(pq_uvarint(cur, header))) {
        LOG_WARN("fail to read hybrid header", K(ret), K(pos), K(src_len));
      } else {
        pos = cur.pos_;
        if (0 != (header & 1)) {
          const int64_t values = static_cast<int64_t>(header >> 1) * 8;
          const int64_t bytes = (values * bit_width + 7) / 8;
          if (pos + bytes > src_len) {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("truncated bit packed run", K(ret), K(pos), K(bytes), K(src_len));
          }
          for (int64_t i = 0; OB_SUCC(ret) && i < values && out.count() < count; ++i) {
            const int64_t bit = i * bit_width;
            const int64_t byte0 = bit / 8;
            const int64_t off = bit % 8;
            const int64_t need = (off + bit_width + 7) / 8;
            uint64_t raw = 0;
            for (int64_t b = 0; b < need; ++b) {
              raw |= (static_cast<uint64_t>(static_cast<uint8_t>(src[pos + byte0 + b])))
                     << (8 * b);
            }
            if (OB_FAIL(out.push_back(static_cast<int64_t>((raw >> off) & mask)))) {
              LOG_WARN("fail to push level", K(ret));
            }
          }
          pos += bytes;
        } else {
          const int64_t run = static_cast<int64_t>(header >> 1);
          const int64_t bytes = (bit_width + 7) / 8;
          uint64_t value = 0;
          if (pos + bytes > src_len) {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("truncated rle run", K(ret), K(pos), K(bytes));
          }
          for (int64_t b = 0; OB_SUCC(ret) && b < bytes; ++b) {
            value |= (static_cast<uint64_t>(static_cast<uint8_t>(src[pos + b]))) << (8 * b);
          }
          pos += bytes;
          for (int64_t i = 0; OB_SUCC(ret) && i < run && out.count() < count; ++i) {
            if (OB_FAIL(out.push_back(static_cast<int64_t>(value & mask)))) {
              LOG_WARN("fail to push level", K(ret));
            }
          }
        }
      }
    }
    if (OB_SUCC(ret) && out.count() < count) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("hybrid stream too short", K(ret), K(count), "decoded", out.count());
    }
  }
  return ret;
}

// Snappy block format: varint uncompressed length, then literal and copy tags.
int pq_snappy(const char *src, const int64_t src_len, char *dst, const int64_t dst_cap,
              int64_t &dst_len)
{
  INIT_SUCC(ret);
  dst_len = 0;
  PqCursor cur(src, src_len);
  uint64_t want = 0;
  if (OB_ISNULL(src) || OB_ISNULL(dst)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(pq_uvarint(cur, want))) {
    LOG_WARN("fail to read snappy length", K(ret));
  } else if (static_cast<int64_t>(want) > dst_cap) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("snappy output does not fit", K(ret), K(want), K(dst_cap));
  } else {
    int64_t pos = cur.pos_;
    while (OB_SUCC(ret) && dst_len < static_cast<int64_t>(want) && pos < src_len) {
      const uint8_t tag = static_cast<uint8_t>(src[pos++]);
      const int type = tag & 0x03;
      int64_t length = 0;
      int64_t offset = 0;
      if (0 == type) {
        const int64_t n = tag >> 2;
        int64_t extra = 0;
        if (n < 60) {
          length = n + 1;
        } else if (n > 62) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("reserved snappy literal length", K(ret), K(n));
        } else {
          extra = 1 == n % 2 ? 1 : 2;   // 60 -> 1 byte, 61 -> 2, 62 -> 4
          if (62 == n) {
            extra = 4;
          }
          if (pos + extra > src_len) {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("truncated snappy literal length", K(ret), K(n));
          }
          if (OB_SUCC(ret)) {
            int64_t raw = 0;
            for (int64_t b = 0; b < extra; ++b) {
              raw |= (static_cast<int64_t>(static_cast<uint8_t>(src[pos + b]))) << (8 * b);
            }
            pos += extra;
            length = raw + 1;
          }
        }
        if (OB_FAIL(ret)) {
        } else if (pos + length > src_len || dst_len + length > static_cast<int64_t>(want)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("snappy literal overruns", K(ret), K(pos), K(length), K(dst_len));
        } else {
          MEMCPY(dst + dst_len, src + pos, length);
          pos += length;
          dst_len += length;
        }
      } else {
        bool bad = false;
        if (1 == type) {
          length = 4 + ((tag >> 2) & 0x07);
          bad = pos >= src_len;
          if (!bad) {
            offset = ((static_cast<int64_t>(tag & 0xF8)) << 3) | static_cast<uint8_t>(src[pos]);
            ++pos;
          }
        } else if (2 == type) {
          length = (tag >> 2) + 1;
          bad = pos + 2 > src_len;
          if (!bad) {
            offset = static_cast<int64_t>(static_cast<uint8_t>(src[pos]))
                   | (static_cast<int64_t>(static_cast<uint8_t>(src[pos + 1])) << 8);
            pos += 2;
          }
        } else {
          length = (tag >> 2) + 1;
          bad = pos + 4 > src_len;
          if (!bad) {
            offset = 0;
            for (int64_t b = 0; b < 4; ++b) {
              offset |= (static_cast<int64_t>(static_cast<uint8_t>(src[pos + b]))) << (8 * b);
            }
            pos += 4;
          }
        }
        if (bad) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("truncated snappy copy", K(ret), K(type));
        } else if (offset <= 0 || offset > dst_len
                   || dst_len + length > static_cast<int64_t>(want)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("invalid snappy copy", K(ret), K(offset), K(length), K(dst_len));
        } else {
          for (int64_t i = 0; i < length; ++i) {
            dst[dst_len + i] = dst[dst_len - offset + i];
          }
          dst_len += length;
        }
      }
    }
    if (OB_SUCC(ret) && dst_len != static_cast<int64_t>(want)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("snappy produced short output", K(ret), K(dst_len), K(want));
    }
  }
  return ret;
}

int pq_fixed_size(const int32_t phys, const int32_t type_length)
{
  int64_t size = 0;
  switch (phys) {
    case PQ_INT32:
    case PQ_FLOAT: size = 4; break;
    case PQ_INT64:
    case PQ_DOUBLE: size = 8; break;
    case PQ_FIXED_LEN_BYTE_ARRAY: size = type_length; break;
    default: size = 0; break;
  }
  return size;
}

// PLAIN values for `count` entries. BOOLEAN is bit packed, so it is handled apart.
int pq_decode_plain(ObIAllocator &alloc, const int32_t phys, const int32_t type_length,
                    const char *src, const int64_t src_len, const int64_t count,
                    ObIArray<ObObj> &out)
{
  INIT_SUCC(ret);
  int64_t pos = 0;
  if (PQ_BOOLEAN == phys) {
    if ((count + 7) / 8 > src_len) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("truncated boolean values", K(ret), K(count), K(src_len));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
      ObObj value;
      value.set_bool(0 != ((static_cast<uint8_t>(src[i / 8]) >> (i % 8)) & 1));
      if (OB_FAIL(out.push_back(value))) {
        LOG_WARN("fail to push boolean", K(ret));
      }
    }
  } else if (PQ_BYTE_ARRAY == phys) {
    for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
      int64_t blen = 0;
      char *text = NULL;
      if (pos + 4 > src_len) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("truncated byte array length", K(ret), K(pos));
      } else if (OB_FAIL(pq_le32(src + pos, blen))) {
        LOG_WARN("fail to read byte array length", K(ret));
      } else if (blen < 0 || pos + 4 + blen > src_len) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("byte array overruns page", K(ret), K(blen), K(pos));
      } else if (OB_ISNULL(text = static_cast<char *>(alloc.alloc(blen + 1)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(text, src + pos + 4, blen);
        text[blen] = '\0';
        pos += 4 + blen;
        ObObj value;
        value.set_varchar(ObString(static_cast<int32_t>(blen), text));
        if (OB_FAIL(out.push_back(value))) {
          LOG_WARN("fail to push byte array", K(ret));
        }
      }
    }
  } else {
    const int64_t size = pq_fixed_size(phys, type_length);
    if (size <= 0) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("unsupported parquet physical type", K(ret), K(phys));
    } else if (count * size > src_len) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("truncated plain values", K(ret), K(count), K(size), K(src_len));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
      ObObj value;
      int64_t v = 0;
      if (OB_FAIL(ret)) {
      } else if (8 == size && OB_FAIL(pq_le64(src + pos, v))) {
        LOG_WARN("fail to read 8 byte value", K(ret));
      } else if (8 != size && OB_FAIL(pq_le32(src + pos, v))) {
        LOG_WARN("fail to read 4 byte value", K(ret));
      } else if (PQ_INT32 == phys || PQ_INT64 == phys) {
        value.set_int(v);
      } else if (PQ_FLOAT == phys) {
        union { uint32_t u_; float f_; } cvt;
        cvt.u_ = static_cast<uint32_t>(v);
        value.set_double(static_cast<double>(cvt.f_));
      } else if (PQ_DOUBLE == phys) {
        union { uint64_t u_; double d_; } cvt;
        cvt.u_ = static_cast<uint64_t>(v);
        value.set_double(cvt.d_);
      } else {
        value.set_varchar(ObString(static_cast<int32_t>(size), src + pos));
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(out.push_back(value))) {
        LOG_WARN("fail to push value", K(ret));
      } else {
        pos += size;
      }
    }
  }
  return ret;
}

// DataPageHeader(v1): 1 num_values, 2 encoding. DataPageHeaderV2: 1 num_values,
// 2 num_nulls, 3 encoding, 4/5 level lengths. DictionaryPageHeader: 1 num_values,
// 2 encoding.
struct PqPageBodySink
{
  PqPageBodySink(PqPage &page, int64_t &dict_values) : page_(page), dict_values_(dict_values) {}
  int operator()(const int64_t fid, const int ftype, PqCursor &c)
  {
    int ret = OB_SUCCESS;
    int64_t v = 0;
    if (T_I32 != ftype) {
      if (OB_FAIL(pq_skip(c, ftype))) {
        LOG_WARN("fail to skip page body field", K(ret), K(fid), K(ftype));
      }
    } else if (OB_FAIL(pq_zigzag(c, v))) {
      LOG_WARN("fail to read page body int", K(ret), K(fid));
    } else if (1 == fid) {
      if (page_.is_v2_ || PQ_PAGE_DICTIONARY == page_.type_) {
        page_.num_values_ = v;
        dict_values_ = v;
      } else {
        page_.num_values_ = v;
      }
    } else if (page_.is_v2_ && 2 == fid) {
      page_.nulls_ = v;
    } else if (page_.is_v2_ && 3 == fid) {
      page_.encoding_ = static_cast<int32_t>(v);
    } else if (!page_.is_v2_ && 2 == fid && PQ_PAGE_DATA == page_.type_) {
      page_.encoding_ = static_cast<int32_t>(v);
    } else if (page_.is_v2_ && (4 == fid || 5 == fid)) {
      // Writers disagree about which level length comes first, and a flat column
      // has no repetition levels at all, so only the sum is used.
      page_.levels_len_ += v;
    }
    return ret;
  }
  PqPage &page_;
  int64_t &dict_values_;
};

// PageHeader: 1 type, 2 uncompressed, 3 compressed, 4 crc, 5 data page v1,
// 7 dictionary page, 8 data page v2.
struct PqPageSink
{
  PqPageSink(PqPage &page) : page_(page), dict_values_(0) {}
  int operator()(const int64_t fid, const int ftype, PqCursor &c)
  {
    int ret = OB_SUCCESS;
    if (T_I32 == ftype && (1 == fid || 2 == fid || 3 == fid)) {
      int64_t v = 0;
      if (OB_FAIL(pq_zigzag(c, v))) {
        LOG_WARN("fail to read page header int", K(ret), K(fid));
      } else if (1 == fid) {
        page_.type_ = static_cast<int32_t>(v);
      } else if (2 == fid) {
        page_.uncompressed_ = v;
      } else {
        page_.compressed_ = v;
      }
    } else if (T_STRUCT == ftype && (5 == fid || 7 == fid || 8 == fid)) {
      page_.is_v2_ = (8 == fid);
      PqPageBodySink body(page_, dict_values_);
      if (OB_FAIL(pq_struct_of(c, body))) {
        LOG_WARN("fail to read page body header", K(ret), K(fid));
      }
    } else if (OB_FAIL(pq_skip(c, ftype))) {
      LOG_WARN("fail to skip page header field", K(ret), K(fid), K(ftype));
    }
    return ret;
  }
  PqPage &page_;
  int64_t dict_values_;
};

int pq_read_page_header(PqCursor &c, PqPage &page)
{
  INIT_SUCC(ret);
  PqPageSink sink(page);
  if (OB_FAIL(pq_struct_of(c, sink))) {
    LOG_WARN("fail to read page header", K(ret), K(c.pos_));
  } else {
    page.body_off_ = c.pos_;
  }
  return ret;
}

// SchemaElement: 1 type, 2 type_length, 3 repetition_type, 4 name, 5 num_children
struct PqSchemaSink
{
  PqSchemaSink(PqColumnMeta &col, int64_t &num_children)
    : col_(col), num_children_(num_children), name_(NULL), name_len_(0) {}
  int operator()(const int64_t fid, const int ftype, PqCursor &c)
  {
    int ret = OB_SUCCESS;
    if (4 == fid && T_BINARY == ftype) {
      if (OB_FAIL(pq_binary(c, name_, name_len_))) {
        LOG_WARN("fail to read column name", K(ret));
      }
    } else if (T_I32 == ftype) {
      int64_t v = 0;
      if (OB_FAIL(pq_zigzag(c, v))) {
        LOG_WARN("fail to read schema int", K(ret), K(fid));
      } else if (1 == fid) {
        col_.primitive_ = static_cast<int32_t>(v);
      } else if (2 == fid) {
        col_.type_length_ = static_cast<int32_t>(v);
      } else if (3 == fid) {
        col_.repetition_ = static_cast<int32_t>(v);
      } else if (5 == fid) {
        num_children_ = v;
      }
    } else if (OB_FAIL(pq_skip(c, ftype))) {
      LOG_WARN("fail to skip schema field", K(ret), K(fid), K(ftype));
    }
    return ret;
  }
  PqColumnMeta &col_;
  int64_t &num_children_;
  const char *name_;
  int64_t name_len_;
};

// ChunkMetaData: 1 type, 4 codec, 5 num_values, 9 data_page_offset,
// 11 dictionary_page_offset.
struct PqChunkSink
{
  explicit PqChunkSink(PqColumnMeta &col) : col_(col) {}
  int operator()(const int64_t fid, const int ftype, PqCursor &c)
  {
    int ret = OB_SUCCESS;
    if ((T_I32 == ftype && (1 == fid || 4 == fid)) || (T_I64 == ftype && (5 == fid || 9 == fid
        || 11 == fid))) {
      int64_t v = 0;
      if (OB_FAIL(pq_zigzag(c, v))) {
        LOG_WARN("fail to read chunk metadata int", K(ret), K(fid));
      } else if (1 == fid) {
        col_.primitive_ = static_cast<int32_t>(v);
      } else if (4 == fid) {
        col_.codec_ = static_cast<int32_t>(v);
      } else if (5 == fid) {
        col_.num_values_ = v;
      } else if (9 == fid) {
        col_.data_offset_ = v;
      } else {
        col_.dict_offset_ = v;
      }
    } else if (OB_FAIL(pq_skip(c, ftype))) {
      LOG_WARN("fail to skip chunk metadata field", K(ret), K(fid), K(ftype));
    }
    return ret;
  }
  PqColumnMeta &col_;
};

// ColumnChunk: 3 meta_data
struct PqChunkListSink
{
  explicit PqChunkListSink(PqColumnMeta &col) : col_(col) {}
  int operator()(const int64_t fid, const int ftype, PqCursor &c)
  {
    int ret = OB_SUCCESS;
    if (3 == fid && T_STRUCT == ftype) {
      PqChunkSink sink(col_);
      if (OB_FAIL(pq_struct_of(c, sink))) {
        LOG_WARN("fail to read chunk metadata", K(ret));
      }
    } else if (OB_FAIL(pq_skip(c, ftype))) {
      LOG_WARN("fail to skip column chunk field", K(ret), K(fid), K(ftype));
    }
    return ret;
  }
  PqColumnMeta &col_;
};

struct PqColumnElemSink
{
  PqColumnElemSink(ObIArray<PqColumnMeta> &cols, int64_t &rows) : cols_(cols), rows_(rows) {}
  int operator()(PqCursor &c, const int64_t idx)
  {
    UNUSED(idx);
    int ret = OB_SUCCESS;
    PqColumnMeta col;
    PqChunkListSink sink(col);
    if (OB_FAIL(pq_struct_of(c, sink))) {
      LOG_WARN("fail to read column chunk", K(ret));
    } else if (OB_FAIL(cols_.push_back(col))) {
      LOG_WARN("fail to push column layout", K(ret));
    } else if (0 == idx) {
      // RowGroup.num_rows is read after the column list, so it is filled in by
      // the enclosing row group sink.
      UNUSED(rows_);
    }
    return ret;
  }
  ObIArray<PqColumnMeta> &cols_;
  int64_t &rows_;
};

// RowGroup: 1 columns (list of ColumnChunk), 3 num_rows
struct PqRowGroupSink
{
  explicit PqRowGroupSink(PqRowGroup &group) : group_(group) {}
  int operator()(const int64_t fid, const int ftype, PqCursor &c)
  {
    int ret = OB_SUCCESS;
    if (3 == fid && T_I64 == ftype) {
      if (OB_FAIL(pq_zigzag(c, group_.num_rows_))) {
        LOG_WARN("fail to read row group rows", K(ret));
      }
    } else if (1 == fid && T_LIST == ftype) {
      PqColumnElemSink elem(group_.cols_, group_.num_rows_);
      if (OB_FAIL(pq_list_of_structs(c, elem))) {
        LOG_WARN("fail to read column chunks", K(ret));
      }
    } else if (OB_FAIL(pq_skip(c, ftype))) {
      LOG_WARN("fail to skip row group field", K(ret), K(fid), K(ftype));
    }
    return ret;
  }
  PqRowGroup &group_;
};

struct PqRowGroupElemSink
{
  explicit PqRowGroupElemSink(ObIArray<PqRowGroup> &groups) : groups_(groups) {}
  int operator()(PqCursor &c, const int64_t idx)
  {
    UNUSED(idx);
    int ret = OB_SUCCESS;
    PqRowGroup group;
    PqRowGroupSink sink(group);
    if (OB_FAIL(pq_struct_of(c, sink))) {
      LOG_WARN("fail to read row group", K(ret));
    } else if (OB_FAIL(groups_.push_back(group))) {
      LOG_WARN("fail to push row group", K(ret));
    }
    return ret;
  }
  ObIArray<PqRowGroup> &groups_;
};

struct PqFileSink
{
  PqFileSink(ObIAllocator &alloc, ObIArray<PqColumnMeta> &columns,
             ObIArray<PqRowGroup> &groups, int64_t &total_rows)
    : alloc_(alloc), columns_(columns), groups_(groups), total_rows_(total_rows),
      num_rows_(0) {}
  int operator()(const int64_t fid, const int ftype, PqCursor &c)
  {
    int ret = OB_SUCCESS;
    if (3 == fid && T_I64 == ftype) {
      if (OB_FAIL(pq_zigzag(c, num_rows_))) {
        LOG_WARN("fail to read num_rows", K(ret));
      } else {
        total_rows_ = num_rows_;
      }
    } else if (2 == fid && T_LIST == ftype) {
      int64_t index = 0;
      int64_t expected_children = -1;
      auto elem = [this, &index, &expected_children](PqCursor &ec,
                                                     const int64_t /*idx*/) -> int {
        int ret = OB_SUCCESS;
        PqColumnMeta col;
        int64_t num_children = 0;
        PqSchemaSink sink(col, num_children);
        if (OB_FAIL(pq_struct_of(ec, sink))) {
          LOG_WARN("fail to read schema element", K(ret), K(index));
        } else if (0 == index) {
          expected_children = num_children;
        } else if (num_children > 0) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "nested parquet columns are");
          LOG_WARN("nested parquet column", K(ret), K(num_children));
        } else if (NULL != sink.name_ && sink.name_len_ > 0) {
          char *buf = NULL;
          if (sink.name_len_ >= 1024) {
            ret = OB_SIZE_OVERFLOW;
          } else if (OB_ISNULL(buf = static_cast<char *>(alloc_.alloc(sink.name_len_)))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(buf, sink.name_, sink.name_len_);
            col.name_ = ObString(static_cast<int32_t>(sink.name_len_), buf);
            if (OB_FAIL(columns_.push_back(col))) {
              LOG_WARN("fail to push column", K(ret));
            }
          }
        }
        ++index;
        return ret;
      };
      if (OB_FAIL(pq_list_of_structs(c, elem))) {
        LOG_WARN("fail to read schema list", K(ret));
      } else if (expected_children >= 0 && columns_.count() != expected_children) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("schema column count mismatch", K(ret), K(expected_children),
                 "found", columns_.count());
      }
    } else if (4 == fid && T_LIST == ftype) {
      PqRowGroupElemSink rg(groups_);
      if (OB_FAIL(pq_list_of_structs(c, rg))) {
        LOG_WARN("fail to read row groups", K(ret));
}
    } else if (OB_FAIL(pq_skip(c, ftype))) {
      LOG_WARN("fail to skip file metadata field", K(ret), K(fid), K(ftype));
    }
    return ret;
  }
  ObIAllocator &alloc_;
  ObIArray<PqColumnMeta> &columns_;
  ObIArray<PqRowGroup> &groups_;
  int64_t &total_rows_;
  int64_t num_rows_;
};

int pq_obj_meta_of(const int32_t phys, ObObjMeta &meta)
{
  INIT_SUCC(ret);
  switch (phys) {
    case PQ_BOOLEAN: meta.set_tinyint(); break;
    case PQ_INT32:
    case PQ_INT64: meta.set_int(); break;
    case PQ_FLOAT:
    case PQ_DOUBLE: meta.set_double(); break;
    case PQ_BYTE_ARRAY:
    case PQ_FIXED_LEN_BYTE_ARRAY: meta.set_varchar(); break;
    default:
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "this parquet column type is");
      LOG_WARN("unsupported physical type", K(ret), K(phys));
      break;
  }
  return ret;
}

// Decodes every page of one column chunk into `values`, one ObObj per row
// (null where the definition level says the row has no value).
int pq_decode_column(ObIAllocator &alloc, const char *file, const int64_t file_len,
                     const PqColumnMeta &meta, ObIArray<ObObj> &values)
{
  INIT_SUCC(ret);
  const int64_t bit_width = meta.bit_width();
  int64_t pos = meta.dict_offset_ >= 0 ? meta.dict_offset_ : meta.data_offset_;
  ObSEArray<ObObj, 64> dictionary;
  int64_t produced = 0;
  char stack_buf[8192];
  while (OB_SUCC(ret) && produced < meta.num_values_ && pos >= 0 && pos + 4 < file_len) {
    PqCursor pc(file, file_len, pos);
    PqPage page;
    if (OB_FAIL(pq_read_page_header(pc, page))) {
      LOG_WARN("fail to read page header", K(ret), K(pos));
    } else if (page.compressed_ <= 0 || pc.pos_ + page.compressed_ > file_len) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("page body out of range", K(ret), K(page), K(file_len));
    } else {
      const char *body = file + page.body_off_;
      const int64_t body_len = page.compressed_;
      const int64_t cap = page.uncompressed_ > 0 ? page.uncompressed_ : body_len;
      char *plain = cap <= static_cast<int64_t>(sizeof(stack_buf))
                        ? stack_buf
                        : static_cast<char *>(alloc.alloc(cap));
      int64_t plain_len = 0;
      const char *value_src = NULL;
      int64_t value_len = 0;
      int64_t level_prefix = 0;
      ObSEArray<int64_t, 64> defs;
      if (OB_ISNULL(plain)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("fail to alloc page buffer", K(ret), K(cap));
      } else if (PQ_PAGE_DATA_V2 == page.type_) {
        level_prefix = page.levels_len_;
        const int64_t vlen = body_len - level_prefix;
        if (level_prefix < 0 || vlen <= 0) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("bad v2 page split", K(ret), K(page), K(body_len));
        } else if (PQ_CODEC_SNAPPY == meta.codec_
                   && OB_FAIL(pq_snappy(body + level_prefix, vlen, plain, cap, plain_len))) {
          LOG_WARN("fail to decompress v2 page", K(ret));
        } else if (PQ_CODEC_SNAPPY != meta.codec_) {
          MEMCPY(plain, body + level_prefix, std::min<int64_t>(vlen, cap));
          plain_len = std::min<int64_t>(vlen, cap);
        }
        if (OB_FAIL(ret)) {
        } else if (level_prefix > 0) {
          // in v2 pages the levels sit uncompressed in front of the values
          if (OB_FAIL(pq_read_hybrid(body, level_prefix, bit_width, page.num_values_,
                                     defs))) {
            LOG_WARN("fail to decode v2 definition levels", K(ret));
          }
        }
        value_src = plain;
        value_len = plain_len;
      } else if (PQ_PAGE_DATA == page.type_ || PQ_PAGE_DICTIONARY == page.type_) {
        if (PQ_CODEC_SNAPPY == meta.codec_
            && OB_FAIL(pq_snappy(body, body_len, plain, cap, plain_len))) {
          LOG_WARN("fail to decompress page", K(ret));
        } else if (PQ_CODEC_SNAPPY != meta.codec_) {
          MEMCPY(plain, body, std::min<int64_t>(body_len, cap));
          plain_len = std::min<int64_t>(body_len, cap);
        }
        if (OB_FAIL(ret)) {
        } else {
          int64_t off = 0;
          if (PQ_PAGE_DICTIONARY != page.type_ && bit_width > 0) {
            int64_t def_len = 0;
            if (off + 4 > plain_len) {
              ret = OB_INVALID_ARGUMENT;
              LOG_WARN("truncated v1 level header", K(ret), K(plain_len));
            } else if (OB_FAIL(pq_le32(plain + off, def_len))) {
              LOG_WARN("fail to read level length", K(ret));
            } else {
              off += 4;
              if (def_len > 0 && off + def_len <= plain_len
                  && OB_FAIL(pq_read_hybrid(plain + off, def_len, bit_width,
                                            page.num_values_, defs))) {
                LOG_WARN("fail to decode v1 definition levels", K(ret));
              }
              off += def_len;
            }
          }
          value_src = plain + off;
          value_len = plain_len - off;
        }
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "this parquet page type is");
        LOG_WARN("unsupported page type", K(ret), K(page));
      }
      if (OB_FAIL(ret)) {
      } else if (PQ_PAGE_DICTIONARY == page.type_) {
        if (OB_FAIL(pq_decode_plain(alloc, meta.primitive_, meta.type_length_, value_src,
                                    value_len, page.num_values_, dictionary))) {
          LOG_WARN("fail to decode dictionary page", K(ret));
        }
      } else {
        const bool use_dictionary
            = (PQ_ENC_RLE_DICTIONARY == page.encoding_
               || PQ_ENC_PLAIN_DICTIONARY == page.encoding_)
              && dictionary.count() > 0;
        if (use_dictionary) {
          if (value_len < 1) {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("missing dictionary index width", K(ret));
          } else {
            const int64_t idx_width = static_cast<int64_t>(static_cast<uint8_t>(value_src[0]));
            const int64_t live = bit_width > 0 ? page.num_values_ - page.nulls_
                                               : page.num_values_;
            ObSEArray<int64_t, 128> indexes;
            if (OB_FAIL(pq_read_hybrid(value_src + 1, value_len - 1, idx_width, live,
                                       indexes))) {
              LOG_WARN("fail to decode dictionary indexes", K(ret));
            }
            for (int64_t i = 0; OB_SUCC(ret) && i < defs.count(); ++i) {
              if (defs.at(i) < bit_width) {
                ObObj null_value;
                null_value.set_null();
                if (OB_FAIL(values.push_back(null_value))) {
                  LOG_WARN("fail to push null", K(ret));
                }
              } else if (i >= indexes.count()) {
                ret = OB_INVALID_ARGUMENT;
                LOG_WARN("not enough dictionary indexes", K(ret), K(i));
              } else if (indexes.at(i) < 0 || indexes.at(i) >= dictionary.count()) {
                ret = OB_INVALID_ARGUMENT;
                LOG_WARN("dictionary index out of range", K(ret), K(indexes.at(i)));
              } else if (OB_FAIL(values.push_back(dictionary.at(indexes.at(i))))) {
                LOG_WARN("fail to push value", K(ret));
              }
            }
            if (OB_SUCC(ret) && 0 == defs.count()) {
              for (int64_t i = 0; OB_SUCC(ret) && i < indexes.count(); ++i) {
                if (indexes.at(i) < 0 || indexes.at(i) >= dictionary.count()) {
                  ret = OB_INVALID_ARGUMENT;
                } else if (OB_FAIL(values.push_back(dictionary.at(indexes.at(i))))) {
                  LOG_WARN("fail to push value", K(ret));
                }
              }
            }
            produced += page.num_values_;
          }
        } else {
          const int64_t live = bit_width > 0 ? page.num_values_ - page.nulls_
                                            : page.num_values_;
          ObSEArray<ObObj, 64> present;
          if (OB_FAIL(pq_decode_plain(alloc, meta.primitive_, meta.type_length_, value_src,
                                      value_len, live, present))) {
            LOG_WARN("fail to decode plain values", K(ret), K(live), K(value_len));
          }
          int64_t taken = 0;
          for (int64_t i = 0; OB_SUCC(ret) && i < page.num_values_; ++i) {
            ObObj value;
            value.set_null();
            if (0 == defs.count() || defs.at(i) >= bit_width) {
              if (taken >= present.count()) {
                ret = OB_INVALID_ARGUMENT;
                LOG_WARN("not enough decoded values", K(ret), K(i), "count",
                         present.count());
              } else {
                value = present.at(taken++);
              }
            }
            if (OB_FAIL(ret)) {
            } else if (OB_FAIL(values.push_back(value))) {
              LOG_WARN("fail to push value", K(ret));
            }
          }
          produced += page.num_values_;
        }
      }
      pos = page.body_off_ + page.compressed_;
    }
  }
  if (OB_SUCC(ret) && produced != meta.num_values_) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("parquet column did not produce all values", K(ret), K(produced), K(meta));
  }
  return ret;
}
} // namespace

int ObFileTableReader::load_parquet(ObIAllocator &alloc, const char *buf,
                                    const int64_t buf_len,
                                    ObIArray<ObFileTableColumn> &schema,
                                    ObIArray<ObFileTableRow> &rows)
{
  INIT_SUCC(ret);
  schema.reset();
  rows.reset();
  ObSEArray<PqColumnMeta, 8> columns;
  ObSEArray<PqRowGroup, 4> groups;
  int64_t total_rows = 0;
  if (OB_ISNULL(buf) || buf_len < 12) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("parquet file is too small", K(ret), K(buf_len));
  } else if (0 != MEMCMP(buf, "PAR1", 4) || 0 != MEMCMP(buf + buf_len - 4, "PAR1", 4)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parquet file magic");
    LOG_WARN("bad parquet magic", K(ret));
  } else {
    int64_t meta_len = 0;
    if (OB_FAIL(pq_le32(buf + buf_len - 8, meta_len))) {
      LOG_WARN("fail to read footer length", K(ret));
    } else if (meta_len <= 0 || meta_len + 8 > buf_len) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("bad parquet footer length", K(ret), K(meta_len), K(buf_len));
    } else {
      PqCursor footer(buf, buf_len, buf_len - 8 - meta_len);
      PqFileSink sink(alloc, columns, groups, total_rows);
      if (OB_FAIL(pq_struct_of(footer, sink))) {
        LOG_WARN("fail to read parquet footer", K(ret));
      } else if (columns.empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("parquet file has no columns", K(ret));
      } else if (0 == total_rows) {
        // an empty file is an empty table, but the schema is still needed
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    ObFileTableColumn col;
    col.name_ = columns.at(i).name_;
    if (OB_FAIL(pq_obj_meta_of(columns.at(i).primitive_, col.meta_))) {
      LOG_WARN("fail to map parquet type", K(ret), K(i), K(columns.at(i)));
    } else if (PQ_OPTIONAL == columns.at(i).repetition_) {
      col.nullable_ = true;
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(schema.push_back(col))) {
      LOG_WARN("fail to push schema column", K(ret));
    }
  }
  ObSEArray<ObSEArray<ObObj, 32>, 8> per_column;
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    if (OB_FAIL(per_column.push_back(ObSEArray<ObObj, 32>()))) {
      LOG_WARN("fail to prepare column values", K(ret));
    }
  }
  for (int64_t g = 0; OB_SUCC(ret) && g < groups.count(); ++g) {
    const PqRowGroup &group = groups.at(g);
    if (group.cols_.count() != columns.count()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("row group column count mismatch", K(ret), "chunks", group.cols_.count(),
               "schema", columns.count(), K(group));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < group.cols_.count(); ++i) {
      const PqColumnMeta &layout = group.cols_.at(i);
      if (PQ_CODEC_UNCOMPRESSED != layout.codec_ && PQ_CODEC_SNAPPY != layout.codec_) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "this parquet codec is");
        LOG_WARN("unsupported codec", K(ret), K(layout));
      } else if (PQ_INT96 == layout.primitive_) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "parquet INT96 columns are");
      } else {
        PqColumnMeta full = columns.at(i);
        full.primitive_ = layout.primitive_;
        full.codec_ = layout.codec_;
        full.num_values_ = layout.num_values_;
        full.data_offset_ = layout.data_offset_;
        full.dict_offset_ = layout.dict_offset_;
        if (full.num_values_ <= 0 || full.data_offset_ < 0) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("bad parquet column layout", K(ret), K(full), K(layout));
        } else {
          ObSEArray<ObObj, 32> values;
          if (OB_FAIL(pq_decode_column(alloc, buf, buf_len, full, values))) {
            LOG_WARN("fail to decode parquet column", K(ret), K(g), K(i), K(full));
          } else if (values.count() != full.num_values_) {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("parquet column produced the wrong row count", K(ret), K(g), K(i),
                     "values", values.count(), K(full));
          } else {
            for (int64_t k = 0; OB_SUCC(ret) && k < values.count(); ++k) {
              if (OB_FAIL(per_column.at(i).push_back(values.at(k)))) {
                LOG_WARN("fail to append value", K(ret));
              }
            }
          }
        }
      }
    }
  }
  for (int64_t r = 0; OB_SUCC(ret) && r < total_rows; ++r) {
    ObFileTableRow row;
    for (int64_t c = 0; OB_SUCC(ret) && c < per_column.count(); ++c) {
      ObObj value;
      value.set_null();
      if (r < per_column.at(c).count()) {
        value = per_column.at(c).at(r);
      }
      if (OB_FAIL(row.push_back(value))) {
        LOG_WARN("fail to push row value", K(ret));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(rows.push_back(row))) {
      LOG_WARN("fail to push row", K(ret));
    }
  }
  if (OB_SUCC(ret) && !per_column.empty() && total_rows != rows.count()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("parquet row count mismatch", K(ret), K(total_rows), "rows", rows.count());
  }
  return ret;
}

int ObFileTableReader::load_glob(ObIAllocator &alloc, const char *path, const int64_t path_len,
                                 ObIArray<ObFileTableColumn> &schema,
                                 ObIArray<ObFileTableRow> &rows)
{
  INIT_SUCC(ret);
  schema.reset();
  rows.reset();
  char *zero_term = NULL;
  if (OB_ISNULL(path) || path_len <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(zero_term = static_cast<char *>(alloc.alloc(path_len + 1)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(zero_term, path, path_len);
    zero_term[path_len] = '\0';
    char *slash = strrchr(zero_term, '/');
    const char *dir = NULL;
    const char *pat = NULL;
    int64_t pat_len = 0;
    if (NULL == slash) {
      dir = ".";
      pat = zero_term;
      pat_len = path_len;
    } else {
      slash[0] = '\0';
      dir = ('\0' == zero_term[0]) ? "/" : zero_term;
      pat = slash + 1;
      pat_len = strlen(pat);
    }
    {
      ObFileTableColumn col;
      char *name_buf = NULL;
      if (OB_ISNULL(name_buf = static_cast<char *>(alloc.alloc(4)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(name_buf, "file", 4);
        col.name_ = ObString(4, name_buf);
        col.meta_.set_type(ObVarcharType);
        col.nullable_ = false;
        if (OB_FAIL(schema.push_back(col))) {
          LOG_WARN("fail to push glob column", K(ret));
        }
      }
    }
    if (OB_FAIL(ret) || 0 == pat_len) {
      // nothing to list
    } else {
      DIR *dp = ::opendir(dir);
      if (NULL == dp) {
        ret = OB_IO_ERROR;
        LOG_WARN("fail to open directory", K(ret), K(dir), K(errno));
      } else {
        const int64_t dir_len = strlen(dir);
        const bool root = ('/' == dir[dir_len - 1]);
        struct dirent *ent = NULL;
        while (NULL != (ent = ::readdir(dp))) {
          const int64_t name_len = strlen(ent->d_name);
          if (0 == name_len || '.' == ent->d_name[0]) {
            continue;  // skip dotfiles, like glob() does
          } else if (!glob_name_match(pat, pat_len, ent->d_name, name_len)) {
            continue;
          } else {
            const int64_t need = dir_len + 1 + name_len + 1;
            char *full = NULL;
            ObFileTableRow row;
            ObObj value;
            if (OB_ISNULL(full = static_cast<char *>(alloc.alloc(need)))) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else {
              const int64_t n = root ? snprintf(full, need, "%s%s", dir, ent->d_name)
                                     : snprintf(full, need, "%s/%s", dir, ent->d_name);
              value.set_varchar(ObString(static_cast<int32_t>(n), full));
              if (n <= 0 || n >= need) {
                ret = OB_SIZE_OVERFLOW;
              } else if (OB_FAIL(row.push_back(value))) {
                LOG_WARN("fail to push glob value", K(ret));
              } else if (OB_FAIL(rows.push_back(row))) {
                LOG_WARN("fail to push glob row", K(ret));
              }
            }
          }
        }
        ::closedir(dp);
      }
    }
  }
  return ret;
}

int ObFileTableReader::load_impl(const char *path, const int64_t path_len,
                                 const ObFileTableFormat fmt, const int64_t sample_size,
                                 ObIAllocator &alloc,
                                 ObIArray<ObFileTableColumn> &schema,
                                 ObIArray<ObFileTableRow> &rows)
{
  int ret = OB_SUCCESS;
  schema.reset();
  rows.reset();
  char *buf = nullptr;
  int64_t buf_len = 0;
  if (FILE_FMT_GLOB == fmt) {
    if (OB_FAIL(load_glob(alloc, path, path_len, schema, rows))) {
      LOG_WARN("fail to list files for glob", K(ret), KPHEX(path, path_len));
    }
  } else if (FILE_FMT_PARQUET == fmt) {
    if (OB_FAIL(read_file(alloc, path, path_len, buf, buf_len))) {
      LOG_WARN("fail to read parquet file", K(ret), KPHEX(path, path_len));
    } else if (OB_FAIL(load_parquet(alloc, buf, buf_len, schema, rows))) {
      LOG_WARN("fail to decode parquet file", K(ret), K(buf_len), KPHEX(path, path_len));
    }
  } else if (FILE_FMT_CSV != fmt && FILE_FMT_JSONL != fmt) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("format not readable yet through the file table function",
             K(ret), K(fmt), KPHEX(path, path_len));
  } else if (OB_FAIL(read_file(alloc, path, path_len, buf, buf_len))) {
    LOG_WARN("fail to read file", K(ret), KPHEX(path, path_len));
  } else if (FILE_FMT_JSONL == fmt) {
    ObSEArray<ObString, 16> headers;
    ObSEArray<ObSEArray<ObString, 16>, 8> records;
    if (OB_FAIL(parse_jsonl(alloc, buf, buf_len, headers, records))) {
      LOG_WARN("fail to parse jsonl", K(ret), K(buf_len));
    } else if (headers.empty()) {
      // an empty file is an empty table
    } else if (OB_FAIL(infer_and_convert(alloc, headers, records, 0, sample_size,
                                         schema, rows))) {
      LOG_WARN("fail to infer jsonl schema", K(ret));
    }
  } else {
    ObSEArray<ObSEArray<ObString, 16>, 8> records;
    if (OB_FAIL(parse_csv(alloc, buf, buf_len, records))) {
      LOG_WARN("fail to parse csv", K(ret), K(buf_len));
    } else if (records.empty()) {
      // an empty file is an empty table
    } else if (OB_FAIL(infer_and_convert(alloc, records.at(0), records, 1, sample_size,
                                         schema, rows))) {
      LOG_WARN("fail to infer schema", K(ret));
    }
  }
  return ret;
}

namespace
{
// The type spellings DuckDB's DESCRIBE prints for the shapes we can infer.
const char *describe_type_name(const ObObjMeta &meta)
{
  const char *name = "VARCHAR";
  if (ObIntType == meta.get_type()) {
    name = "BIGINT";
  } else if (ObTinyIntType == meta.get_type()) {
    name = "BOOLEAN";
  } else if (ObDoubleType == meta.get_type() || ObFloatType == meta.get_type()) {
    name = "DOUBLE";
  } else if (ObDateType == meta.get_type()) {
    name = "DATE";
  } else if (ObDateTimeType == meta.get_type() || ObTimestampType == meta.get_type()) {
    name = "DATETIME";
  }
  return name;
}

int push_describe_text(ObIAllocator &alloc, ObFileTableRow &row, const char *text,
                       const int64_t len_in = 0)
{
  INIT_SUCC(ret);
  const int64_t len = len_in > 0 ? len_in : strlen(text);
  char *buf = NULL;
  if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(len)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to alloc describe text", K(ret), K(len));
  } else {
    MEMCPY(buf, text, len);
    ObObj value;
    value.set_varchar(ObString(static_cast<int32_t>(len), buf));
    if (OB_FAIL(row.push_back(value))) {
      LOG_WARN("fail to push describe value", K(ret));
    }
  }
  return ret;
}
} // namespace

int ObFileTableReader::load(const char *path, const int64_t path_len,
                            const ObFileTableFormat fmt, const int64_t sample_size,
                            const bool describe, ObIAllocator &alloc,
                            ObIArray<ObFileTableColumn> &schema,
                            ObIArray<ObFileTableRow> &rows)
{
  INIT_SUCC(ret);
  if (!describe) {
    if (OB_FAIL(load_impl(path, path_len, fmt, sample_size, alloc, schema, rows))) {
      LOG_WARN("fail to load file table", K(ret), K(fmt), KPHEX(path, path_len));
    }
  } else {
    // DESCRIBE SELECT ... FROM read_x(...) is a table of the inferred schema, so
    // the file still has to be sampled, but its rows are never surfaced.
    ObSEArray<ObFileTableColumn, 16> inferred;
    ObSEArray<ObFileTableRow, 1> discarded;
    static const char *column_names[6] = {"column_name", "column_type", "null",
                                          "key", "default", "extra"};
    schema.reset();
    rows.reset();
    if (OB_FAIL(load_impl(path, path_len, fmt, sample_size, alloc, inferred, discarded))) {
      LOG_WARN("fail to infer the schema being described", K(ret), K(fmt));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < 6; ++i) {
      ObFileTableColumn col;
      char *buf = NULL;
      const int64_t len = strlen(column_names[i]);
      if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(len)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(buf, column_names[i], len);
        col.name_ = ObString(static_cast<int32_t>(len), buf);
        col.meta_.set_varchar();
        col.nullable_ = (i >= 3);
        if (OB_FAIL(schema.push_back(col))) {
          LOG_WARN("fail to push describe column", K(ret), K(i));
        }
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < inferred.count(); ++i) {
      ObFileTableRow row;
      if (OB_FAIL(push_describe_text(alloc, row,
                                     inferred.at(i).name_.ptr(),
                                     inferred.at(i).name_.length()))) {
        LOG_WARN("fail to push column name", K(ret), K(i));
      } else if (OB_FAIL(push_describe_text(alloc, row,
                                            describe_type_name(inferred.at(i).meta_)))) {
        LOG_WARN("fail to push column type", K(ret), K(i));
      } else if (OB_FAIL(push_describe_text(alloc, row,
                                            inferred.at(i).nullable_ ? "YES" : "NO"))) {
        LOG_WARN("fail to push nullability", K(ret), K(i));
      }
      for (int64_t k = 3; OB_SUCC(ret) && k < 6; ++k) {
        ObObj value;
        value.set_null();
        if (OB_FAIL(row.push_back(value))) {
          LOG_WARN("fail to push null field", K(ret), K(k));
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(rows.push_back(row))) {
        LOG_WARN("fail to push describe row", K(ret), K(i));
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
