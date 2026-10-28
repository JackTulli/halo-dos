// R_SPAN8.S
//
// The 8-bit picture's opaque pixels, hand-written (r_soft.c's raster8_opaque):
// a run of a row's pixels from p, each the texel at u and v (16.16; v is
// times the texture's width), drawn where its depth z is greater than the
// depth buffer's there, and the depth written; u, v and z step by r_span8's
// du, dv and dz. Five kinds:
//
//   unlit         the texel itself (no lighting: the texels are the
//                 picture's colours)
//   shaded        shade[texel] (a light level's row)
//   fogged        fog[shade[texel]]
//   cut_shaded    a cut-out's: texel 0 a hole (no pixel, no depth), the
//                 others shade[remap[texel]]
//   cut_fogged    fog[shade[remap[texel]]]
//
// Each kind two ways, a triangle's rows (one half of it, along one short
// edge) in one call, r_span8's rows of them from pixel_row, each row's ends
// from its two edges:
//
//   r_span8_rows_<kind>()                without perspective: each row's
//                                        start values from the row's (at x =
//                                        0, stepped a row), its light and
//                                        fog levels at its first pixel
//   r_span8_perspective_<kind>()         with: each row's start values from
//                                        the planes (p_*), then its segments
//                                        of up to 16 pixels, each's texture
//                                        coordinates at its end divided out
//                                        (a whole segment's steps by a
//                                        shift, a shorter one's by a
//                                        division), its light and fog
//                                        levels at its start
//
// The perspective's floating point is r_soft.c's own, operation for
// operation in the same order and precision (what the compiler made of it:
// the row's planes and steps floats, w and its step across as the x87
// holds them, a row's s and t rounded to floats between its segments), so
// the picture is the same to the pixel.
//
// What DOSBox charges is the instructions (one cycle each, whatever they
// are), and a Pentium's pairing likes them short too. Three things keep the
// count down:
//
// - The depth buffer is found from the pixel's own address, d = depth_offset
//   + 4 * p (depth_offset the row's: stepped a row by depth_step, which is 0
//   where the 8-bit picture's rows are as many bytes as the depth's are
//   words, its pitch its width), so one pointer steps for both.
// - The texel's index, (u >> 16 & umask) | (v >> 16 & vmask), is
//   ((((u ^ v) & umask << 16) ^ v) >> 16) & (umask | vmask): u's bits where
//   umask is, v's elsewhere, with no register to spare for either half.
// - The pixels go four to a loop pass (one pointer step and test for four),
//   a run's first pass entered at the pixel that leaves a whole number after
//   it, as Duff's device does.
//
// A row's own work, which a triangle of a few pixels a row pays as much as
// its pixels, stays here too: no call, no C set-up a row.
//
// And, at the end, r_vertices8_draw: r_main.c's vertex loop for the 8-bit
// picture's textured draws.
//
// Registers in a run: %ebp the four pixels' first, %ebx depth_offset, %eax
// u, %edx v, %edi z, %esi the texels, %ecx a scratch one.

// SafeSEH-compatible (no handlers here: the Windows 95 build's linker asks)
.def @feat.00
	.scl	3
	.type	0
.endef
.globl @feat.00
.set @feat.00, 1

// r_span8's fields (r_internal.h's struct r_span8_constants: keep the order)
.set TEXELS, 0
.set SHADE, 4
.set FOG, 8
.set DU, 12
.set DV, 16
.set DZ, 20
.set UMASK16, 24
.set INDEX_MASK, 28
.set REMAP, 32
.set DEPTH_OFFSET, 36
.set PIXEL_ROW, 40
.set ROWS, 44
.set X_LONG, 48
.set LONG_STEP, 52
.set X_SHORT, 56
.set SHORT_STEP, 60
.set X0, 64
.set X1, 68
.set U_ROW, 72
.set U_DY, 76
.set V_ROW, 80
.set V_DY, 84
.set Z_ROW, 88
.set Z_DY, 92
.set LIGHT_ROW, 96
.set LIGHT_DY, 100
.set DLIGHT, 104
.set FOG_ROW, 108
.set FOG_DY, 112
.set DFOG, 116
.set PITCH, 120
.set CONSTANT_SHADE, 124
.set END, 128
.set SEGMENT_P, 132
.set SEGMENT_U, 136
.set SEGMENT_V, 140
.set SEGMENT_Z, 144
.set SEGMENT_LIGHT, 148
.set SEGMENT_FOG, 152
.set DLIGHT16, 156
.set DFOG16, 160
.set WIDTH_SHIFT, 164
.set P_W_ROW, 168
.set P_S_ROW, 172
.set P_T_ROW, 176
.set P_L_ROW, 180
.set P_F_ROW, 184
.set P_S_DY, 188
.set P_T_DY, 192
.set P_L_DY, 196
.set P_F_DY, 200
.set P_S_DX, 204
.set P_T_DX, 208
.set P_L_DX, 212
.set P_F_DX, 216
.set P_K, 220
.set P_HALF, 224
.set P_FIXED, 228
.set P_DEPTH_BIAS, 232
.set P_S, 236
.set P_T, 240
.set P_LEFT, 244
.set P_COUNT, 248
.set P_U_END, 252
.set P_V_END, 256
.set P_TMP, 260
.set P_W_DX, 264
.set P_W_DY, 272
.set DEPTH_STEP, 280

