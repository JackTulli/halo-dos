/*
DOS_COMDAT.C

External copies of the game's header inline functions that some units call
through a prototype (the DOS and Windows 95 builds' version of
port/linux/game/msvc_comdat.c).

The Windows build gives each a weak definition in msvc_comdat.c; Open
Watcom 1.9's linker crashes on a weak definition of a name other objects
also have as a COMDAT. Here they keep MSVC's linkage (pick-any COMDATs,
which clang's Microsoft target gives C __inline functions), and taking
their addresses makes clang emit them. A name a link reports undefined
belongs in the table.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "bitmaps/bitmaps_inlines.h"
#include "physics/collisions.h"

static void *const dos_inline_copies[] __attribute__((used)) =
{
	(void *)point_from_line3d,
	(void *)real_random_range,
	(void *)real_local_random_range,
	(void *)uniform_cubic_spline_vector3d,
	(void *)signed_angular_difference,
	(void *)real_argb_color_to_pixel32,
	(void *)real_rgb_color_to_pixel32,
	(void *)distance3d,
	(void *)magnitude_squared3d,
	(void *)vector_from_points3d,
	(void *)collision_test_line,
};
