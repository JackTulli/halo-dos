// W9X_TLS.S
//
// The offset of the thread's TLS array in its thread information block,
// which clang's code for __thread variables reads (fs:[__tls_array]). The
// 9x thread information block keeps it where NT's does, at 0x2C, which is
// why MSVC's __declspec(thread) works in a 9x executable; nxdk's runtime
// says 0x04 (the console kernel's). The TLS directory itself, without
// callbacks (the 9x loader never calls them), is in w9x_start.c.

// SafeSEH-compatible (the start-up check's handler is registered)
.def @feat.00
.scl 3
.type 0
.endef
.globl @feat.00
.set @feat.00, 1

.globl __tls_array
.set __tls_array, 0x2C
