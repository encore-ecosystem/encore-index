#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
source_file="$root/runtime/runtime.c"
portable="$root/packages/core/runtime.c"
platform="$root/packages/platform/runtime.c"

test -f "$source_file"

{
  sed -n '1,70p' "$source_file"
  sed -n '267,868p' "$source_file"
  sed -n '938,1038p' "$source_file"
  sed -n '1089,1342p' "$source_file"
} > "$portable"

perl -pi -e '
  s/^static encore_str encore_empty_str/encore_str encore_empty_str/;
  s/^static encore_str encore_from_owned_buffer/encore_str encore_from_owned_buffer/;
  s/^static encore_str encore_from_cstr_copy/encore_str encore_from_cstr_copy/;
  s/^static char \*encore_str_data/char *encore_str_data/;
  s/^static size_t encore_str_size/size_t encore_str_size/;
  s/^static char \*encore_to_cstr/char *encore_to_cstr/;
' "$portable"

{
  sed -n '1,70p' "$source_file"
  printf '%s\n' \
    'typedef struct { _Atomic size_t ref_count; size_t len; char data[]; } encore_str_object;' \
    'typedef struct { encore_str_object *object; } encore_str;' \
    'extern encore_str encore_empty_str(void);' \
    'extern encore_str encore_from_owned_buffer(char *buffer, size_t len);' \
    'extern encore_str encore_from_cstr_copy(const char *value);' \
    'extern char *encore_str_data(encore_str value);' \
    'extern size_t encore_str_size(encore_str value);' \
    'extern char *encore_to_cstr(encore_str value);'
  sed -n '72,265p' "$source_file"
  sed -n '869,936p' "$source_file"
  sed -n '1039,1088p' "$source_file"
  sed -n '1343,$p' "$source_file"
} > "$platform"

perl -0777 -pi -e 's/\n+\z/\n/' "$portable" "$platform"
clang -std=c11 -fsyntax-only "$portable"
clang -std=c11 -fsyntax-only "$platform"
