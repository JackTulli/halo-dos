// DOS_PROFILE_ISR.S
//
// Testing (setting profile=1, dos_profile.c): the timer's interrupt (IRQ 0,
// int 8), run a thousand times a second instead of 18.2, notes where the
// game was: the interrupted EIP, then the return addresses of its frame
// pointer chain (every unit keeps its frame pointer), up to 12 words a
// sample, zeros after the last. Frames are followed only inside the game's
// stack (dos_profile.c sets its bounds), so a stray EBP reads nothing. One
// interrupt in 55 goes on to the BIOS's handler, which keeps its tick and
// ends the interrupt; the others end it here.

	.text
	.globl	_dos_profile_isr
_dos_profile_isr:
	pushl	%eax
	pushl	%ebx
	pushl	%ecx
	pushl	%edx
	pushw	%ds
	movw	%cs:_dos_profile_data_selector, %ax
	movw	%ax, %ds
	cmpl	$0, _dos_profile_on
	je	3f
	movl	_dos_profile_count, %ebx
	cmpl	_dos_profile_capacity, %ebx
	jae	3f
	imull	$48, %ebx, %ebx
	addl	_dos_profile_samples, %ebx
	// (the interrupted EIP, above the 4 words and 1 half word pushed)
	movl	18(%esp), %eax
	movl	%eax, (%ebx)
	movl	$1, %ecx
	movl	%ebp, %edx
1:	cmpl	$12, %ecx
	jae	4f
	cmpl	_dos_profile_stack_low, %edx
	jb	2f
	cmpl	_dos_profile_stack_high, %edx
	jae	2f
	testl	$3, %edx
	jnz	2f
	movl	4(%edx), %eax
	movl	%eax, (%ebx,%ecx,4)
	incl	%ecx
	movl	(%edx), %eax
	cmpl	%edx, %eax
	jbe	2f
	movl	%eax, %edx
	jmp	1b
2:	cmpl	$12, %ecx
	jae	4f
	movl	$0, (%ebx,%ecx,4)
	incl	%ecx
	jmp	2b
4:	incl	_dos_profile_count
3:	decl	_dos_profile_chain_countdown
	jnz	5f
	movl	$55, _dos_profile_chain_countdown
	popw	%ds
	popl	%edx
	popl	%ecx
	popl	%ebx
	popl	%eax
	ljmp	*%cs:_dos_profile_old_vector
5:	movb	$0x20, %al
	outb	%al, $0x20
	popw	%ds
	popl	%edx
	popl	%ecx
	popl	%ebx
	popl	%eax
	iret

	.globl	_dos_profile_data_selector
	.p2align 1
_dos_profile_data_selector:
	.short	0

	// the BIOS's handler (offset, then selector: a far pointer)
	.globl	_dos_profile_old_vector
	.p2align 2
_dos_profile_old_vector:
	.long	0
	.short	0

	.globl	_dos_profile_isr_end
_dos_profile_isr_end:

	.data
	.p2align 2
	.globl	_dos_profile_samples
_dos_profile_samples:
	.long	0
	.globl	_dos_profile_count
_dos_profile_count:
	.long	0
	.globl	_dos_profile_capacity
_dos_profile_capacity:
	.long	0
	.globl	_dos_profile_stack_low
_dos_profile_stack_low:
	.long	0
	.globl	_dos_profile_stack_high
_dos_profile_stack_high:
	.long	0
	.globl	_dos_profile_on
_dos_profile_on:
	.long	0
	.globl	_dos_profile_chain_countdown
_dos_profile_chain_countdown:
	.long	55
	.globl	_dos_profile_data_end
_dos_profile_data_end:
