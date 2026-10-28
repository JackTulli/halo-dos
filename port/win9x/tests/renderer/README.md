# Tests of the 9x renderers

These run on the PC that builds the game, not on Windows 98: they check
the parts of the renderers that need no window and no OpenGL.

## The vertex program evaluators (`t_vsh_cpu.c`)

The light renderer (OpenGL 1.1) has no vertex shaders, so the game's NV2A
vertex programs run on the processor: `port/linux/src/nv2a_vsh_cpu.c`, a
scalar evaluator and an SSE one (no SSE2: the Pentium III is the least
processor of the 9x build). `tools/nv2a_vsh_cpu_test.py` compares both
with `tools/nv2a_vsh_reference.py`, an interpreter written from the GLSL
that `port/linux/src/nv2a_vsh.c` generates for the same programs.
`t_vsh_cpu.c` is the program in between: it reads the cases the test
writes, runs the evaluators and writes their results.

Build it with the 9x build's processor flags (from the repository's root,
in the shell the Windows build is made in):

```sh
OUT=build/win9x/tests/renderer
FLAGS="--target=i686-pc-windows-msvc -march=pentium3 -mfpmath=sse -ffp-eval-method=source
  -ffp-contract=off -fno-math-errno -O2 -fms-extensions -fno-strict-aliasing -std=gnu11
  -DHALO_LINUX_PLATFORM_LAYER -DHALO_WIN9X=1 -D_USE_32BIT_TIME_T -D_CRT_SECURE_NO_WARNINGS
  -include port/windows/include/halo_windows_prefix.h -Iport/windows/include/posix
  -Iport/windows/include/crt -Iport/linux/src -Iport/windows/include -iquote port/linux/include
  -Isource -Isource/cseries -Iport/include/xdk -Iport/third_party/khronos -Iport/win9x/platform"
mkdir -p $OUT
clang $FLAGS -c port/linux/src/nv2a_vsh_cpu.c -o $OUT/nv2a_vsh_cpu.o
clang $FLAGS -c port/win9x/tests/renderer/t_vsh_cpu.c -o $OUT/t_vsh_cpu.o
clang --target=i686-pc-windows-msvc -fuse-ld=lld $OUT/nv2a_vsh_cpu.o $OUT/t_vsh_cpu.o -o $OUT/t_vsh_cpu.exe
```

Then:

```sh
python3 tools/xbox_sse2_check.py $OUT/nv2a_vsh_cpu.o      # no instruction the Pentium III lacks
python3 tools/nv2a_vsh_cpu_test.py $OUT/t_vsh_cpu.exe     # the comparison; a few minutes
python3 tools/nv2a_vsh_cpu_test.py $OUT/t_vsh_cpu.exe --classes   # each program's classification
python3 tools/nv2a_vsh_cpu_test.py $OUT/t_vsh_cpu.exe --bench     # vertices a second on this PC
python3 tools/nv2a_vsh_cpu_test.py $OUT/t_vsh_cpu.exe --count     # instructions a vertex, and a Pentium III's cycles
```

The comparison runs each of the game's 67 programs on random vertices
(every element format, missing and constant elements, odd strides) with
random constants, for each set of result arrays a pass of the light
renderer asks for, and a few thousand random programs that cover the
rest of the instruction set. It fails unless:

- the scalar and the SSE evaluator give the same bytes (results that are
  not numbers are compared as such);
- neither leaves MXCSR changed, whatever it was;
- with `NV2A_VSH_CPU_EXACT` the results are the reference's to the bit
  (results of `exp`, `log` and `lit` come from the C library and may
  differ by 4e-6 of their size);
- without it, `rcp`, `rcc` and `rsq` (the processor's estimates and one
  Newton step) are within 2^-20 of the exact results over the whole range
  of floats;
- no byte outside the arrays asked for is written;
- the matrices of affine outputs (`nv2a_vsh_cpu_affine_output`) reproduce
  the reference's outputs.

`--count` single-steps the evaluators with the processor's trap flag and
counts the instructions by kind; the cycles it prints are what those
counts cost a Pentium III by its instruction tables (the model is in the
test), not a measurement.

## The light renderer's device (`t_fixed_device.c`)

`display.renderer = "gl1"` draws the Xbox's Direct3D device with OpenGL
1.1's fixed function (`port/linux/src/xgpu_fixed.c`,
`xgpu_fixed_textures.c`, behind the entry points of `d3d8_gl.c`).
`t_fixed_device.c` links that device as it is against a stand-in for the
driver that records every OpenGL call, and draws with the game's own
vertex programs, vertex formats and pixel states. No window, no OpenGL.

```sh
tools/t_fixed_device.sh            # builds into build/win9x/tests/renderer/fixed and runs
```