// the kinds
.set UNLIT, 0
.set SHADED, 1
.set FOGGED, 2
.set CUT_SHADED, 3
.set CUT_FOGGED, 4

// r_statistics.pixels_drawn (r_public.h's struct r_statistics)
.set PIXELS_DRAWN, 8

	.text

// one pixel: slot 0 to 3 past %ebp; u, v and z stepped after it
.macro PIXEL kind, slot
	cmpl	4*\slot(%ebx,%ebp,4), %edi
	jbe	1f
.if \kind < CUT_SHADED
	movl	%edi, 4*\slot(%ebx,%ebp,4)
.endif
	movl	%eax, %ecx
	xorl	%edx, %ecx
	andl	_r_span8+UMASK16, %ecx
	xorl	%edx, %ecx
	shrl	$16, %ecx
	andl	_r_span8+INDEX_MASK, %ecx
.if \kind == UNLIT
	movb	(%esi,%ecx), %cl
.else
	movzbl	(%esi,%ecx), %ecx
.if \kind >= CUT_SHADED
	// (the texel read before the depth is written: a hole writes neither)
	testl	%ecx, %ecx
	jz	1f
	movl	%edi, 4*\slot(%ebx,%ebp,4)
	addl	_r_span8+REMAP, %ecx
	movzbl	(%ecx), %ecx
.endif
	addl	_r_span8+SHADE, %ecx
.if \kind == FOGGED || \kind == CUT_FOGGED
	movzbl	(%ecx), %ecx
	addl	_r_span8+FOG, %ecx
.endif
	movb	(%ecx), %cl
.endif
	movb	%cl, \slot(%ebp)
1:
	addl	_r_span8+DU, %eax
	addl	_r_span8+DV, %edx
	addl	_r_span8+DZ, %edi
.endm

// %ebp a run's first pixel, %ecx its count (at least 1): its end kept, %ebp
// moved back to its first pass's slot 0 and %ecx that slot's number
.macro ENTER
	addl	%ebp, %ecx
	movl	%ecx, _r_span8+END
	subl	%ebp, %ecx
	negl	%ecx
	andl	$3, %ecx
	subl	%ecx, %ebp
.endm

// the run, four pixels a pass, entered at slot %ecx
.macro RUN kind
	cmpl	$2, %ecx
	je	.Lslot2_\@
	ja	.Lslot3_\@
	testl	%ecx, %ecx
	jnz	.Lslot1_\@
.Lslot0_\@:
	PIXEL	\kind, 0
.Lslot1_\@:
	PIXEL	\kind, 1
.Lslot2_\@:
	PIXEL	\kind, 2
.Lslot3_\@:
	PIXEL	\kind, 3
	addl	$4, %ebp
	cmpl	_r_span8+END, %ebp
	jb	.Lslot0_\@
.endm

// a 16.16 level in %edx as a row of a colour map (r_soft.c's light_row:
// whole levels, kept inside the map), to field
.macro LEVEL_ROW last, map, field
	sarl	$16, %edx
	jns	1f
	xorl	%edx, %edx
1:
	cmpl	$\last, %edx
	jle	2f
	movl	$\last, %edx
2:
	shll	$8, %edx
	addl	$\map, %edx
	movl	%edx, _r_span8+\field
.endm

// the level at the row's first pixel (%eax its x)
.macro LEVEL start, step, last, map, field
	movl	%eax, %edx
	imull	_r_span8+\step, %edx
	addl	_r_span8+\start, %edx
	LEVEL_ROW \last, \map, \field
