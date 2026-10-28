/*
DOS_VOODOO.C

A 3dfx Voodoo Graphics card for the DOS build (the setting renderer =
voodoo): found on the PCI bus (3dfx 121Ah, device 1), its 16 MB mapped, its
frame buffer laid out for 640x480 with a back buffer and a depth buffer,
its video turned on (the card takes the monitor from the VGA card through
its pass-through cable), and handed to the renderer (port/dos/render/
r_voodoo.c), which draws everything through it. At the end (and after a
crash) the pass-through goes back to the VGA card.

The card is brought up as 3dfx's own initialization does it (sst1init, the
part of Glide 2 that starts a Voodoo Graphics, released with Glide's
sources): its units reset, their clock delays and memory timings set, the
DAC found (an ICS5342, an AT&T ATT20C409 or a TI TVP3409) and its two clock
synthesizers programmed -- the graphics clock at 50 MHz, the video clock
for 640x480 at 60 Hz -- the DRAM refreshed, the memory FIFO on, the video
unit set for 24-bit output with the card's filter, and a straight gamma
table. DOSBox-X's emulation (its [voodoo] section, voodoo_card = software or
opengl) takes all of it and keeps what it models: the memory layout and the
video size; real cards have not been tried yet. Settings for a card that
does not come up: voodoo_clock = the graphics clock in MHz (16 to 60, 50 by
default), voodoo_init = 0 to leave all of this out (the memory layout and
video registers only, as before). A Voodoo 2 (device 2) is not driven yet.
*/

#include <stdio.h>
#include <string.h>

#include "halo_system.h"
#include "dos_dpmi.h"
#include "dos_pci.h"
#include "r_voodoo.h"

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

long dos_setting_integer(const char *name, long default_value);

#define WIDTH 640
#define HEIGHT 480

/* registers (byte offsets) */
#define STATUS 0x000
#define CLIP_LEFT_RIGHT 0x118
#define CLIP_LOW_Y_HIGH_Y 0x11C
#define FBI_INIT4 0x200
#define BACK_PORCH 0x208
#define VIDEO_DIMENSIONS 0x20C
#define FBI_INIT0 0x210
#define FBI_INIT1 0x214
#define FBI_INIT2 0x218
#define FBI_INIT3 0x21C
#define H_SYNC 0x220
#define V_SYNC 0x224
#define CLUT_DATA 0x228
#define DAC_DATA 0x22C
#define VIDEO_FILTER_THRESHOLD 0x230
#define T_LOD 0x304
#define TREX_INIT0 0x31C
#define TREX_INIT1 0x320
/* a texture unit's own copy of a register (its chip select) */
#define TMU(n) (0x800U << (n))

