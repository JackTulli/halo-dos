/*
DOS_AUDIO.C

DOS's sound device (halo_system.h): a Sound Blaster, found by the setting
sound_blaster or the BLASTER variable its setup leaves (A220 I5 D1 H5 T6:
the port, the interrupt, the 8-bit and 16-bit DMA channels); without
either, the usual ports are tried, and a 16 tells its interrupt and DMA
channels in its mixer's registers (an older card is taken to be at
interrupt 7, DMA 1, DOSBox's). A 16 (DSP version 4 or later) plays 16-bit
stereo at the asked rate over its 16-bit DMA channel; the earlier ones
(2.0 to 3.x: SB 2.0, Pro) 8-bit mono at up to 22 kHz over the 8-bit one,
their time constant's rate passed back to the mixer.

The card plays a ring of RING_FRAMES in conventional memory (a buffer that
crosses no DMA page) round and round (auto-initialised DMA), interrupting
at each half; dos_audio_isr.s only acknowledges it. The game's thread fills
the ring (system_audio_update, from the mixer: every frame and in the
game's waits) lead frames ahead of where the DMA controller has read, and
writes silence over the rest, so a stall longer than the lead plays
nothing rather than the old sound again. Where it has read is the DMA
channel's count, followed across laps; a gap of most of a ring between
updates may hide a lap, and then the writing starts again at the play
position.

The setting sound_record=<file> writes the mix to a .WAV file instead
(sys_audio_record.c).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "halo_system.h"
#include "dos_dpmi.h"

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

int sys_audio_record_open(unsigned long rate);
int sys_audio_recording(void);
void sys_audio_record_update(void (*fill)(short *samples, unsigned long frames));
void sys_audio_record_close(void);
long dos_setting_integer(const char *name, long default_value);

#define RING_FRAMES 8192
/* frames the writing keeps from the play position */
#define RING_GUARD 512
#define FILL_FRAMES 512

void dos_audio_isr(void);
extern char dos_audio_isr_end;
extern unsigned short dos_audio_data_selector;
extern unsigned long dos_audio_ack_port, dos_audio_slave;
extern volatile unsigned long dos_audio_interrupts;
extern char dos_audio_data_start, dos_audio_data_end;

static struct
{
	int open;
	unsigned short base;
	int irq, dma8, dma16;
	/* 16-bit stereo (a 16), else 8-bit mono */
	int sixteen;
	int dma;
	unsigned short selector;
	unsigned char *ring;
	unsigned long ring_physical, ring_bytes, frame_bytes;
	/* frames played and written since the start, and where the DMA was */
	unsigned long long played, written;
	unsigned long last_position;
	unsigned long long last_update;
	unsigned long rate;
	int vector;
	unsigned short old_selector;
	unsigned long old_offset;
	unsigned char old_master_mask, old_slave_mask;
	short mix[FILL_FRAMES * 2];
} sb;

/* ---------- the card's DSP */

static int dsp_write(unsigned char value)
{
	for (long i = 0; i < 65536; i++)
	{
		if (!(dos_inb(sb.base + 0xC) & 0x80))
		{
			dos_outb(sb.base + 0xC, value);
			return TRUE;
		}
	}
	return FALSE;
}

static int dsp_read(void)
{
	for (long i = 0; i < 65536; i++)
	{
		if (dos_inb(sb.base + 0xE) & 0x80)
			return dos_inb(sb.base + 0xA);
	}
	return -1;
}

static int dsp_reset(void)
{
	if (dos_inb(sb.base + 0xE) == 0xFF && dos_inb(sb.base + 0xA) == 0xFF && dos_inb(sb.base + 0xC) == 0xFF)
	{
		/* (nothing there: an empty port reads all ones) */
		dos_outb(sb.base + 6, 1);
		dos_outb(sb.base + 6, 0);
		if (dos_inb(sb.base + 0xE) == 0xFF && dos_inb(sb.base + 0xA) == 0xFF)
			return FALSE;
	}
	dos_outb(sb.base + 6, 1);
	/* (at least 3 microseconds: a port read is about one) */
	for (int i = 0; i < 16; i++)
		dos_inb(sb.base + 6);
	dos_outb(sb.base + 6, 0);
	for (int tries = 0; tries < 64; tries++)
	{
		if (dsp_read() == 0xAA)
			return TRUE;
	}
	return FALSE;
}