.endm

// a row's pixels: between its edges (rounded: a pixel whose centre is on
// the left edge in, on the right out), inside x0 and x1; %eax its first x,
// %edx how many, to skip if none
.macro EDGES skip
	movl	_r_span8+X_LONG, %eax
	movl	_r_span8+X_SHORT, %edx
	cmpl	%edx, %eax
	jle	1f
	xchgl	%eax, %edx
1:
	addl	$0x8000, %eax
	sarl	$16, %eax
	addl	$0x8000, %edx
	sarl	$16, %edx
	cmpl	_r_span8+X0, %eax
	jge	2f
	movl	_r_span8+X0, %eax
2:
	cmpl	_r_span8+X1, %edx
	jle	3f
	movl	_r_span8+X1, %edx
3:
	subl	%eax, %edx
	jle	\skip
	addl	%edx, _r_statistics+PIXELS_DRAWN
.endm

// down a row: the edges, the picture's row and its depth offset
.macro DOWN
	movl	_r_span8+LONG_STEP, %eax
	addl	%eax, _r_span8+X_LONG
	movl	_r_span8+SHORT_STEP, %eax
	addl	%eax, _r_span8+X_SHORT
	movl	_r_span8+PITCH, %eax
	addl	%eax, _r_span8+PIXEL_ROW
	movl	_r_span8+DEPTH_STEP, %eax
	addl	%eax, _r_span8+DEPTH_OFFSET
.endm

// r_span8_rows_<kind>()
.macro ROWS name, kind
	.globl	\name
	.p2align 4
\name:
	pushl	%ebp
	pushl	%ebx
	pushl	%esi
	pushl	%edi
	movl	_r_span8+TEXELS, %esi
.Lrow_\@:
	EDGES	.Lnext_\@
	movl	_r_span8+DEPTH_OFFSET, %ebx
	movl	_r_span8+PIXEL_ROW, %ebp
	addl	%eax, %ebp
	movl	%edx, %ecx
.if \kind != UNLIT
	movl	_r_span8+CONSTANT_SHADE, %edx
	movl	%edx, _r_span8+SHADE
	testl	%edx, %edx
	jnz	6f
	LEVEL	LIGHT_ROW, DLIGHT, 31, _r_color_map, SHADE
6:
.endif
.if \kind == FOGGED || \kind == CUT_FOGGED
	LEVEL	FOG_ROW, DFOG, 15, _r_fog_map, FOG
.endif
	movl	%eax, %edi
	imull	_r_span8+DZ, %edi
	addl	_r_span8+Z_ROW, %edi
	movl	%eax, %edx
	imull	_r_span8+DV, %edx
	addl	_r_span8+V_ROW, %edx
	imull	_r_span8+DU, %eax
	addl	_r_span8+U_ROW, %eax
	ENTER
	RUN	\kind
.Lnext_\@:
	DOWN
	movl	_r_span8+U_DY, %eax
	addl	%eax, _r_span8+U_ROW
	movl	_r_span8+V_DY, %eax
	addl	%eax, _r_span8+V_ROW
	movl	_r_span8+Z_DY, %eax
	addl	%eax, _r_span8+Z_ROW
.if \kind != UNLIT
	movl	_r_span8+LIGHT_DY, %eax
	addl	%eax, _r_span8+LIGHT_ROW
.endif
.if \kind == FOGGED || \kind == CUT_FOGGED
	movl	_r_span8+FOG_DY, %eax
	addl	%eax, _r_span8+FOG_ROW
.endif
	decl	_r_span8+ROWS
	jnz	.Lrow_\@
	popl	%edi
	popl	%esi
	popl	%ebx
	popl	%ebp
	ret
.endm

// a plane's value at the row's start (%st(2) its first pixel's centre's x;
// times 65536 if fixed), to field: as an integer (fistp), else a float
.macro PLANE dx, row, field, fixed
	flds	_r_span8+\dx
	fmul	%st(2), %st
	fadds	_r_span8+\row
.if \fixed
	fmuls	_r_span8+P_FIXED
	fistpl	_r_span8+\field
.else
	fstps	_r_span8+\field
.endif
.endm

// a float plane's row value stepped down a row (kept a float; w's step a
// double)
.macro PLANE_DOWN row, dy, double=0
	flds	_r_span8+\row
.if \double
	faddl	_r_span8+\dy
.else
	fadds	_r_span8+\dy
.endif
	fstps	_r_span8+\row
.endm

// r_span8_perspective_<kind>()
.macro PERSPECTIVE name, kind
	.globl	\name
	.p2align 4
\name:
	pushl	%ebp
	pushl	%ebx
	pushl	%esi
	pushl	%edi
.Lrow_\@:
	EDGES	.Lnext_\@
	movl	%edx, _r_span8+P_LEFT
	movl	_r_span8+PIXEL_ROW, %ecx
	addl	%eax, %ecx
	movl	%ecx, _r_span8+SEGMENT_P
	// the row's start: x the first pixel's centre; w (as the x87 has it)
	// and its depth; the light and fog levels; s and t (floats); the
	// texture coordinates there
	movl	%eax, _r_span8+P_TMP
	fildl	_r_span8+P_TMP
	fadds	_r_span8+P_HALF
	fldl	_r_span8+P_W_DX
	fmul	%st(1), %st
	fadds	_r_span8+P_W_ROW
	fistl	_r_span8+P_TMP
	movl	_r_span8+P_TMP, %eax
	addl	_r_span8+P_DEPTH_BIAS, %eax
	movl	%eax, _r_span8+SEGMENT_Z
.if \kind != UNLIT
	PLANE	P_L_DX, P_L_ROW, SEGMENT_LIGHT, 1
.endif
.if \kind == FOGGED || \kind == CUT_FOGGED
	PLANE	P_F_DX, P_F_ROW, SEGMENT_FOG, 1
.endif
	PLANE	P_S_DX, P_S_ROW, P_S, 0
	PLANE	P_T_DX, P_T_ROW, P_T, 0
	fstp	%st(1)
	flds	_r_span8+P_K
	fdiv	%st(1), %st
	flds	_r_span8+P_S
	fmul	%st(1), %st
	fistpl	_r_span8+SEGMENT_U
	flds	_r_span8+P_T
	fmul	%st(1), %st
	fistpl	_r_span8+SEGMENT_V
	fstp	%st(0)
.Lsegment_\@:
	// a segment: up to 16 pixels; w, s and t at its end (s and t as the
	// x87 has them, then kept as floats), the texture coordinates there
	movl	_r_span8+P_LEFT, %ecx
	cmpl	$16, %ecx
	jle	4f
	movl	$16, %ecx
4:
	subl	%ecx, _r_span8+P_LEFT
	movl	%ecx, _r_span8+P_COUNT
	fildl	_r_span8+P_COUNT
	fldl	_r_span8+P_W_DX
	fmul	%st(1), %st
	faddp	%st, %st(2)
	flds	_r_span8+P_S_DX
	fmul	%st(1), %st
	fadds	_r_span8+P_S
	flds	_r_span8+P_T_DX
	fmul	%st(2), %st
	fadds	_r_span8+P_T
	flds	_r_span8+P_K
	fdiv	%st(4), %st
	fld	%st(2)
	fmul	%st(1), %st
	fistpl	_r_span8+P_U_END
	fld	%st(1)
	fmul	%st(1), %st
	fistpl	_r_span8+P_V_END
	fstp	%st(0)
	fstps	_r_span8+P_T
	fstps	_r_span8+P_S
	fstp	%st(0)
	// the steps: from where the last segment ended to this one's end, over
	// its count (a shift for a whole one, else a division, toward 0); v's
	// times the width
	movl	_r_span8+P_U_END, %eax
	subl	_r_span8+SEGMENT_U, %eax
	movl	_r_span8+P_V_END, %ebx
	subl	_r_span8+SEGMENT_V, %ebx
	cmpl	$16, %ecx
	jne	5f
	sarl	$4, %eax
	sarl	$4, %ebx
	jmp	6f
5:
	cltd
	idivl	%ecx
	xchgl	%eax, %ebx
	cltd
	idivl	%ecx
	xchgl	%eax, %ebx
6:
	movl	%eax, _r_span8+DU
	movl	_r_span8+WIDTH_SHIFT, %ecx
	shll	%cl, %ebx
	movl	%ebx, _r_span8+DV
	// v at its start, times the width; the next segment's start
	movl	_r_span8+SEGMENT_V, %edx
	shll	%cl, %edx
	movl	_r_span8+P_V_END, %ebx
	movl	%ebx, _r_span8+SEGMENT_V
	movl	%edx, %ebx
