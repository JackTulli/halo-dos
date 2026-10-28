// DOS_AUDIO_ISR.S
//
// The Sound Blaster's interrupt (dos_audio.c): the card raises it at each
// half of the looping buffer it plays (auto-initialised DMA). Nothing is
// mixed here, the game's thread keeps the buffer filled; the handler only
// acknowledges the card (reading its 8-bit or 16-bit acknowledge port, as
// dos_audio.c set) and ends the interrupt at the PICs, and counts it.

	.text
	.globl	_dos_audio_isr
_dos_audio_isr:
	pushl	%eax
	pushl	%edx
	pushw	%ds
	movw	%cs:_dos_audio_data_selector, %ax
	movw	%ax, %ds
	movw	_dos_audio_ack_port, %dx
	inb	%dx, %al
	incl	_dos_audio_interrupts
	movb	$0x20, %al
	cmpl	$0, _dos_audio_slave
	je	1f
	outb	%al, $0xA0
1:	outb	%al, $0x20
	popw	%ds
	popl	%edx
	popl	%eax
	iret

	.globl	_dos_audio_data_selector
	.p2align 1
_dos_audio_data_selector:
	.short	0

	.globl	_dos_audio_isr_end
_dos_audio_isr_end:

	.data
	.p2align 2
	.globl	_dos_audio_data_start
_dos_audio_data_start:
	.globl	_dos_audio_ack_port
_dos_audio_ack_port:
	.long	0
	.globl	_dos_audio_slave
_dos_audio_slave:
	.long	0
	.globl	_dos_audio_interrupts
_dos_audio_interrupts:
	.long	0
	.globl	_dos_audio_data_end
_dos_audio_data_end:
