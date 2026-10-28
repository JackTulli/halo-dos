/* t_picture_guard_alloc.h: for the device's units, compiled with -include: their allocations end at a page that cannot be touched (t_picture.c has the functions) */
#include <stdlib.h>
#include <string.h>
void *guard_malloc(size_t size);
void *guard_calloc(size_t count, size_t size);
void *guard_realloc(void *block, size_t size);
void guard_free(void *block);
#define malloc guard_malloc
#define calloc guard_calloc
#define realloc guard_realloc
#define free guard_free
