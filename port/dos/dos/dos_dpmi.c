/*
DOS_DPMI.C

The DPMI services of dos_dpmi.h, by int 31h. The carry flag comes back
through flag-output operands (=@ccc), which need a register of their own:
the clobber lists name only what a service may change (DPMI keeps every
register it does not answer in).
*/

#include "dos_dpmi.h"

#include <string.h>

unsigned long dos_segment_base;
unsigned short dos_transfer_segment;
unsigned char *dos_transfer;

int dos_real_interrupt(int number, struct dos_registers *registers)
{
	unsigned long eax = 0x0300, ebx = (unsigned long)(number & 0xFF), ecx = 0;
	int carry;

	/* (the real-mode handler returns with these flags: interrupts on; the
	host gives the call a real-mode stack of its own) */
	registers->flags |= 0x0200;
	registers->sp = 0;
	registers->ss = 0;
	__asm__ __volatile__(
		"int $0x31"
		: "+a"(eax), "+b"(ebx), "+c"(ecx), "=@ccc"(carry)
		: "D"(registers)
		: "edx", "esi", "memory");
	return !carry;
}

unsigned long dos_dpmi_largest_free_block(void)
{
	unsigned long information[12];
	unsigned long eax = 0x0500;
	int carry;

	memset(information, 0xFF, sizeof(information));
	__asm__ __volatile__(
		"int $0x31"
		: "+a"(eax), "=@ccc"(carry)
		: "D"(information)
		: "ebx", "ecx", "edx", "esi", "memory");
	return carry ? 0 : information[0];
}

int dos_dpmi_allocate(unsigned long size, unsigned long *linear, unsigned long *handle)
{
	unsigned long eax = 0x0501, ebx = size >> 16, ecx = size & 0xFFFF, esi, edi;
	int carry;

	__asm__ __volatile__(
		"int $0x31"
		: "+a"(eax), "+b"(ebx), "+c"(ecx), "=S"(esi), "=D"(edi), "=@ccc"(carry)
		:
		: "memory");
	if (carry)
		return 0;
	*linear = ((ebx & 0xFFFF) << 16) | (ecx & 0xFFFF);
	*handle = ((esi & 0xFFFF) << 16) | (edi & 0xFFFF);
	return 1;
}

void dos_dpmi_free(unsigned long handle)
{
	unsigned long eax = 0x0502, esi = handle >> 16, edi = handle & 0xFFFF;

	__asm__ __volatile__("int $0x31" : "+a"(eax), "+S"(esi), "+D"(edi) : : "ebx", "ecx", "edx", "memory", "cc");
}

unsigned long dos_dpmi_map_physical(unsigned long physical, unsigned long size)
{
	unsigned long eax = 0x0800, ebx = physical >> 16, ecx = physical & 0xFFFF;
	unsigned long esi = size >> 16, edi = size & 0xFFFF;
	int carry;

	__asm__ __volatile__(
		"int $0x31"
		: "+a"(eax), "+b"(ebx), "+c"(ecx), "=@ccc"(carry), "+S"(esi), "+D"(edi)
		:
		: "memory");
	if (carry)
		return 0;
	return ((ebx & 0xFFFF) << 16) | (ecx & 0xFFFF);
}

void dos_dpmi_lock(const void *address, unsigned long size)
{
	unsigned long linear = dos_linear(address);
	unsigned long eax = 0x0600, ebx = linear >> 16, ecx = linear & 0xFFFF;
	unsigned long esi = size >> 16, edi = size & 0xFFFF;

	__asm__ __volatile__("int $0x31" : "+a"(eax), "+b"(ebx), "+c"(ecx), "+S"(esi), "+D"(edi) : : "edx", "memory",
		"cc");
}

int dos_dos_allocate(unsigned short paragraphs, unsigned short *segment, unsigned short *selector)
{
	unsigned long eax = 0x0100, ebx = paragraphs, edx;
	int carry;

	__asm__ __volatile__(
		"int $0x31"
		: "+a"(eax), "+b"(ebx), "=@ccc"(carry), "=d"(edx)
		:
		: "memory");
	if (carry)
		return 0;
	*segment = (unsigned short)eax;
	*selector = (unsigned short)edx;
	return 1;
}

void dos_dos_free(unsigned short selector)
{
	unsigned long eax = 0x0101, edx = selector;

	__asm__ __volatile__("int $0x31" : "+a"(eax), "+d"(edx) : : "ebx", "ecx", "esi", "edi", "memory", "cc");
}

void dos_get_interrupt_vector(int number, unsigned short *selector, unsigned long *offset)
{
	unsigned long eax = 0x0204, ebx = (unsigned long)number, ecx, edx;

	__asm__ __volatile__("int $0x31" : "+a"(eax), "+b"(ebx), "=c"(ecx), "=d"(edx) : : "esi", "edi", "memory", "cc");
	*selector = (unsigned short)ecx;
	*offset = edx;
}

void dos_set_interrupt_vector(int number, unsigned short selector, unsigned long offset)
{
	unsigned long eax = 0x0205, ebx = (unsigned long)number, ecx = selector, edx = offset;

	__asm__ __volatile__("int $0x31" : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx) : : "esi", "edi", "memory", "cc");
}

/* the base of a selector's segment (DPMI 0006h) */
unsigned long dos_selector_base(unsigned short selector)
{
	unsigned long eax = 0x0006, ebx = selector, ecx, edx;

	__asm__ __volatile__("int $0x31" : "+a"(eax), "+b"(ebx), "=c"(ecx), "=d"(edx) : : "esi", "edi", "memory", "cc");
	return ((ecx & 0xFFFF) << 16) | (edx & 0xFFFF);
}