/* ---------- the DMA controller */

static const unsigned short dma_page_ports[8] = { 0x87, 0x83, 0x81, 0x82, 0x8F, 0x8B, 0x89, 0x8A };

static void dma_start(void)
{
	int channel = sb.dma, low = channel & 3;
	unsigned long units = sb.sixteen ? sb.ring_bytes / 2 : sb.ring_bytes;
	unsigned long address = sb.sixteen ? (sb.ring_physical >> 1) & 0xFFFF : sb.ring_physical & 0xFFFF;
	unsigned short mask = channel >= 4 ? 0xD4 : 0x0A, mode = channel >= 4 ? 0xD6 : 0x0B;
	unsigned short flip_flop = channel >= 4 ? 0xD8 : 0x0C;
	unsigned short address_port = channel >= 4 ? 0xC0 + 4 * low : 2 * low;
	unsigned short count_port = channel >= 4 ? 0xC2 + 4 * low : 2 * low + 1;

	dos_outb(mask, (unsigned char)(4 | low));
	dos_outb(flip_flop, 0);
	/* (single transfers, auto-initialised, memory to the card) */
	dos_outb(mode, (unsigned char)(0x58 | low));
	dos_outb(address_port, (unsigned char)address);
	dos_outb(address_port, (unsigned char)(address >> 8));
	dos_outb(dma_page_ports[channel], (unsigned char)((sb.ring_physical >> 16) & (sb.sixteen ? 0xFE : 0xFF)));
	dos_outb(flip_flop, 0);
	dos_outb(count_port, (unsigned char)(units - 1));
	dos_outb(count_port, (unsigned char)((units - 1) >> 8));
	dos_outb(mask, (unsigned char)low);
}

static void dma_stop(void)
{
	dos_outb(sb.dma >= 4 ? 0xD4 : 0x0A, (unsigned char)(4 | (sb.dma & 3)));
}

/* where in the ring the DMA has read to, in frames */
static unsigned long dma_position(void)
{
	int low = sb.dma & 3;
	unsigned short flip_flop = sb.dma >= 4 ? 0xD8 : 0x0C;
	unsigned short count_port = sb.dma >= 4 ? 0xC2 + 4 * low : 2 * low + 1;
	unsigned long units = sb.sixteen ? sb.ring_bytes / 2 : sb.ring_bytes;
	unsigned long counts[2];

	/* (twice, the count moving between its two bytes' reads: the later
	of two that agree within a few) */
	for (int tries = 0; tries < 4; tries++)
	{
		for (int k = 0; k < 2; k++)
		{
			unsigned long flags = dos_interrupts_disable();
			unsigned long lowbyte, highbyte;

			dos_outb(flip_flop, 0);
			lowbyte = dos_inb(count_port);
			highbyte = dos_inb(count_port);
			dos_interrupts_restore(flags);
			counts[k] = (highbyte << 8) | lowbyte;
		}
		if ((counts[0] >= counts[1] ? counts[0] - counts[1] : counts[1] - counts[0]) < 64)
			break;
	}
	{
		unsigned long remaining = (counts[1] + 1) & 0xFFFF, consumed;

		if (remaining > units)
			remaining = units;
		consumed = units - remaining;
		return (sb.sixteen ? consumed / 2 : consumed) % RING_FRAMES;
	}
}

/* ---------- the ring */

static void ring_silence(unsigned long at, unsigned long frames)
{
	while (frames)
	{
		unsigned long count = RING_FRAMES - at < frames ? RING_FRAMES - at : frames;

		memset(sb.ring + at * sb.frame_bytes, sb.sixteen ? 0 : 0x80, count * sb.frame_bytes);
		frames -= count;
		at = (at + count) % RING_FRAMES;
	}
}

