// DOS_KEYBOARD.S
//
// The keyboard's interrupt (IRQ 1, int 9) in protected mode: each byte the
// keyboard controller has (port 60h) goes into a ring of 256 for
// dos_system.c to read, and the interrupt controller is told it is done.
// The BIOS never sees the keys (no typing ahead into DOS while the game
// runs). The handler may come in with any DS: it loads its own, kept in the
// code by dos_system.c. dos_system.c locks this code and the ring in memory
// (hosts that page).

	.text
	.globl	_dos_keyboard_isr
_dos_keyboard_isr:
	pushl	%eax
	pushl	%ebx
	pushw	%ds
	movw	%cs:_dos_keyboard_data_selector, %ax
	movw	%ax, %ds
	inb	$0x60, %al
	movl	_dos_keyboard_head, %ebx
	movb	%al, _dos_keyboard_ring(%ebx)
	incl	%ebx
	andl	$255, %ebx
	movl	%ebx, _dos_keyboard_head
	movb	$0x20, %al
	outb	%al, $0x20
	popw	%ds
	popl	%ebx
	popl	%eax
	iret

	.globl	_dos_keyboard_data_selector
	.p2align 1
_dos_keyboard_data_selector:
	.short	0

	.globl	_dos_keyboard_isr_end
_dos_keyboard_isr_end:

	.data
	.globl	_dos_keyboard_head
	.p2align 2
_dos_keyboard_head:
	.long	0
	.globl	_dos_keyboard_ring
_dos_keyboard_ring:
	.space	256