/* status */
#define STATUS_FBI_BUSY 0x80UL
#define STATUS_TMU_BUSY 0x100UL
/* PCI configuration: initEnable and its bits, the bus snoops, the video
clock */
#define PCI_INIT_ENABLE 0x40
#define INIT_WRITES 0x1UL
#define INIT_PCI_FIFO 0x2UL
#define INIT_DAC_READS 0x4UL
#define PCI_BUS_SNOOP_0 0x44
#define PCI_BUS_SNOOP_1 0x48
#define PCI_VIDEO_CLOCK_ON 0xC0
#define PCI_VIDEO_CLOCK_OFF 0xE0
/* fbiInit0 */
#define VGA_PASS 0x1UL
#define GRAPHICS_RESET 0x2UL
#define PCI_FIFO_RESET 0x4UL
#define PCI_FIFO_LOW_SHIFT 6
#define PCI_FIFO_LOW (0x1FUL << PCI_FIFO_LOW_SHIFT)
#define LFB_THROUGH_MEMORY_FIFO 0x800UL
#define TEXTURES_THROUGH_MEMORY_FIFO 0x1000UL
#define MEMORY_FIFO_ON 0x2000UL
#define MEMORY_FIFO_HIGH_SHIFT 14
#define MEMORY_FIFO_HIGH (0x7FFUL << MEMORY_FIFO_HIGH_SHIFT)
#define MEMORY_FIFO_BURST_SHIFT 25
#define MEMORY_FIFO_BURST (0x3FUL << MEMORY_FIFO_BURST_SHIFT)
/* fbiInit1 */
#define PCI_WRITE_WAIT 0x2UL
#define LFB_READS 0x8UL
#define TILES_SHIFT 4
#define TILES (0xFUL << TILES_SHIFT)
#define VIDEO_RESET 0x100UL
#define VIDEO_BLANK 0x1000UL
#define VIDEO_DRIVE (0x2000UL | 0x4000UL | 0x8000UL | 0x10000UL)
#define VIDEO_CLOCK_HALF 0x100000UL
#define VIDEO_24_BIT 0x400000UL
#define VIDEO_FILTER 0x2000000UL
#define VIDEO_OUT_DELAY_SHIFT 27
#define VIDEO_IN_DELAY_SHIFT 29
#define VIDEO_BITS 0x7E7FFE00UL
/* fbiInit2 */
#define FAST_RAS_READ 0x20UL
#define FAST_READ_AHEAD_WRITE 0x80UL
#define SWAP_ALGORITHM 0x600UL
#define BUFFER_OFFSET_SHIFT 11
#define BUFFER_OFFSET (0x1FFUL << BUFFER_OFFSET_SHIFT)
#define DRAM_READ_AHEAD 0x200000UL
#define DRAM_REFRESH 0x400000UL
#define DRAM_REFRESH_COUNT_SHIFT 23
#define DRAM_REFRESH_COUNT (0x1FFUL << DRAM_REFRESH_COUNT_SHIFT)
/* fbiInit3 */
#define VIDEO_FIFO_SHIFT 1
#define VIDEO_FIFO (0x1FUL << VIDEO_FIFO_SHIFT)
#define VGA_PASS_POWER_ON 0x1000UL
#define FT_DELAY_SHIFT 13
#define FT_DELAY (0xFUL << FT_DELAY_SHIFT)
#define TF_FIFO_SHIFT 17
#define TF_FIFO (0x1FUL << TF_FIFO_SHIFT)
#define Y_ORIGIN_SHIFT 22
#define Y_ORIGIN (0x3FFUL << Y_ORIGIN_SHIFT)
/* fbiInit4 */
#define LFB_READ_AHEAD 0x2UL
#define MEMORY_FIFO_LOW_SHIFT 2
#define MEMORY_FIFO_LOW (0x3FUL << MEMORY_FIFO_LOW_SHIFT)
#define MEMORY_FIFO_BASE_SHIFT 8
#define MEMORY_FIFO_BASE (0x3FFUL << MEMORY_FIFO_BASE_SHIFT)
#define MEMORY_FIFO_ROLL_SHIFT 18
#define MEMORY_FIFO_ROLL (0x3FFUL << MEMORY_FIFO_ROLL_SHIFT)
/* the init registers' starting values (sst1init's defaults) */
#define FBI_INIT0_DEFAULT 0x00000410UL
#define FBI_INIT1_DEFAULT 0x00201102UL
#define FBI_INIT2_DEFAULT 0x80000040UL
#define FBI_INIT3_DEFAULT 0x001E4000UL
#define FBI_INIT4_DEFAULT 0x00000001UL
/* trexInit0: texture memory refresh on, every 0x20, 9-bit pages, the second
RAS on bit 18; trexInit1: FIFO stall levels 8 and 8, the TMU-to-FBI clock
delay */
#define TREX_INIT0_DEFAULT (0x1UL | (0x20UL << 1) | (0x1UL << 10) | (0x1UL << 12) | 0x4000UL)
#define TREX_INIT1_DEFAULT ((0x8UL << 2) | (0x8UL << 7))
#define TF_DELAY_SHIFT 12
#define TF_DELAY (0xFUL << TF_DELAY_SHIFT)

/* the DAC's registers (dacData: data, the register << 8, 0x800 a read;
reads come back through fbiInit2 while initEnable remaps it) */
#define DAC_WRITE_ADDRESS 0
#define DAC_READ_MASK 2
#define DAC_INDEX_ADDRESS DAC_WRITE_ADDRESS
#define DAC_INDEX_DATA DAC_READ_MASK
/* the AT&T and TI DACs' indexed registers */
#define INDEX_CR0 0x1
#define INDEX_MANUFACTURER 0x2
#define INDEX_DEVICE 0x3
#define INDEX_CLOCK_CHOICE 0x6
#define INDEX_VIDEO_CLOCK 0x4C
#define INDEX_GRAPHICS_CLOCK 0x6C
#define CR0_INDEXED 0x1UL
#define CR0_8_BIT 0x2UL
#define CR0_COLOR_MODE 0xF0UL
#define CR0_24_BIT 0x50UL
/* the ICS5342's */
#define ICS_PLL_WRITE 4
#define ICS_PLL_DATA 5
#define ICS_COMMAND 6
#define ICS_PLL_READ 7
#define ICS_VIDEO_CLOCK_0 0x0
#define ICS_VIDEO_CLOCK_1 0x1
#define ICS_VIDEO_CLOCK_7 0x7
#define ICS_GRAPHICS_CLOCK_0 0xA
#define ICS_GRAPHICS_CLOCK_1 0xB
#define ICS_PLL_CONTROL 0xE
#define ICS_24_BIT 0x70UL

