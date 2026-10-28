/*
DOS_DPMI.H

The DOS build's view of the machine: DPMI services (int 31h) of the
DOS/32A extender (or of whatever DPMI host runs the game: Windows' DOS box,
CWSDPMI), real-mode interrupts through them, conventional memory, and the
I/O ports. Only the DOS units (port/dos/dos) include this.

The game runs in a flat 32-bit model: CS, DS, ES and SS cover the whole
address space from the same base. Under DOS/32A that base is 0, so a
pointer is a linear address; dos_linear and dos_pointer convert anyway, for
hosts whose base is not 0.
*/

#ifndef __DOS_DPMI_H
#define __DOS_DPMI_H

/* ---------- real-mode calls (DPMI 0300h's register structure) */

#pragma pack(push, 1)
struct dos_registers
{
	unsigned long edi, esi, ebp, reserved, ebx, edx, ecx, eax;
	unsigned short flags, es, ds, fs, gs, ip, cs, sp, ss;
};
#pragma pack(pop)

typedef char dos_registers_size_assert[sizeof(struct dos_registers) == 50 ? 1 : -1];

#define DOS_CARRY 0x0001

/* runs real-mode interrupt number with registers (DPMI 0300h); FALSE if
the host refused (the interrupt's own result is in registers, its carry
in flags) */
int dos_real_interrupt(int number, struct dos_registers *registers);

/* the transfer buffer: conventional memory for passing data to real-mode
calls (64 KB, below 1 MB) */
#define DOS_TRANSFER_SIZE 0x10000
extern unsigned short dos_transfer_segment;
extern unsigned char *dos_transfer;

/* ---------- addresses */

/* the base of the program's segments (0 under DOS/32A) */
extern unsigned long dos_segment_base;

static __inline void *dos_pointer(unsigned long linear)
{
	return (void *)(linear - dos_segment_base);
}

static __inline unsigned long dos_linear(const void *pointer)
{
	return (unsigned long)pointer + dos_segment_base;
}

/* conventional memory at segment:offset */
static __inline void *dos_real_pointer(unsigned short segment, unsigned short offset)
{
	return dos_pointer(((unsigned long)segment << 4) + offset);
}

/* ---------- DPMI services */

/* the DPMI host's memory: a block of size bytes, and its handle; FALSE if
there is none that large */
int dos_dpmi_allocate(unsigned long size, unsigned long *linear, unsigned long *handle);
void dos_dpmi_free(unsigned long handle);
/* the largest block dos_dpmi_allocate could give now */
unsigned long dos_dpmi_largest_free_block(void);
/* physical memory (a video card's frame buffer) mapped into the address
space; 0 if it cannot be */
unsigned long dos_dpmi_map_physical(unsigned long physical, unsigned long size);
/* keeps memory from being paged out (interrupt handlers and their data) */
void dos_dpmi_lock(const void *address, unsigned long size);
/* conventional memory: paragraphs of 16 bytes; the real-mode segment, and
its protected-mode selector for dos_dos_free */
int dos_dos_allocate(unsigned short paragraphs, unsigned short *segment, unsigned short *selector);
void dos_dos_free(unsigned short selector);
/* a protected-mode interrupt handler (DPMI 0204h/0205h) */
void dos_get_interrupt_vector(int number, unsigned short *selector, unsigned long *offset);
void dos_set_interrupt_vector(int number, unsigned short selector, unsigned long offset);

/* ---------- ports */

static __inline unsigned char dos_inb(unsigned short port)
{
	unsigned char value;

	__asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static __inline void dos_outb(unsigned short port, unsigned char value)
{
	__asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static __inline unsigned short dos_inw(unsigned short port)
{
	unsigned short value;

	__asm__ __volatile__("inw %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static __inline void dos_outw(unsigned short port, unsigned short value)
{
	__asm__ __volatile__("outw %0, %1" : : "a"(value), "Nd"(port));
}

static __inline unsigned short dos_code_selector(void)
{
	unsigned short selector;

	__asm__ __volatile__("movw %%cs, %0" : "=r"(selector));
	return selector;
}

static __inline unsigned short dos_data_selector(void)
{
	unsigned short selector;

	__asm__ __volatile__("movw %%ds, %0" : "=r"(selector));
	return selector;
}

/* the processor's time stamp counter (every Pentium has one) */
static __inline unsigned long long dos_rdtsc(void)
{
	unsigned long long value;

	__asm__ __volatile__("rdtsc" : "=A"(value));
	return value;
}

/* interrupts off and on around work an interrupt handler shares */
static __inline unsigned long dos_interrupts_disable(void)
{
	unsigned long flags;

	__asm__ __volatile__("pushfl\n\tpopl %0\n\tcli" : "=r"(flags) : : "memory");
	return flags;
}

static __inline void dos_interrupts_restore(unsigned long flags)
{
	if (flags & 0x200)
		__asm__ __volatile__("sti" : : : "memory");
}

/* ---------- the rest of the DOS layer */

/* start-up (dos_start.c): the transfer buffer, the clock, the heap */
void dos_machine_initialize(void);
/* the clock (dos_start.c): time stamp counter ticks a second, measured
against the BIOS timer at start-up */
extern unsigned long long dos_tsc_frequency;
unsigned long long dos_microseconds(void);

/* the program ends (dos_start.c): undoes what the platform changed (video
mode, interrupt handlers), then DOS's exit */
void dos_exit(int code) __attribute__((noreturn));
/* the same with a message on the text screen and in the log */
void dos_exit_with_message(const char *text) __attribute__((noreturn));
/* each part that must be undone registers it here (at most 16) */
void dos_at_exit(void (*function)(void));

#endif /* __DOS_DPMI_H */