.if \kind != UNLIT
	movl	_r_span8+CONSTANT_SHADE, %edx
	movl	%edx, _r_span8+SHADE
	testl	%edx, %edx
	jnz	7f
	movl	_r_span8+SEGMENT_LIGHT, %edx
	LEVEL_ROW 31, _r_color_map, SHADE
7:
.endif
.if \kind == FOGGED || \kind == CUT_FOGGED
	movl	_r_span8+SEGMENT_FOG, %edx
	LEVEL_ROW 15, _r_fog_map, FOG
.endif
	movl	%ebx, %edx
	movl	_r_span8+SEGMENT_U, %eax
	movl	_r_span8+P_U_END, %ecx
	movl	%ecx, _r_span8+SEGMENT_U
	movl	_r_span8+SEGMENT_Z, %edi
	movl	_r_span8+SEGMENT_P, %ebp
	movl	_r_span8+P_COUNT, %ecx
	movl	_r_span8+DEPTH_OFFSET, %ebx
	movl	_r_span8+TEXELS, %esi
	ENTER
	RUN	\kind
	// the next starts where this one ended, its light and fog a segment on
	movl	%ebp, _r_span8+SEGMENT_P
	movl	%edi, _r_span8+SEGMENT_Z
	movl	_r_span8+DLIGHT16, %eax
	addl	%eax, _r_span8+SEGMENT_LIGHT
	movl	_r_span8+DFOG16, %eax
	addl	%eax, _r_span8+SEGMENT_FOG
	cmpl	$0, _r_span8+P_LEFT
	jg	.Lsegment_\@
	fstp	%st(0)
.Lnext_\@:
	// down a row (the planes' row values kept floats)
	DOWN
	PLANE_DOWN P_W_ROW, P_W_DY, 1
	PLANE_DOWN P_S_ROW, P_S_DY
	PLANE_DOWN P_T_ROW, P_T_DY
.if \kind != UNLIT
	PLANE_DOWN P_L_ROW, P_L_DY
.endif
.if \kind == FOGGED || \kind == CUT_FOGGED
	PLANE_DOWN P_F_ROW, P_F_DY
.endif
	decl	_r_span8+ROWS
	jnz	.Lrow_\@
	popl	%edi
	popl	%esi
	popl	%ebx
	popl	%ebp
	ret
.endm

	ROWS	_r_span8_rows_unlit, UNLIT
	ROWS	_r_span8_rows_shaded, SHADED
	ROWS	_r_span8_rows_fogged, FOGGED
	ROWS	_r_span8_rows_cut_shaded, CUT_SHADED
	ROWS	_r_span8_rows_cut_fogged, CUT_FOGGED

	PERSPECTIVE	_r_span8_perspective_unlit, UNLIT
	PERSPECTIVE	_r_span8_perspective_shaded, SHADED
	PERSPECTIVE	_r_span8_perspective_fogged, FOGGED
	PERSPECTIVE	_r_span8_perspective_cut_shaded, CUT_SHADED
	PERSPECTIVE	_r_span8_perspective_cut_fogged, CUT_FOGGED

// ---------- r_vertices8_draw(): r_main.c's vertex loop for a textured draw
// in the 8-bit picture
//
// Each vertex: into view space by turn and offset (or as it is, view_space),
// its light level (its colour's, remembered from the last vertex's: most of
// a draw's are the same) and fog level, its outcode, and where it is in
// front of the near plane its projection; all a clip vertex and a projected
// one hold, as r_main.c's loop leaves them. The floating point is that
// loop's own, operation for operation in the same order and precision: the
// transformed x and y floats from then on, z as the x87 has it while the
// last colour holds (the compiler kept it on its stack) and a float after a
// new colour's light (it read it back), the fog worked out from that z
// before either. Comparisons are ordered (a NaN sets no bit), as C's.
//
// r_vertices8 (r_internal.h): turn +0 (3 x 3), offset +36, fog_start +48,
// fog_scale +52, z_near +56, far +60, tangent_x +64, tangent_y +68, focal_x
// +72, focal +76, centre_x +80, centre_y +84, vertices +88, transformed +92,
// projected +96, codes +100, count +104, view_space +108, fogged +112,
// last_color +116, last_light +120, red +124, green +128, blue +132, eighth
// +136, fifteen +140, one +144, scratch +148. A struct r_vertex is 24 bytes
// (position, u, v, colour), a clip vertex 60 (x, y, z, u, v, r, g, b, fog's
// r, g, b, light, fog, lightmap u, v), a projected one 68 (x, y, w, s, t,
// u, v, r, g, b, fog's r, g, b, light, fog, lightmap s, t).

