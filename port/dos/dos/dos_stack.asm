; DOS_STACK.ASM
;
; The linear executable's stack object. wlink sizes the segment of class
; STACK by its option stack (tools/dos_build.py); clang's COFF objects have
; no way to name a class, so this one Open Watcom object (wasm) gives it.

.386p

_STACK segment dword stack use32 'STACK'
_STACK ends

end
