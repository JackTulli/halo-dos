/*
R_VOODOO.H

The renderer's 3dfx Voodoo Graphics rasterizer (r_voodoo.c): the card's
registers programmed directly, no Glide. The DOS build finds and starts the
card (port/dos/dos/dos_voodoo.c: the PCI bus, its memory, the video mode)
and hands over its registers; the renderer then draws through them.
*/

#ifndef __R_VOODOO_H
#define __R_VOODOO_H

struct r_voodoo_card
{
	/* the card's 16 MB (PCI BAR0), mapped */
	volatile unsigned long *base;
	/* its screen, as started */
	int width, height;
	/* bytes of texture memory of its first texture unit: at least this
	much (r_voodoo_use puts there what it found) */
	unsigned long texture_memory;
	/* (r_voodoo_use's) 1 if the card leaves out the last column of a clip
	rectangle, which the renderer then makes a column wider */
	int clip_extra;
};

/* the card draws from now (r_internal.h's backend) */
int r_voodoo_use(struct r_voodoo_card *card);
/* the frame drawn shown: the back buffer and the front swapped */
void r_voodoo_swap(void);

#endif /* __R_VOODOO_H */
