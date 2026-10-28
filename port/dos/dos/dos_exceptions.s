// DOS_EXCEPTIONS.S
//
// The DPMI exception handlers (int 31h function 0203h; dos_crash.c installs
// them). The host calls one on its own stack with this frame:
//
//     [esp+0]  return EIP, [esp+4] return CS   (back to the host: retf)
//     [esp+8]  error code
//     [esp+12] EIP, [esp+16] CS, [esp+20] EFLAGS, [esp+24] ESP, [esp+28] SS
//
// Each records the fault in _dos_crash_record, then edits the frame so that
// the host "returns" into _dos_crash_entry on a stack of its own (a stack
// overflow leaves none to run on), with interrupts as they were.

	.text

	.macro	EXCEPTION number
	.globl	_dos_exception_\number
_dos_exception_\number:
	pushl	$\number
	jmp	exception_common
	.endm

	EXCEPTION 0
	EXCEPTION 1
	EXCEPTION 3
	EXCEPTION 4
	EXCEPTION 5
	EXCEPTION 6
	EXCEPTION 7
	EXCEPTION 8
	EXCEPTION 10
	EXCEPTION 11
	EXCEPTION 12
	EXCEPTION 13
	EXCEPTION 14
	EXCEPTION 16
	EXCEPTION 17

// after the push: [esp] number, then the host's frame 4 bytes further on
exception_common:
	pushl	%eax
	pushw	%ds
	// (the data selector, kept in the code by dos_crash.c: the host enters
	// with any DS)
	movw	%cs:_dos_crash_data_selector, %ax
	movw	%ax, %ds
	// [esp+0] ds (2), [esp+2] eax, [esp+6] number, [esp+10] the frame
	movl	6(%esp), %eax
	movl	%eax, _dos_crash_record
	movl	18(%esp), %eax
	movl	%eax, _dos_crash_record+4
	movl	22(%esp), %eax
	movl	%eax, _dos_crash_record+8
	movl	30(%esp), %eax
	movl	%eax, _dos_crash_record+12
	movl	34(%esp), %eax
	movl	%eax, _dos_crash_record+16
	movl	2(%esp), %eax
	movl	%eax, _dos_crash_record+20
	movl	%ebx, _dos_crash_record+24
	movl	%ecx, _dos_crash_record+28
	movl	%edx, _dos_crash_record+32
	movl	%esi, _dos_crash_record+36
	movl	%edi, _dos_crash_record+40
	movl	%ebp, _dos_crash_record+44
	// back into _dos_crash_entry, on the crash stack, in our segments
	movl	$_dos_crash_entry, %eax
	movl	%eax, 22(%esp)
	xorl	%eax, %eax
	movw	%cs, %ax
	movl	%eax, 26(%esp)
	movl	$_dos_crash_stack+32768, %eax
	movl	%eax, 34(%esp)
	xorl	%eax, %eax
	movw	_dos_crash_data_selector, %ax
	movl	%eax, 38(%esp)
	popw	%ds
	popl	%eax
	addl	$4, %esp
	lret

	// (here, in the code segment, read through CS above)
	.globl	_dos_crash_data_selector
	.p2align 1
_dos_crash_data_selector:
	.short	0
