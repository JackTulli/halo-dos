/*
ZSTD_DECOMPRESS.C

Zstandard's decoder (zstd 1.5.6, BSD licence: zstd/LICENSE), the files of
lib/common and lib/decompress it needs, built as one unit for the DOS and
Windows 95 builds: Custom Edition maps downloaded from Chimera's map
repository come compressed by Invader, the map's header as it is and the
rest one zstd stream (port/dos/game/dos_map_download.c unpacks them).

For a Pentium with no MMX or SSE: no assembly, no BMI2 code paths, no
legacy formats, no tracing.
*/

/* (clang for the MSVC target says it is MSVC: zstd would reach for
<intrin.h>, which this C library has not; its GCC paths are clang's) */
#undef _MSC_VER

#define ZSTD_DISABLE_ASM 1
#define DYNAMIC_BMI2 0
#define ZSTD_LEGACY_SUPPORT 0
#define ZSTD_TRACE 0
#define ZSTD_NO_INTRINSICS 1
#define XXH_NAMESPACE ZSTD_
#define DEBUGLEVEL 0

#include "zstd/lib/common/debug.c"
#include "zstd/lib/common/entropy_common.c"
#include "zstd/lib/common/error_private.c"
#include "zstd/lib/common/fse_decompress.c"
#include "zstd/lib/common/xxhash.c"
#include "zstd/lib/common/zstd_common.c"
#include "zstd/lib/decompress/huf_decompress.c"
#include "zstd/lib/decompress/zstd_ddict.c"
#include "zstd/lib/decompress/zstd_decompress.c"
#include "zstd/lib/decompress/zstd_decompress_block.c"