enum { DAC_NONE, DAC_ATT, DAC_ICS, DAC_TI };

static struct
{
	struct dos_pci_device device;
	volatile unsigned long *base;
	int open;
	/* fbiInit0's VGA pass-through bit when the Voodoo shows its picture (the
	boards differ: sst1init reads it from fbiInit3 at power on), and whether
	that is known */
	unsigned long show;
	int show_known;
	int dac;
	/* fbiInit1 and 2 kept while the DAC is reached (dac_open) */
	unsigned long saved1, saved2;
} card;

static void write_register(unsigned offset, unsigned long value)
{
	card.base[offset >> 2] = value;
}

static unsigned long read_register(unsigned offset)
{
	return card.base[offset >> 2];
}

static void set_bits(unsigned offset, unsigned long clear, unsigned long set)
{
	write_register(offset, (read_register(offset) & ~clear) | set);
}

/* status register reads: a wait the card's own clock times */
static void stall(long reads)
{
	for (long i = 0; i < reads; i++)
		(void)read_register(STATUS);
}

/* the FBI idle: not busy six reads running (bounded: a card that never says
so is not waited on for ever) */
static void idle(void)
{
	int quiet = 0;

	for (long i = 0; i < 1000000 && quiet < 6; i++)
		quiet = (read_register(STATUS) & STATUS_FBI_BUSY) ? 0 : quiet + 1;
}

static void init_enable(unsigned long bits)
{
	dos_pci_write(&card.device, PCI_INIT_ENABLE, bits);
}

/* ---------- the DAC */

static void dac_write(int address, unsigned long data)
{
	write_register(DAC_DATA, (data & 0xFF) | ((unsigned long)address << 8));
	idle();
}

static unsigned long dac_read(int address)
{
	write_register(DAC_DATA, ((unsigned long)address << 8) | 0x800UL);
	idle();
	return read_register(FBI_INIT2) & 0xFF;
}

/* the DAC reachable: the video unit in reset and the DRAM refresh off (no
one else on the memory bus), its reads through fbiInit2, nothing through
the PCI FIFO; and back */
static void dac_open(void)
{
	idle();
	card.saved1 = read_register(FBI_INIT1);
	card.saved2 = read_register(FBI_INIT2);
	write_register(FBI_INIT1, card.saved1 | VIDEO_RESET);
	write_register(FBI_INIT2, card.saved2 & ~DRAM_REFRESH);
	idle();
	init_enable(INIT_WRITES | INIT_DAC_READS);
	idle();
}

static void dac_close(void)
{
	init_enable(INIT_WRITES | INIT_PCI_FIFO);
	idle();
	write_register(FBI_INIT1, card.saved1);
	write_register(FBI_INIT2, card.saved2);
	idle();
}

/* an AT&T or TI DAC's indexed registers on (its command register 0 behind
the read mask register: four reads of it, then the write); its maker's
code (0x84 AT&T, 0x97 TI), 0 if it does not answer */
static unsigned long dac_indexed_on(void)
{
	for (int tries = 0; tries < 100; tries++)
	{
		unsigned long cr0, maker, device;

		idle();
		dac_write(DAC_WRITE_ADDRESS, 0);
		for (int k = 0; k < 4; k++)
			dac_read(DAC_READ_MASK);
		cr0 = dac_read(DAC_READ_MASK);
		dac_write(DAC_WRITE_ADDRESS, 0);
		for (int k = 0; k < 4; k++)
			dac_read(DAC_READ_MASK);
		dac_write(DAC_READ_MASK, (cr0 & 0xF0) | CR0_INDEXED | CR0_8_BIT);
		dac_write(DAC_INDEX_ADDRESS, INDEX_MANUFACTURER);
		maker = dac_read(DAC_INDEX_DATA);
		dac_write(DAC_INDEX_ADDRESS, INDEX_DEVICE);
		device = dac_read(DAC_INDEX_DATA);
		if ((maker == 0x84 || maker == 0x97) && device == 0x09)
			return maker;
	}
	return 0;
}