The script takes the compile command from the Windows build (`ninja -t
commands`), so it runs in the shell that build is made in. It runs the
program for a driver without extensions, for one with the extensions of
1999 (none of them allowed by `debug.gl1_extensions`, multitexture alone,
all), for OpenGL 1.5 with four texture units, with 32-bit textures, with
`"simple"`, and with `debug.gpu_stats`. Each run fails unless:

- every OpenGL call is one OpenGL 1.1 has, or belongs to an extension the
  driver lists and the setting allows; textures are powers of two, within
  the driver's size, with complete mip chains where a mip filter is set;
- every draw has its arrays enabled and readable for every vertex its
  indices name, the indices are the game's own and inside the range the
  vertex program was run for, nothing but the positions is enabled when
  arrays are locked, and nothing is left locked;
- a 640x480 screen rectangle through the HUD's program lands on the
  window's pixels the right way up, in windows of three sizes; a triangle
  in the world lands where the test's own arithmetic puts it, with the
  depth Direct3D gives it, and a second vertex program over the same
  vertices gives the same bits;
- the environment as the light pipeline draws it (lightmap times base map)
  is one pass with each texture on a unit and its own coordinates where
  the driver has two units, and two passes, the second multiplied in at
  equal depth, where it has one;
- for each of the game's pixel states (`combiner_ff_sites.c`) the passes,
  units, environments, constants, blends, alpha tests and depth functions
  are those of a plan `nv2a_combiner_ff.c` makes for the state, and every
  pass's primary colour is the plan's expression of the vertex's colours;
- clears, a draw that samples the frame, off-screen targets (a 64x64 one
  drawn in the window's corner and sampled from a copy when the window has
  alpha, left out otherwise, as a larger one is), visibility tests and the
  processor's floating-point state behave as the device says they do.

The environment's programs go through OpenGL's own transform (the
position rows as `GL_PROJECTION`, the game's streams as the arrays, the
affine texture coordinates through `GL_TEXTURE`): the recorder reads the
arrays by their types and applies the matrices (`rec_vertex_clip`,
`rec_vertex_coordinates`), and the checks of positions and coordinates go
through those.

`-v` prints the device's log as it is written.

## The light renderer's processor time (`t_bench.c`)

`t_bench.c` includes `t_fixed_device.c` (its recorder, with the draws
themselves replaced by counters) and makes a frame of about 650 draws of
the kinds the game's log shows over and over: the environment through
the light pipeline (lightmap x base map through vs16 over ranges, some
sparse, of a shared vertex buffer; a second pass over some; the fog pass),
skinned models with a palette of their own each, particles, decals and
HUD quads.

```sh
tools/t_bench.sh [folder] [frames]      # this processor's code, then -march=pentium3
```

It prints the frame's processor time (median, lowest, highest) for the
OpenGL 1.5, 1.2 and 1.1 drivers, and with `-s` the device's own split
(`debug.gpu_stats`, over its real clock here). `-sites` draws each of the
light pipeline's pixel states once through vs16 and once through vs10
with the frame traced, and prints what the device makes of each.

## The light renderer's pixel side (`combiner_ff_test.c`)

`port/linux/src/nv2a_combiner_ff.c` turns the Xbox's pixel state (texture
modes, register combiners, final combiner, alpha test, blend) into plans
OpenGL 1.1's texture environments can draw. The test is plain C with no
dependencies; any compiler builds it:

```sh
OUT=build/win9x/tests/renderer
clang -std=c89 -pedantic -Wall -Wextra -Wno-long-long -O2 -Iport/linux/src \
  port/win9x/tests/renderer/combiner_ff_test.c port/linux/src/nv2a_combiner_ff.c -o $OUT/combiner_ff_test.exe
$OUT/combiner_ff_test.exe --fuzz 3000 --details $OUT/combiner_ff_details.txt     # about two minutes
```

`combiner_ff_sites.c` is every place in `source/rasterizer/xbox` that
builds a pixel shader, written out again with the framebuffer state it
draws with (when the game's code changes there, this file has to follow).
For each of those states and each of five sets of capabilities the test
compiles a plan as the renderer does and draws thousands of random
fragments both ways: through a reference that computes what
`nv2a_psh.c`'s shader computes, and through a model of the fixed-function
pipeline running the plan. It fails when a plan called exact differs from
the reference by more than its tolerance, when nothing can be drawn for a
state of the game, when a plan asks for an environment, an operand or a
blend its capabilities lack (or counts on the format a texture is stored
in where it need not), and when a state of the light pipeline does not
come out as the plan its author built it for. `--fuzz` does the same with
random states. It also prints how long each compile took.
