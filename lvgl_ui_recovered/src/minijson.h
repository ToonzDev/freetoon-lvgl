#ifndef TOON_MINIJSON_H
#define TOON_MINIJSON_H
#include <stddef.h>

/* Tiny read-only JSON path lookup, no allocation. A path is a chain of
 * object keys and array indices: "data.page.records[0].power", "[1].wNow",
 * "fields[3].key". The lookup parses only the first JSON value at `json`,
 * so a pointer returned by mj_find() can be used as the root of the next
 * lookup (iterate an array with "[i]" until it returns NULL).
 *
 * The same file is copied into freetoon-integrations/integrations/inverters;
 * keep the two copies identical. */

/* Start of the value at `path` (leading whitespace skipped), its length in
 * *len (optional). NULL when absent or the JSON is malformed. */
const char * mj_find(const char * json, const char * path, int * len);

/* String value unescaped into out (\uXXXX -> UTF-8); a number, true, false
 * or null is copied as written. 1 = found. */
int mj_str(const char * json, const char * path, char * out, size_t n);

/* Number value; a string holding a number ("12.5") counts too. 1 = found. */
int mj_num(const char * json, const char * path, double * out);

/* 1 when the value at path is true, a non-zero number or "1"/"true". */
int mj_bool(const char * json, const char * path);

#endif