static void ring_put(unsigned long at, const short *samples, unsigned long frames)
{
	if (sb.sixteen)
	{
		memcpy(sb.ring + at * 4, samples, frames * 4);
		return;
	}
	for (unsigned long i = 0; i < frames; i++)
	{
		long mono = ((long)samples[2 * i] + (long)samples[2 * i + 1]) >> 1;

		sb.ring[at + i] = (unsigned char)((mono >> 8) + 128);
	}
}

/* ---------- opening and closing */

static void parse_blaster(const char *text)
{
	sb.base = 0x220;
	sb.irq = 5;
	sb.dma8 = 1;
	sb.dma16 = -1;
	while (text && *text)
	{
		char kind = *text++;

		if (kind == 'A' || kind == 'a')
			sb.base = (unsigned short)strtoul(text, NULL, 16);
		else if (kind == 'I' || kind == 'i')
			sb.irq = (int)strtol(text, NULL, 10);
		else if (kind == 'D' || kind == 'd')
			sb.dma8 = (int)strtol(text, NULL, 10);
		else if (kind == 'H' || kind == 'h')
			sb.dma16 = (int)strtol(text, NULL, 10);
		while (*text && *text != ' ' && *text != '\t')
			text++;
		while (*text == ' ' || *text == '\t')
			text++;
	}
	if (sb.irq == 2)
		sb.irq = 9;
}

/* a 16's interrupt and DMA channels, from its mixer's registers 80h and
81h: FALSE if they say nothing */
static int sb16_resources(void)
{
	int irq, dma8 = -1, dma16 = -1;
	unsigned char irqs, dmas;

	dos_outb(sb.base + 4, 0x80);
	irqs = dos_inb(sb.base + 5);
	dos_outb(sb.base + 4, 0x81);
	dmas = dos_inb(sb.base + 5);
	irq = irqs & 1 ? 9 : irqs & 2 ? 5 : irqs & 4 ? 7 : irqs & 8 ? 10 : -1;
	dma8 = dmas & 2 ? 1 : dmas & 1 ? 0 : dmas & 8 ? 3 : -1;
	dma16 = dmas & 0x20 ? 5 : dmas & 0x40 ? 6 : dmas & 0x80 ? 7 : -1;
	if (irq < 0 || (dma8 < 0 && dma16 < 0))
		return FALSE;
	sb.irq = irq;
	sb.dma8 = dma8;
	sb.dma16 = dma16;
	return TRUE;
}

/* conventional memory for the ring that no DMA page boundary (64 KB, 128
KB for 16-bit transfers) crosses: twice its size, and the half of it on one
side of the boundary that holds it */
static int ring_allocate(void)
{
	unsigned long page = sb.sixteen ? 0x20000 : 0x10000, start, block = sb.ring_bytes * 2;
	unsigned short segment;

	if (!dos_dos_allocate((unsigned short)((block + 15) / 16), &segment, &sb.selector))
		return FALSE;
	start = (unsigned long)segment << 4;
	if ((start / page) != ((start + sb.ring_bytes - 1) / page))
		start = (start + page - 1) / page * page;
	sb.ring_physical = start;
	sb.ring = dos_pointer(start);
	return TRUE;
}

static void interrupt_install(void)
{
	sb.vector = sb.irq < 8 ? 8 + sb.irq : 0x70 + sb.irq - 8;
	dos_audio_data_selector = dos_data_selector();
	dos_audio_ack_port = sb.base + (sb.sixteen ? 0xF : 0xE);
	dos_audio_slave = sb.irq >= 8;
	dos_dpmi_lock((const void *)dos_audio_isr, (unsigned long)(&dos_audio_isr_end - (char *)dos_audio_isr));
	dos_dpmi_lock(&dos_audio_data_start, (unsigned long)(&dos_audio_data_end - &dos_audio_data_start));
	dos_get_interrupt_vector(sb.vector, &sb.old_selector, &sb.old_offset);
	__asm__ __volatile__("cli");
	dos_set_interrupt_vector(sb.vector, dos_code_selector(), (unsigned long)dos_audio_isr);
	sb.old_master_mask = dos_inb(0x21);
	sb.old_slave_mask = dos_inb(0xA1);
	if (sb.irq < 8)
	{
		dos_outb(0x21, (unsigned char)(sb.old_master_mask & ~(1 << sb.irq)));
	}
	else
	{
		dos_outb(0xA1, (unsigned char)(sb.old_slave_mask & ~(1 << (sb.irq - 8))));
		dos_outb(0x21, (unsigned char)(sb.old_master_mask & ~(1 << 2)));
	}
	__asm__ __volatile__("sti");
}