static void dac_indexed_off(void)
{
	dac_write(DAC_INDEX_ADDRESS, INDEX_CR0);
	dac_write(DAC_INDEX_DATA, dac_read(DAC_INDEX_DATA) & ~CR0_INDEXED);
}

static void dac_indexed_write(int index, unsigned long data)
{
	dac_write(DAC_INDEX_ADDRESS, (unsigned long)index);
	dac_write(DAC_INDEX_DATA, data);
}

static unsigned long dac_indexed_read(int index)
{
	dac_write(DAC_INDEX_ADDRESS, (unsigned long)index);
	return dac_read(DAC_INDEX_DATA);
}

/* which DAC: an ICS5342 by its clock synthesizer's power-on values, else
an AT&T or TI one by the maker in its indexed registers */
static int dac_detect(void)
{
	int type = DAC_NONE;

	dac_open();
	for (int tries = 0; tries < 100 && type == DAC_NONE; tries++)
	{
		unsigned long graphics1, video1, video7;

		idle();
		dac_write(ICS_PLL_READ, ICS_GRAPHICS_CLOCK_1);
		graphics1 = dac_read(ICS_PLL_DATA);
		dac_read(ICS_PLL_DATA);
		dac_write(ICS_PLL_READ, ICS_VIDEO_CLOCK_1);
		video1 = dac_read(ICS_PLL_DATA);
		dac_read(ICS_PLL_DATA);
		dac_write(ICS_PLL_READ, ICS_VIDEO_CLOCK_7);
		video7 = dac_read(ICS_PLL_DATA);
		dac_read(ICS_PLL_DATA);
		if (graphics1 == 0x79 && video1 == 0x55 && video7 == 0x71)
			type = DAC_ICS;
	}
	if (type == DAC_NONE)
	{
		unsigned long maker = dac_indexed_on();

		if (maker)
		{
			type = maker == 0x84 ? DAC_ATT : DAC_TI;
			dac_indexed_off();
		}
	}
	dac_close();
	return type;
}

/* ---------- clocks */

struct clock
{
	unsigned long m, n, p, l, ib;
};

/* a clock synthesizer's settings for freq MHz: 14.318 MHz times (m + 2) /
(n + 2), its VCO kept between 120 and 240 MHz and divided by 2^p; l and ib
its loop filter's, by the frequency (sst1init's figures) */
static int clock_settings(float freq, struct clock *clock)
{
	float divide = 0.0f, ratio, best = 9999.0f;
	unsigned long p = 4;

	for (unsigned long k = 0; k < 4; k++)
	{
		float vco = freq * (float)(1UL << k);

		if (vco >= 120.0f && vco <= 240.0f)
		{
			divide = (float)(1UL << k);
			p = k;
		}
	}
	if (p > 3)
		return FALSE;
	ratio = freq * divide * 0.06984216f;
	clock->n = 0;
	for (int n2 = 3; n2 < 32; n2++)
	{
		int m2 = (int)((float)n2 * ratio + 0.5f);
		float error = (float)m2 / (float)n2 - ratio;

		if (error < 0.0f)
			error = -error;
		if (error < best && m2 - 2 < 127)
		{
			best = error;
			clock->n = (unsigned long)(n2 - 2);
			clock->m = (unsigned long)(m2 - 2);
		}
	}
	if (!clock->n)
		return FALSE;
	clock->p = p;
	clock->l = freq < 37.0f ? 0xA : freq < 45.0f ? 0xC : freq < 58.0f ? 0x8 : 0xA;
	clock->ib = freq < 37.0f ? 0x6 : freq < 45.0f ? 0x4 : freq < 58.0f ? 0x4 : freq < 66.0f ? 0x6 : 0x8;
	return TRUE;
}

