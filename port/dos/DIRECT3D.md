# Direct3D renderer: Halo's own renderer on Direct3D 8

**Set aside (2026-10-27).** The user wanted a Direct3D 8 renderer in the
SETTINGS > RENDERER row of the Windows 95 build, beside SOFTWARE and
OPENGL, as close to the Xbox's picture as Direct3D 8 allows, not Halo's
own renderer on an emulated Xbox device. That renderer is
`port/dos/render/r_d3d.c` with `port/dos/win95/w95_d3d.c` (the setting
renderer = direct3d). What follows, and HALOD3D.EXE (`ninja ... d3d`),
stays on disk unused; nothing of it is in HALO.EXE or HALO95.EXE.

The plan for the Windows 95 build's third renderer (SETTINGS, RENDERER:
DIRECT3D), asked for on 2026-10-27: the picture as close to the original
game's as it can be, by running Halo's own rendering code rather than this
port's.

## What it is

Halo on the Xbox draws through the Xbox's own Direct3D 8: its rasterizer
(`chupathingyce/source/rasterizer/xbox/*`, `source/render/*`) sets textures,
render states, NV2A vertex programs and register-combiner pixel shaders, and
draws. ChupathingyCE runs that code unchanged on PCs by implementing the
Xbox Direct3D API on OpenGL 4.5 (`port/linux/src/d3d8_gl.c`, ~55 entry
points). This renderer implements the same API on the PC's Direct3D 8
(DirectX 8.0a, the last DirectX for Windows 95), so everything Halo draws -
its shaders, passes, fog, lights, decals, particles, screen effects - is
drawn the way the game asks.

Choices made with the user: Halo's own renderer (not a Direct3D backend for
the port's renderer); Windows 95/98-era hardware (`d3d8.dll` loaded at run
time, so the program still starts without DirectX 8); offered from
HALO95.EXE. It also runs on later Windows (Windows 10/11 still ship a
32-bit `d3d8.dll`), which is where the hidden tests run.

## How it fits the build

The DOS and Windows 95 programs share one set of game objects built with
Halo's renderer left out and the port's own renderer in its place, and with
visual work the port's renderer never shows (lights, particle systems,
contrails, detail objects) cut out of the game (`tools/dos_hooks.py`). Halo's
renderer needs all of it. So the Direct3D renderer is a third program from
the same tree:

- `HALOD3D.EXE`: the game compiled a second time (`-DHALO_D3D`, objects in
  `build/dos/obj-d3d`), the trims of `dos_hooks.py` that only serve the
  port's renderer skipped under `HALO_D3D`; Halo's `source/render` and
  `source/rasterizer` (with `xbox/`) and the texture cache compiled in; the
  port's own 3D renderer (`port/dos/render`, `dos_render_game.c`,
  `dos_level.c`, `dos_models.c`, ...) left out; everything else of the
  port kept - the Windows 95 platform layer (window, input, controllers,
  sound, sockets), the C runtime, the menu, split screen, bots, Firefight,
  online play.
- HALO95.EXE's RENDERER row gains DIRECT3D. Renderers already change from
  the next start: at start HALO95.EXE with `renderer = direct3d` starts
  HALOD3D.EXE with the same command line and ends, and HALOD3D.EXE with any
  other renderer starts HALO95.EXE. One settings file, maps, cache and
  saves; to the player it is one program.
- The port's menu, loading screen and FPS counter draw through a small 2D
  layer on the Direct3D device (rectangles, pictures, Halo's menu art);
  their text is Halo's own text renderer.

## The device (`port/dos/d3d/`)

- `d3d8.dll` by `LoadLibrary`, `Direct3DCreate8(D3D_SDK_VERSION)`, a device
  on the game's window (window or full screen as the DISPLAY setting),
  `D3DPOOL_MANAGED` resources so a lost device (Alt+Tab out of full screen)
  only needs a `Reset`.
- The Xbox Direct3D state is kept as ChupathingyCE's `d3d8_gl.c` keeps it
  (the XDK's inline functions write `D3D__RenderState` and friends); at each
  draw it is translated, as there, but into Direct3D 8 calls.
- Textures: the Xbox's swizzled and compressed formats converted
  (`port/support/xbox_textures_convert.c`); DXT1/3/5 kept compressed where
  the card has them. Render targets (shadows, glows, screen effects, active
  camouflage, water reflections) as Direct3D render-target textures.
- Two paths for the shaders, chosen from the card's caps:
  - **fixed**, for every DirectX 8 card (GeForce 256/2/4 MX, Radeon 7000,
    Voodoo 4/5...): the vertex programs run on the processor (the user's
    `halo-ce-universal/port/linux/src/nv2a_vsh_cpu.c`, proven in the
    Windows 98 port's light renderer) and the register combiners become
    texture-stage passes (`nv2a_combiner_ff.c`'s plans, there drawn with
    OpenGL 1.1's texture environments, here with Direct3D 8's texture
    stages, which have more of the operations the plans need);
  - **shaders**, for GeForce 3/4 Ti and Radeon 8500 class cards: the NV2A
    vertex programs translated to `vs.1.1` (the GeForce 3's vertex
    programs are the NV2A's own kind; `nv2a_vsh.c`'s decoder) and the
    combiners to `ps.1.1`-`ps.1.3`, as Direct3D 8 shader tokens written
    directly (no D3DX). Software vertex processing runs `vs.1.1` on cards
    without it.
- What Direct3D 8 lacks: occlusion queries (the lens flares' visibility
  tests: answered from the depth buffer read back, or always visible),
  some NV2A combiner features (approximated, logged once).

## Testing

Hidden windows on this PC (Windows 11's `d3d8.dll`), screenshots read back
from the back buffer, compared frame for frame with ChupathingyCE (the
reference: the same game code drawing on OpenGL 4.5) at the same camera
(`-camera`). The user tests real Windows 95/98 hardware.

## Milestones

1. **Bring-up**: `d3d8.h`; HALOD3D.EXE builds and links; the device made on
   the window; clears, presentation, the screen-space draws of the menu and
   HUD (text, rectangles).
2. **The world**: textures, vertex/index buffers, the fixed path's CPU
   vertex programs and combiner passes: the level, objects, first person,
   HUD in a game; compared with ChupathingyCE.

3. **Effects**: render targets (shadows, screen effects, active camouflage,
   water), fog, decals, particles, lens flares.
4. **Shaders**: the `vs.1.1`/`ps.1.x` path for GeForce 3-class cards.
5. **Finish**: the port's menu/loading/FPS on Direct3D, full screen and
   lost devices, speed, settings and README, the real-machine zip.

## State (2026-10-27)

The first picture: `ninja -f build/dos/build.ninja d3d` makes HALOD3D.EXE
(imports still only GDI32, KERNEL32, USER32, WINMM and WSOCK32), which
draws the title (ui.map: the sky, Threshold, the Ring and the menu's text)
with Halo's own renderer at a steady 30 frames a second, at the same camera
as ChupathingyCE frame for frame. What is there:

- `pcd3d.h`: the two halves' plain-C seam (the PC's Direct3D 8 numbers).
- `pc/pcd3d.c`: the device on the window (`system_display_window`), back
  buffer the window's size and format, D24S8 (or the best depth/stencil
  the card has), hardware or software vertex processing; a nine-move
  `vs.1.1` written as tokens hands on clip-space positions, both colours,
  the fog factor and four coordinate sets; `D3DPOOL_MANAGED` A8R8G8B8
  textures, A8R8G8B8 target textures, `CopyRects` for the screen's copy;
  states cached; `DrawPrimitiveUP`/`DrawIndexedPrimitiveUP`; read-back of
  the lockable back buffer for screenshots.
- `xbox/xd3d_resources.c`: resources after ChupathingyCE's
  `d3d8_resources.c`, Data a plain pointer; `Lock` holds a serial that
  changes at `Register` and every lock (the PC's copy is remade then).
- `xbox/xd3d_textures.c`: every Xbox format to ARGB through
  `xbox_textures_convert.c`; levels over the card's size dropped; linear
  textures padded on cards that want powers of two; targets by Data; the
  back buffer, sampled, copied when it has changed.
- `xbox/xd3d_device.c`: the state, vertex programs (`nv2a_vsh_cpu.c`, the
  position captured before the program's screen mapping and handed to
  Direct3D's viewport), plans (`nv2a_combiner_ff.c`) per pixel state, each
  pass's primary/secondary colours and fog factor worked out per vertex;
  immediate mode, clears (an alpha-only clear is left out), presentation,
  a vertical blank of 60 a second polled from the game's sleeps and the
  presentation (one thread, as everywhere in this port); the menu's
  rectangles and pictures drawn over the frame (`xd3d_overlay_*`).
- `game/d3d_game.c`, `game/d3d_glue.c`: what the left-out units gave
  (`SetRenderStateSmart`, `rasterizer_debug_initialize` and
  `editor_preprocess_rendered_object` must answer TRUE, or the rasterizer
  fails to start and no object is drawn); the port's renderer glue as the
  Xbox's renderer would answer it.
- Settings for testing: `screenshot_every`, `screenshot_frame`,
  `quit_frame`, `d3d_trace_frame=<n>` (every draw of frame n, its passes,
  and the visibility's clusters and objects, in HALO.LOG).
- The game state is 9 MB here (6 MB in the DOS and Windows 95 builds), the
  texture cache the Xbox's 22 MB (`tools/dos_hooks.py` d3d_texture_cache).

Open: the Ring's hull is brighter and greener than ChupathingyCE's (a
pass of the model shader approximated or left out: check the plans'
approximations); visibility tests answer 0 (no lens flares); the menu's
art and logo, loading screens and the HALO95 hand-off; a stage's scale on
ADD, single TFACTOR per pass; derived textures (bakes) off.
