# PKTSTUB.S
#
# halo-dos: the packet driver's receiver for mTCP in the DOS build. The
# packet driver calls it from its hardware interrupt, in real mode, twice a
# packet: AX=0 asks for a buffer of CX bytes (ES:DI, or 0:0 to drop the
# packet), AX=1 says the packet is in it (DS:SI, CX bytes). packet.cpp
# copies these bytes into conventional memory, at RECEIVER_CODE of the
# block whose start is struct receiver_block (packet.cpp), and gives the
# driver that segment:offset: it runs entirely in real mode, with no switch
# to protected mode. The game's side takes the filled buffers from the
# ring in its own time and gives them back to the free stack.
#
# The fields' offsets here are packet.cpp's struct receiver_block's.

	.equ FREE_COUNT, 2
	.equ RING_NEXT, 4
	.equ BUFFER_LENGTH, 8
	.equ DROPPED, 12
	.equ RECEIVED, 16
	.equ FREE_STACK, 20
	.equ RING_OFFSET, 116
	.equ RING_LENGTH, 216
	.equ RING_SIZE, 49

	.section .rdata,"dr"
	.globl _mtcp_receiver_code
	.globl _mtcp_receiver_code_end
_mtcp_receiver_code:
	.code16
	pushf
	cli
	testw %ax, %ax
	jnz 2f
	# a buffer for CX bytes
	cmpw %cs:BUFFER_LENGTH, %cx
	ja 1f
	pushw %bx
	movw %cs:FREE_COUNT, %bx
	testw %bx, %bx
	jz 3f
	decw %bx
	movw %bx, %cs:FREE_COUNT
	shlw $1, %bx
	movw %cs:FREE_STACK(%bx), %di
	popw %bx
	pushw %cs
	popw %es
	popf
	lret
3:	popw %bx
1:	# none: the packet is dropped
	xorw %di, %di
	movw %di, %es
	addw $1, %cs:DROPPED
	adcw $0, %cs:DROPPED+2
	popf
	lret
2:	# the packet is in DS:SI, CX bytes: onto the ring
	pushw %bx
	movw %cs:RING_NEXT, %bx
	shlw $1, %bx
	movw %si, %cs:RING_OFFSET(%bx)
	movw %cx, %cs:RING_LENGTH(%bx)
	shrw $1, %bx
	incw %bx
	cmpw $RING_SIZE, %bx
	jb 4f
	xorw %bx, %bx
4:	movw %bx, %cs:RING_NEXT
	addw $1, %cs:RECEIVED
	adcw $0, %cs:RECEIVED+2
	popw %bx
	popf
	lret
	.code32
_mtcp_receiver_code_end:
