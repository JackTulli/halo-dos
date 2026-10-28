// DOS_ENTRY.S
//
// The DOS build's entry point (wlink option start=_dos_start). DOS/32A
// enters with CS, DS and SS flat, ESP at the top of the stack object, and
// ES selecting the program's PSP: that selector is kept for dos_start.c,
// and ES is made DS's, as the compiled code expects (string instructions
// write through ES). Then the C start-up, which never returns.

	.text
	.globl	_dos_start
_dos_start:
	cld
	movw	%es, %ax
	movw	%ax, _dos_psp_selector
	movw	%ds, %ax
	movw	%ax, %es
	andl	$-16, %esp
	call	_dos_start_c
0:
	hlt
	jmp	0b

	.data
	.globl	_dos_psp_selector
	.p2align 1
_dos_psp_selector:
	.short	0