/* the graphics (memory) clock: the FBI and its FIFO held in reset while it
changes, time given for it to settle */
static void set_graphics_clock(float freq)
{
	struct clock clock;

	if (card.dac == DAC_NONE || !clock_settings(freq, &clock))
		return;
	set_bits(FBI_INIT0, 0, GRAPHICS_RESET | PCI_FIFO_RESET);
	idle();
	dac_open();
	if (card.dac == DAC_ICS)
	{
		unsigned long control;

		dac_write(ICS_PLL_READ, ICS_PLL_CONTROL);
		control = dac_read(ICS_PLL_DATA);
		dac_write(ICS_PLL_WRITE, ICS_GRAPHICS_CLOCK_0);
		dac_write(ICS_PLL_DATA, clock.m);
		dac_write(ICS_PLL_DATA, (clock.p << 5) | clock.n);
		dac_write(ICS_PLL_WRITE, ICS_PLL_CONTROL);
		dac_write(ICS_PLL_DATA, control & ~0x10UL);
	}
	else if (dac_indexed_on())
	{
		dac_indexed_write(INDEX_GRAPHICS_CLOCK, clock.m);
		dac_indexed_write(INDEX_GRAPHICS_CLOCK + 1, (clock.p << 6) | clock.n);
		dac_indexed_write(INDEX_GRAPHICS_CLOCK + 2, (clock.l << 4) | clock.ib);
		dac_indexed_write(INDEX_CLOCK_CHOICE, dac_indexed_read(INDEX_CLOCK_CHOICE) | 0x3UL | 0x8UL);
		dac_indexed_off();
	}
	idle();
	dac_close();
	stall(200000);
	set_bits(FBI_INIT0, PCI_FIFO_RESET, 0);
	idle();
	set_bits(FBI_INIT0, GRAPHICS_RESET, 0);
	idle();
	/* (sst1init's: a write that makes the texture units take the first
	downloads after a clock change) */
	write_register(0xF00000, 0xDEADBEEFUL);
}

static void set_video_clock(float freq)
{
	struct clock clock;

	if (card.dac == DAC_NONE || !clock_settings(freq, &clock))
		return;
	dac_open();
	if (card.dac == DAC_ICS)
	{
		unsigned long control;

		dac_write(ICS_PLL_READ, ICS_PLL_CONTROL);
		control = dac_read(ICS_PLL_DATA);
		dac_write(ICS_PLL_WRITE, ICS_VIDEO_CLOCK_0);
		dac_write(ICS_PLL_DATA, clock.m);
		dac_write(ICS_PLL_DATA, (clock.p << 5) | clock.n);
		dac_write(ICS_PLL_WRITE, ICS_PLL_CONTROL);
		dac_write(ICS_PLL_DATA, (control & ~0x7UL) | 0x20UL);
	}
	else if (dac_indexed_on())
	{
		dac_indexed_write(INDEX_VIDEO_CLOCK, clock.m);
		dac_indexed_write(INDEX_VIDEO_CLOCK + 1, (clock.p << 6) | clock.n);
		dac_indexed_write(INDEX_VIDEO_CLOCK + 2, (clock.l << 4) | clock.ib);
		dac_indexed_write(INDEX_CLOCK_CHOICE, dac_indexed_read(INDEX_CLOCK_CHOICE) | 0x30UL | 0x80UL);
		dac_indexed_off();
	}
	idle();
	dac_close();
}

/* the DAC taking 24-bit pixels */
static void set_dac_24_bit(void)
{
	if (card.dac == DAC_NONE)
		return;
	dac_open();
	if (card.dac == DAC_ICS)
		dac_write(ICS_COMMAND, ICS_24_BIT);
	else if (dac_indexed_on())
	{
		dac_indexed_write(INDEX_CR0, (dac_indexed_read(INDEX_CR0) & ~CR0_COLOR_MODE) | CR0_24_BIT | CR0_8_BIT);
		dac_indexed_off();
	}
	idle();
	dac_close();
}

/* ---------- bringing it up */

static const char *dac_name(int dac)
{
	switch (dac)
	{
	case DAC_ATT: return "AT&T ATT20C409";
	case DAC_ICS: return "ICS5342";
	case DAC_TI: return "TI TVP3409";
	default: return "not found";
	}
}

