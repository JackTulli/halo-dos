#!/bin/bash
# the retail server list's host test (Linux, gcc): build_test.sh -> /tmp/retail_test
set -e
here=$(cd "$(dirname "$0")" && pwd)
gcc -O1 -g -Wall -Wno-pointer-sign -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
	-Wno-format-truncation -Wno-sign-compare -o /tmp/retail_test \
	"$here/retail_test.c" "$here/../retail_browse.c" "$here/../gs_list.c" "$here/../gs_query.c" \
	"$here/../enctypex_decoder.c"
echo built /tmp/retail_test