static void sb_close(void)
{
	if (!sb.open)
		return;
	sb.open = FALSE;
	/* (stop the output, leave auto-initialised mode, then the DMA) */
	dsp_write(sb.sixteen ? 0xD5 : 0xD0);
	dsp_write(sb.sixteen ? 0xD9 : 0xDA);
	dsp_write(0xD3);
	dma_stop();
	dsp_reset();
	__asm__ __volatile__("cli");
	if (sb.irq < 8)
		dos_outb(0x21, (unsigned char)((dos_inb(0x21) & ~(1 << sb.irq)) | (sb.old_master_mask & (1 << sb.irq))));
	else
		dos_outb(0xA1, (unsigned char)((dos_inb(0xA1) & ~(1 << (sb.irq - 8))) |
			(sb.old_slave_mask & (1 << (sb.irq - 8)))));
	dos_set_interrupt_vector(sb.vector, sb.old_selector, sb.old_offset);
	__asm__ __volatile__("sti");
	dos_dos_free(sb.selector);
}

static void audio_close_at_exit(void)
{
	system_audio_close();
}

int system_audio_open(unsigned long rate, unsigned long *opened_rate)
{
	const char *dos_setting(const char *name, const char *default_value);
	const char *blaster = dos_setting("sound_blaster", NULL);
	int major, minor, found = FALSE;
	char line[160];

	if (sys_audio_record_open(rate))
	{
		*opened_rate = rate;
		dos_at_exit(audio_close_at_exit);
		return TRUE;
	}
	if (!blaster || !blaster[0])
		blaster = getenv("BLASTER");
	if (blaster && blaster[0])
	{
		parse_blaster(blaster);
		found = dsp_reset();
		if (!found)
		{
			snprintf(line, sizeof(line), "sound: no Sound Blaster answers at %X (BLASTER=%s)", sb.base, blaster);
			system_log(line);
			return FALSE;
		}
	}
	else
	{
		static const unsigned short ports[] = { 0x220, 0x240, 0x260, 0x280 };

		/* (the usual ports; DOSBox's interrupt and DMA until the card says) */
		blaster = "none";
		for (int i = 0; i < (int)(sizeof(ports) / sizeof(ports[0])) && !found; i++)
		{
			parse_blaster("I7 D1 H5");
			sb.base = ports[i];
			if (dos_setting_integer("debug_sound", 0))
			{
				snprintf(line, sizeof(line), "sound: %X: %02X %02X %02X", sb.base, dos_inb(sb.base + 0xE),
					dos_inb(sb.base + 0xA), dos_inb(sb.base + 0xC));
				system_log(line);
			}
			found = dsp_reset();
		}
		if (!found)
		{
			system_log("sound: no BLASTER variable and no Sound Blaster at 220h-280h");
			return FALSE;
		}
	}
	dsp_write(0xE1);
	major = dsp_read();
	minor = dsp_read();
	if (major >= 4 && !strcmp(blaster, "none") && !sb16_resources())
		sb.dma16 = 5;
	sb.sixteen = major >= 4 && sb.dma16 >= 5 && sb.dma16 <= 7;
	if (!sb.sixteen && (major < 2 || sb.dma8 < 0 || sb.dma8 > 3))
	{
		snprintf(line, sizeof(line), "sound: a Sound Blaster with DSP %d.%02d, too old (BLASTER=%s)", major, minor,
			blaster);
		system_log(line);
		return FALSE;
	}
	sb.dma = sb.sixteen ? sb.dma16 : sb.dma8;
	sb.frame_bytes = sb.sixteen ? 4 : 1;
	sb.ring_bytes = RING_FRAMES * sb.frame_bytes;
	if (sb.sixteen)
	{
		sb.rate = rate;
	}
	else
	{
		/* (8-bit mono by the time constant, which the normal mode keeps to
		22 kHz or so) */
		unsigned long time_constant;

		if (rate > 22050)
			rate = 22050;
		time_constant = 256 - 1000000UL / rate;
		sb.rate = 1000000UL / (256 - time_constant);
	}
	if (!ring_allocate())
	{
		system_log("sound: no conventional memory for the Sound Blaster's buffer");
		return FALSE;
	}
	ring_silence(0, RING_FRAMES);
	interrupt_install();
	dma_start();
	if (sb.sixteen)
	{
		unsigned long block = RING_FRAMES - 1;

		dsp_write(0x41);
		dsp_write((unsigned char)(sb.rate >> 8));
		dsp_write((unsigned char)sb.rate);
		/* (16-bit output, auto-initialised, the FIFO on; signed stereo; a
		block of half the ring's samples) */
		dsp_write(0xB6);
		dsp_write(0x30);
		dsp_write((unsigned char)block);
		dsp_write((unsigned char)(block >> 8));
	}
	else
	{
		unsigned long block = RING_FRAMES / 2 - 1;

		dsp_write(0xD1);
		dsp_write(0x40);
		dsp_write((unsigned char)(256 - 1000000UL / sb.rate));
		dsp_write(0x48);
		dsp_write((unsigned char)block);
		dsp_write((unsigned char)(block >> 8));
		dsp_write(0x1C);
	}
	sb.open = TRUE;
	sb.played = sb.written = 0;
	sb.last_position = 0;
	sb.last_update = system_microseconds();
	dos_at_exit(audio_close_at_exit);
	*opened_rate = sb.rate;
	snprintf(line, sizeof(line), "sound: Sound Blaster DSP %d.%02d at %X, interrupt %d, DMA %d: %s at %lu Hz", major,
		minor, sb.base, sb.irq, sb.dma, sb.sixteen ? "16-bit stereo" : "8-bit mono", sb.rate);
	system_log(line);
	return TRUE;
}

