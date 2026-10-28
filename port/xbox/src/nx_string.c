/*
NX_STRING.C

The block memory functions, a word at a time.

nxdk's C library (PDCLib) copies, fills, moves and compares memory one
byte per loop pass, and it was built without optimisation: some fifteen
instructions a byte, each byte a load and a store through the emulator's
software memory unit. The game, the graphics device, the mixer and nxdk's
own network and USB stacks all go through these, so they are defined
here, where the linker finds them before the library: the Pentium III's
string instructions move four bytes a step (rep movsl and rep stosl,
which its microcode runs at a word or more a cycle once the destination
is aligned), and memcmp compares four bytes at a time until they differ.

The direction flag is clear on entry, as the calling convention requires;
memmove sets it only inside the one block that copies backwards, and
clears it again there.
*/

#include <stddef.h>
#include <stdint.h>

typedef uint32_t __attribute__((__may_alias__, __aligned__(1))) unaligned_word;

void *memcpy(void *destination, const void *source, size_t size)
{
	void *to = destination;
	const void *from = source;

	/* a few bytes (most of the game's copies: vectors, small structures):
	plain moves, which cost less than the string instructions' start-up
	(the compiler makes no call of these loops: -fno-builtin) */
	if (size < 16)
	{
		unsigned char *d = destination;
		const unsigned char *s = source;

		for (; size >= 4; size -= 4, d += 4, s += 4)
			*(unaligned_word *)d = *(const unaligned_word *)s;
		for (; size; size--)
			*d++ = *s++;
		return destination;
	}
	if (size >= 16)
	{
		/* bytes up to an aligned destination, then words */
		size_t head = (0u - (uintptr_t)to) & 3;
		size_t words;

		size -= head;
		words = size >> 2;
		size &= 3;
		__asm__ __volatile__("rep movsb" : "+D"(to), "+S"(from), "+c"(head) : : "memory");
		__asm__ __volatile__("rep movsl" : "+D"(to), "+S"(from), "+c"(words) : : "memory");
	}
	__asm__ __volatile__("rep movsb" : "+D"(to), "+S"(from), "+c"(size) : : "memory");
	return destination;
}

void *memmove(void *destination, const void *source, size_t size)
{
	unsigned char *to = destination;
	const unsigned char *from = source;

	if (to == from || size == 0)
		return destination;
	if (to < from || to >= from + size)
		return memcpy(destination, source, size);
	/* the destination overlaps the source from above: backwards, the odd
	bytes at the end first, then words down to the start */
	{
		unsigned char *last_to = to + size - 1;
		const unsigned char *last_from = from + size - 1;
		size_t tail = size & 3;
		size_t words = size >> 2;

		__asm__ __volatile__(
			"std\n\t"
			"rep movsb\n\t"
			"subl $3, %%esi\n\t"
			"subl $3, %%edi\n\t"
			"movl %3, %%ecx\n\t"
			"rep movsl\n\t"
			"cld"
			: "+D"(last_to), "+S"(last_from), "+c"(tail)
			: "r"(words)
			: "memory", "cc");
	}
	return destination;
}

void *memset(void *destination, int value, size_t size)
{
	void *to = destination;
	uint32_t pattern = (uint32_t)(unsigned char)value * 0x01010101u;

	if (size < 16)
	{
		unsigned char *d = destination;

		for (; size >= 4; size -= 4, d += 4)
			*(unaligned_word *)d = pattern;
		for (; size; size--)
			*d++ = (unsigned char)pattern;
		return destination;
	}
	if (size >= 16)
	{
		size_t head = (0u - (uintptr_t)to) & 3;
		size_t words;

		size -= head;
		words = size >> 2;
		size &= 3;
		__asm__ __volatile__("rep stosb" : "+D"(to), "+c"(head) : "a"(pattern) : "memory");
		__asm__ __volatile__("rep stosl" : "+D"(to), "+c"(words) : "a"(pattern) : "memory");
	}
	__asm__ __volatile__("rep stosb" : "+D"(to), "+c"(size) : "a"(pattern) : "memory");
	return destination;
}

int memcmp(const void *first, const void *second, size_t size)
{
	const unsigned char *a = first;
	const unsigned char *b = second;

	while (size >= 4 && *(const unaligned_word *)a == *(const unaligned_word *)b)
	{
		a += 4;
		b += 4;
		size -= 4;
	}
	for (; size; size--, a++, b++)
	{
		if (*a != *b)
			return *a - *b;
	}
	return 0;
}