/* sst1InitRegisters then sst1InitVideo for 640x480 at 60 Hz (24-bit video,
the 50.35 MHz clock), a 2 MB frame buffer: buffers of 150 pages, the memory
FIFO in the pages after the third */
static void bring_up(int revision, float graphics_clock)
{
	/* the clock delays between the FBI and the texture units: an FBI of
	revision 1 goes with texture units of 0 (0.6 micron chips), 2 with 1
	(0.5 micron), sst1init's figures for each */
	unsigned long ft_delay = revision >= 2 ? 0x8 : 0xA, tf_delay;
	unsigned long init3 = (FBI_INIT3_DEFAULT & ~(FT_DELAY | TF_FIFO)) | (ft_delay << FT_DELAY_SHIFT) |
		(0x8UL << TF_FIFO_SHIFT);

	dos_pci_write(&card.device, PCI_VIDEO_CLOCK_OFF, 0);
	init_enable(INIT_WRITES);
	dos_pci_write(&card.device, PCI_BUS_SNOOP_0, 0);
	dos_pci_write(&card.device, PCI_BUS_SNOOP_1, 0);
	stall(3);
	write_register(FBI_INIT3, init3);
	stall(3);
	/* (the video unit reset before the graphics unit: else it can hang
	waiting on it) */
	set_bits(FBI_INIT1, 0, VIDEO_RESET);
	stall(3);
	set_bits(FBI_INIT0, 0, GRAPHICS_RESET | PCI_FIFO_RESET);
	idle();
	set_bits(FBI_INIT0, PCI_FIFO_RESET, 0);
	idle();
	set_bits(FBI_INIT0, GRAPHICS_RESET, 0);
	idle();
	write_register(FBI_INIT0, FBI_INIT0_DEFAULT);
	write_register(FBI_INIT1, FBI_INIT1_DEFAULT);
	write_register(FBI_INIT2, FBI_INIT2_DEFAULT);
	write_register(FBI_INIT3, init3);
	write_register(FBI_INIT4, FBI_INIT4_DEFAULT);
	idle();
	init_enable(INIT_WRITES | INIT_PCI_FIFO);
	card.dac = dac_detect();
	for (int tmu = 0; tmu < 3; tmu++)
	{
		write_register(TMU(tmu) + TREX_INIT0, TREX_INIT0_DEFAULT);
		idle();
		write_register(TMU(tmu) + TREX_INIT1, TREX_INIT1_DEFAULT | (0xFUL << TF_DELAY_SHIFT));
		idle();
	}
	/* (a slow clock first, while the rest is set) */
	set_graphics_clock(16.0f);
	set_bits(FBI_INIT1, PCI_WRITE_WAIT, LFB_READS);
	set_bits(FBI_INIT2, SWAP_ALGORITHM, 0);
	set_bits(FBI_INIT4, 0, LFB_READ_AHEAD);
	set_bits(FBI_INIT2, DRAM_REFRESH_COUNT, 0x30UL << DRAM_REFRESH_COUNT_SHIFT);
	idle();
	set_bits(FBI_INIT2, 0, DRAM_REFRESH);
	idle();
	set_bits(FBI_INIT2, 0, FAST_RAS_READ | DRAM_READ_AHEAD | FAST_READ_AHEAD_WRITE);
	idle();
	/* (texture downloads and frame buffer writes in turn with the rest:
	through the memory FIFO too) */
	set_bits(FBI_INIT0, 0, LFB_THROUGH_MEMORY_FIFO | TEXTURES_THROUGH_MEMORY_FIFO);
	idle();

	/* the video: in reset while it is set, its clock on */
	set_bits(FBI_INIT1, 0, VIDEO_RESET);
	dos_pci_write(&card.device, PCI_VIDEO_CLOCK_ON, 0);
	/* (640x480 at 60 Hz as sst1init writes it: sync on and off counts,
	back porches, the width less one) */
	write_register(H_SYNC, (704UL << 16) | 96);
	write_register(V_SYNC, (523UL << 16) | 2);
	write_register(BACK_PORCH, (25UL << 16) | 38);
	write_register(VIDEO_DIMENSIONS, ((unsigned long)HEIGHT << 16) | (WIDTH - 1));
	set_bits(FBI_INIT2, BUFFER_OFFSET, 150UL << BUFFER_OFFSET_SHIFT);
	set_bits(FBI_INIT1, TILES, 10UL << TILES_SHIFT);
	set_bits(FBI_INIT3, VIDEO_FIFO, (graphics_clock < 45.0f ? 19UL : 23UL) << VIDEO_FIFO_SHIFT);
	idle();
	set_bits(FBI_INIT3, Y_ORIGIN, (unsigned long)(HEIGHT - 1) << Y_ORIGIN_SHIFT);
	idle();
	/* the memory FIFO: rows 450 to 511, behind the three buffers */
	set_bits(FBI_INIT4, MEMORY_FIFO_BASE | MEMORY_FIFO_ROLL | MEMORY_FIFO_LOW, (0x1FFUL << MEMORY_FIFO_ROLL_SHIFT) |
		(450UL << MEMORY_FIFO_BASE_SHIFT) | (23UL << MEMORY_FIFO_LOW_SHIFT));
	idle();
	set_bits(FBI_INIT0, MEMORY_FIFO_ON | MEMORY_FIFO_HIGH | PCI_FIFO_LOW | MEMORY_FIFO_BURST, (0x430UL <<
		MEMORY_FIFO_HIGH_SHIFT) | (13UL << PCI_FIFO_LOW_SHIFT) | (54UL << MEMORY_FIFO_BURST_SHIFT) | MEMORY_FIFO_ON);
	idle();
	/* 24-bit output: the DAC's data, blank, syncs and clock driven, the
	video clock halved, the filter that takes out the 16-bit dither */
	set_bits(FBI_INIT1, VIDEO_BITS, VIDEO_DRIVE | VIDEO_CLOCK_HALF | VIDEO_24_BIT |
		((revision >= 2 ? 2UL : 0UL) << VIDEO_OUT_DELAY_SHIFT));
	idle();
	set_bits(FBI_INIT1, 0, VIDEO_FILTER);
	write_register(VIDEO_FILTER_THRESHOLD, 0x080408UL);
	if (revision >= 2)
		ft_delay = tf_delay = graphics_clock >= 55.0f ? 0x4 : graphics_clock >= 50.0f ? 0x5 :
			graphics_clock >= 48.0f ? 0x6 : graphics_clock >= 45.0f ? 0x7 : graphics_clock >= 43.0f ? 0x8 : 0x9;
	else
	{
		ft_delay = 0xA;
		tf_delay = 0xF;
	}
	set_bits(FBI_INIT3, FT_DELAY, ft_delay << FT_DELAY_SHIFT);
	idle();
	for (int tmu = 0; tmu < 3; tmu++)
	{
		/* (no idle wait: the changing delay can put data into the TMU-to-FBI
		FIFO) */
		write_register(TMU(tmu) + TREX_INIT1, TREX_INIT1_DEFAULT | (tf_delay << TF_DELAY_SHIFT));
		stall(100);
	}
	set_graphics_clock(graphics_clock);
	set_dac_24_bit();
	set_video_clock(50.35f);
	stall(200000);
	set_bits(FBI_INIT1, VIDEO_RESET, 0);
	idle();
	set_bits(FBI_INIT2, 0, DRAM_REFRESH);
	idle();
	/* a straight gamma table (every eighth entry, the card interpolates;
	the 33rd the top) */
	for (unsigned long x = 0; x < 32; x++)
		write_register(CLUT_DATA, (x << 24) | ((x << 3) * 0x010101UL));
	write_register(CLUT_DATA, (32UL << 24) | 0xFFFFFFUL);
	idle();
}