void system_audio_update(void (*fill)(short *samples, unsigned long frames), unsigned long lead)
{
	unsigned long long now;
	unsigned long position, delta, ahead;

	if (sys_audio_recording())
	{
		sys_audio_record_update(fill);
		return;
	}
	if (!sb.open)
		return;
	now = system_microseconds();
	position = dma_position();
	delta = (position + RING_FRAMES - sb.last_position) % RING_FRAMES;
	sb.played += delta;
	/* (most of a ring since the last look: a lap may have gone unseen) */
	if ((now - sb.last_update) * sb.rate / 1000000ULL > RING_FRAMES * 3 / 4 || sb.written < sb.played)
		sb.written = sb.played;
	sb.last_position = position;
	sb.last_update = now;
	if (lead > RING_FRAMES - RING_GUARD)
		lead = RING_FRAMES - RING_GUARD;
	ahead = (unsigned long)(sb.written - sb.played);
	while (ahead < lead)
	{
		unsigned long at = (unsigned long)(sb.written % RING_FRAMES);
		unsigned long count = lead - ahead;

		if (count > RING_FRAMES - at)
			count = RING_FRAMES - at;
		if (count > FILL_FRAMES)
			count = FILL_FRAMES;
		fill(sb.mix, count);
		ring_put(at, sb.mix, count);
		sb.written += count;
		ahead += count;
	}
	/* the rest of the ring silent: what plays should the game stall */
	ring_silence((unsigned long)(sb.written % RING_FRAMES), RING_FRAMES - ahead);
	{
		/* (testing, setting debug_sound=1: every five seconds, how far the
		card has played against the clock, and its interrupts) */
		static unsigned long long logged_at;
		static int debug = -1;

		if (debug < 0)
			debug = dos_setting_integer("debug_sound", 0) != 0;
		if (debug && now - logged_at > 5000000ULL)
		{
			char line[160];

			logged_at = now;
			snprintf(line, sizeof(line), "sound: %lu frames played (%lu s), %lu ahead (lead %lu), %lu interrupts",
				(unsigned long)sb.played, (unsigned long)(sb.played / sb.rate), ahead, lead,
				(unsigned long)dos_audio_interrupts);
			system_log(line);
		}
	}
}

void system_audio_close(void)
{
	sys_audio_record_close();
	sb_close();
}
