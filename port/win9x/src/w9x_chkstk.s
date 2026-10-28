// W9X_CHKSTK.S
//
// __chkstk (and its other name, __alloca_probe), which clang calls in the
// prologue of a function with more than a page of locals: EAX holds the
// size, and the call has the effect of "subl %eax, %esp". On Windows
// (9x as NT) a thread's stack is committed a page at a time below a guard
// page, so each page from the old top of the stack down to the new one is
// touched in order before ESP moves past it; a jump over the guard page
// would fault. nxdk's __chkstk only checks the console's fixed stack.
// (LLVM compiler-rt's i386 chkstk, MIT OR NCSA.)

// SafeSEH-compatible (the start-up check's handler is registered)
.def @feat.00
.scl 3
.type 0
.endef
.globl @feat.00
.set @feat.00, 1

.text
.balign 4
.globl __chkstk
.globl __alloca_probe
__chkstk:
__alloca_probe:
    pushl %ecx
    cmpl $0x1000, %eax
    leal 8(%esp), %ecx      // the stack pointer before the call
    jb 2f
1:
    subl $0x1000, %ecx      // the next page down
    testl %ecx, (%ecx)      // touched (a read is enough for the guard page)
    subl $0x1000, %eax
    cmpl $0x1000, %eax
    ja 1b
2:
    subl %eax, %ecx
    testl %ecx, (%ecx)      // the last, partial page
    leal 4(%esp), %eax      // where the return address is
    movl %ecx, %esp         // the new top of the stack
    movl -4(%eax), %ecx     // ECX as it was
    pushl (%eax)            // the return address, on the new top
    subl %esp, %eax         // EAX as it was (the size)
    ret