.set V_TURN, 0
.set V_OFFSET, 36
.set V_FOG_START, 48
.set V_FOG_SCALE, 52
.set V_NEAR, 56
.set V_FAR, 60
.set V_TANGENT_X, 64
.set V_TANGENT_Y, 68
.set V_FOCAL_X, 72
.set V_FOCAL, 76
.set V_CENTRE_X, 80
.set V_CENTRE_Y, 84
.set V_VERTICES, 88
.set V_TRANSFORMED, 92
.set V_PROJECTED, 96
.set V_CODES, 100
.set V_COUNT, 104
.set V_VIEW_SPACE, 108
.set V_FOGGED, 112
.set V_LAST_COLOR, 116
.set V_LAST_LIGHT, 120
.set V_RED, 124
.set V_GREEN, 128
.set V_BLUE, 132
.set V_EIGHTH, 136
.set V_FIFTEEN, 140
.set V_ONE, 144
.set V_SCRATCH, 148

// one row of the turn, the vertex at %esi: ((t0 p0 + t1 p1) + t2 p2) + offset
.macro TURN_ROW row
	flds	(%esi)
	fmuls	_r_vertices8+V_TURN+12*\row
	flds	_r_vertices8+V_TURN+12*\row+4
	fmuls	4(%esi)
	faddp	%st, %st(1)
	flds	_r_vertices8+V_TURN+12*\row+8
	fmuls	8(%esi)
	faddp	%st, %st(1)
	fadds	_r_vertices8+V_OFFSET+4*\row
.endm

// an outcode's bit where %st(0) > %st(1) (ordered), popping %st(0)
.macro ABOVE bit
	fucomp	%st(1)
	fnstsw	%ax
	sahf
	jbe	1f
	orl	$\bit, %ecx
1:
.endm

// a colour channel (the byte at offset in the vertex) times its weight
.macro CHANNEL offset, weight
	movzbl	\offset(%esi), %edx
	movl	%edx, _r_vertices8+V_SCRATCH
	fildl	_r_vertices8+V_SCRATCH
	fmuls	_r_vertices8+\weight
.endm

	.globl	_r_vertices8_draw
	.p2align 4
_r_vertices8_draw:
	pushl	%ebp
	pushl	%ebx
	pushl	%esi
	pushl	%edi
	movl	_r_vertices8+V_VERTICES, %esi
	movl	_r_vertices8+V_TRANSFORMED, %edi
	movl	_r_vertices8+V_PROJECTED, %ebx
	movl	_r_vertices8+V_CODES, %ebp
.Lvertex:
	xorl	%ecx, %ecx
	// x, y (floats) and z (as the x87 has it)
	cmpl	$0, _r_vertices8+V_VIEW_SPACE
	je	2f
	movl	(%esi), %eax
	movl	%eax, (%edi)
	movl	4(%esi), %eax
	movl	%eax, 4(%edi)
	flds	8(%esi)
	fsts	8(%edi)
	jmp	3f
2:
	TURN_ROW 0
	fstps	(%edi)
	TURN_ROW 1
	fstps	4(%edi)
	TURN_ROW 2
	fsts	8(%edi)
3:
	// the texture coordinates; no colour terms (the 8-bit picture's
	// textured triangles read the light level alone) and no lightmap
	movl	12(%esi), %eax
	movl	%eax, 12(%edi)
	movl	16(%esi), %eax
	movl	%eax, 16(%edi)
	xorl	%eax, %eax
	movl	%eax, 20(%edi)
	movl	%eax, 24(%edi)
	movl	%eax, 28(%edi)
	movl	%eax, 32(%edi)
	movl	%eax, 36(%edi)
	movl	%eax, 40(%edi)
	movl	%eax, 52(%edi)
	movl	%eax, 56(%edi)
	// the fog: (z - start) times the scale, kept between 0 and 1, in
	// fifteenths
	cmpl	$0, _r_vertices8+V_FOGGED
	je	5f
	fld	%st(0)
	fsubs	_r_vertices8+V_FOG_START
	fmuls	_r_vertices8+V_FOG_SCALE
	fldz
	fucomp	%st(1)
	fnstsw	%ax
	sahf
	jbe	4f
	fstp	%st(0)
	fldz