/* the memory layout and video registers alone (voodoo_init = 0: as this
driver first did, what DOSBox-X needs) */
static void layout_only(void)
{
	write_register(FBI_INIT0, GRAPHICS_RESET | PCI_FIFO_RESET);
	write_register(FBI_INIT0, 0);
	write_register(FBI_INIT1, (10UL << TILES_SHIFT) | LFB_READS);
	write_register(FBI_INIT2, 150UL << BUFFER_OFFSET_SHIFT);
	write_register(FBI_INIT3, (unsigned long)(HEIGHT - 1) << Y_ORIGIN_SHIFT);
	write_register(FBI_INIT4, 0);
	write_register(H_SYNC, (704UL << 16) | 96);
	write_register(V_SYNC, (523UL << 16) | 2);
	write_register(BACK_PORCH, (25UL << 16) | 38);
	write_register(VIDEO_DIMENSIONS, ((unsigned long)HEIGHT << 16) | (WIDTH - 1));
	dos_pci_write(&card.device, PCI_VIDEO_CLOCK_ON, 0);
}

static void voodoo_close(void)
{
	if (!card.open)
		return;
	card.open = FALSE;
	/* (the VGA card's picture back: the pass-through to it, the video
	blanked, the clock off) */
	write_register(FBI_INIT0, (read_register(FBI_INIT0) & ~VGA_PASS) | (card.show ^ VGA_PASS));
	set_bits(FBI_INIT1, 0, VIDEO_BLANK);
	dos_pci_write(&card.device, PCI_VIDEO_CLOCK_OFF, 0);
}