4:
	fcoms	_r_vertices8+V_ONE
	fnstsw	%ax
	sahf
	jbe	6f
	fstp	%st(0)
	fld1
6:
	fmuls	_r_vertices8+V_FIFTEEN
	fsts	48(%edi)
	ftst
	fnstsw	%ax
	sahf
	jbe	7f
	orl	$0x40, %ecx
7:
	fstp	%st(0)
	jmp	8f
5:
	movl	$0, 48(%edi)
8:
	// the light level: the last vertex's where the colour is its
	movl	20(%esi), %eax
	cmpl	_r_vertices8+V_LAST_COLOR, %eax
	jne	9f
	movl	_r_vertices8+V_LAST_LIGHT, %eax
	movl	%eax, 44(%edi)
	jmp	.Lcodes
9:
	// (a whole level of the colour's brightness, at most the top one; z
	// from here the float stored)
	movl	%eax, _r_vertices8+V_LAST_COLOR
	CHANNEL	22, V_RED
	CHANNEL	21, V_GREEN
	faddp	%st, %st(1)
	CHANNEL	20, V_BLUE
	faddp	%st, %st(1)
	fmuls	_r_vertices8+V_EIGHTH
	fistpl	_r_vertices8+V_SCRATCH
	movl	_r_vertices8+V_SCRATCH, %eax
	cmpl	$31, %eax
	jl	10f
	movl	$31, %eax
10:
	movl	%eax, _r_vertices8+V_SCRATCH
	fildl	_r_vertices8+V_SCRATCH
	fstps	_r_vertices8+V_LAST_LIGHT
	movl	_r_vertices8+V_LAST_LIGHT, %eax
	movl	%eax, 44(%edi)
	fstp	%st(0)
	flds	8(%edi)
.Lcodes:
	// the outcode: near, far; the sides (x against z times the tangent,
	// and against minus that), the top and bottom
	flds	_r_vertices8+V_NEAR
	ABOVE	0x01
	fcoms	_r_vertices8+V_FAR
	fnstsw	%ax
	sahf
	jbe	11f
	orl	$0x02, %ecx
11:
	fld	%st(0)
	fmuls	_r_vertices8+V_TANGENT_X
	flds	(%edi)
	fucom	%st(1)
	fnstsw	%ax
	sahf
	jbe	12f
	orl	$0x08, %ecx
12:
	fxch	%st(1)
	fchs
	ABOVE	0x04
	fstp	%st(0)
	fld	%st(0)
	fmuls	_r_vertices8+V_TANGENT_Y
	flds	4(%edi)
	fucom	%st(1)
	fnstsw	%ax
	sahf
	jbe	13f
	orl	$0x10, %ecx
13:
	fxch	%st(1)
	fchs
	ABOVE	0x20
	fstp	%st(0)
	// projected, where in front of the near plane: w = 1 / z, the
	// position on the screen, the texture coordinates over z
	testl	$0x01, %ecx
	jnz	14f
	fld1
	fdiv	%st(1), %st
	fstp	%st(1)
	flds	(%edi)
	fmul	%st(1), %st
	fmuls	_r_vertices8+V_FOCAL_X
	fadds	_r_vertices8+V_CENTRE_X
	fstps	(%ebx)
	fld	%st(0)
	fmuls	4(%edi)
	fmuls	_r_vertices8+V_FOCAL
	fsubrs	_r_vertices8+V_CENTRE_Y
	fstps	4(%ebx)
	fsts	8(%ebx)
	fld	%st(0)
	fmuls	12(%edi)
	fstps	12(%ebx)
	fmuls	16(%edi)
	fstps	16(%ebx)
	movl	12(%edi), %eax
	movl	%eax, 20(%ebx)
	movl	16(%edi), %eax
	movl	%eax, 24(%ebx)
	movl	44(%edi), %eax
	movl	%eax, 52(%ebx)
	movl	48(%edi), %eax
	movl	%eax, 56(%ebx)
	jmp	15f
14:
	fstp	%st(0)
15:
	movb	%cl, (%ebp)
	addl	$24, %esi
	addl	$60, %edi
	addl	$68, %ebx
	incl	%ebp
	decl	_r_vertices8+V_COUNT
	jnz	.Lvertex
	popl	%edi
	popl	%esi
	popl	%ebx
	popl	%ebp
	ret