int dos_voodoo_open(void)
{
	unsigned long id, bar, physical, linear, command;
	int revision, full = dos_setting_integer("voodoo_init", 1) != 0;
	long clock_setting = dos_setting_integer("voodoo_clock", 50);
	struct r_voodoo_card handed;
	char line[200];

	if (!dos_pci_present())
	{
		system_log("voodoo: no PCI bus");
		return FALSE;
	}
	if (!dos_pci_find(0x121A, 0x0001, &card.device))
	{
		struct dos_pci_device other;

		if (dos_pci_find(0x121A, 0x0002, &other))
			system_log("voodoo: a Voodoo 2 is there, which this version does not drive yet");
		else
			system_log("voodoo: no 3dfx Voodoo Graphics card on the PCI bus");
		return FALSE;
	}
	id = dos_pci_read(&card.device, 0);
	revision = (int)(dos_pci_read(&card.device, 0x08) & 0xFF);
	bar = dos_pci_read(&card.device, 0x10);
	physical = bar & ~0xFUL;
	/* (the address written back as read: the card's window where it says) */
	dos_pci_write(&card.device, 0x10, bar);
	command = dos_pci_read(&card.device, 0x04);
	dos_pci_write(&card.device, 0x04, command | 0x2);
	linear = dos_dpmi_map_physical(physical, 0x1000000);
	if (!linear)
	{
		snprintf(line, sizeof(line), "voodoo: its memory at %08lX cannot be mapped", physical);
		system_log(line);
		return FALSE;
	}
	card.base = dos_pointer(linear);
	if (!card.show_known)
	{
		/* (boards whose pass-through is on at power on show the Voodoo with
		the bit clear, sst1init's rule) */
		card.show = (read_register(FBI_INIT3) & VGA_PASS_POWER_ON) && revision > 0 ? 0 : VGA_PASS;
		card.show_known = TRUE;
	}
	/* (initEnable: the fbiInit registers writable, the PCI FIFO on) */
	init_enable(INIT_WRITES | INIT_PCI_FIFO);
	card.dac = DAC_NONE;
	if (full)
		bring_up(revision, (float)(clock_setting < 16 ? 16 : clock_setting > 60 ? 60 : clock_setting));
	else
		layout_only();
	/* (the second texture unit, if there is one, off: LOD min 8) */
	write_register(TMU(1) + T_LOD, 0x820);
	write_register(CLIP_LEFT_RIGHT, WIDTH);
	write_register(CLIP_LOW_Y_HIGH_Y, HEIGHT);
	card.open = TRUE;
	dos_at_exit(voodoo_close);
	handed.base = card.base;
	handed.width = WIDTH;
	handed.height = HEIGHT;
	/* (2 MB is sure; r_voodoo_use finds 4 where there are) */
	handed.texture_memory = 2UL << 20;
	handed.clip_extra = 0;
	r_voodoo_use(&handed);
	/* the card's picture on (the buffers cleared first): the monitor goes
	to it */
	write_register(FBI_INIT0, (read_register(FBI_INIT0) & ~VGA_PASS) | card.show);
	set_bits(FBI_INIT1, VIDEO_BLANK, 0);
	snprintf(line, sizeof(line), "voodoo: 3dfx %04lX:%04lX revision %d at bus %d slot %d, memory at %08lX: %dx%d, "
		"%lu MB of texture memory%s; %s, DAC %s", id & 0xFFFF, id >> 16, revision, card.device.bus, card.device.slot,
		physical, WIDTH, HEIGHT, handed.texture_memory >> 20,
		handed.clip_extra ? ", clip edges a column wider (as DOSBox-X draws)" : "",
		full ? "brought up as sst1init does" : "memory layout only (voodoo_init 0)", dac_name(card.dac));
	system_log(line);
	return TRUE;
}

int dos_voodoo_active(void)
{
	return card.open;
}

void dos_voodoo_present(void)
{
	if (card.open)
		r_voodoo_swap();
}

void dos_voodoo_close(void)
{
	voodoo_close();
}
