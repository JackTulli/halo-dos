HALO FOR MS-DOS AND WINDOWS 95                                  version %VERSION%
==============================================================================

Halo: Combat Evolved for MS-DOS and Windows 95, on ChupathingyCE's game code.
https://github.com/JackTulli/halo-dos

This is a fan project. It is not made, endorsed or supported by Microsoft,
Bungie or 343 Industries. Halo is a trademark of Microsoft. No part of the
game's own data (maps, sounds, pictures) comes with it: you need your own
copy of Halo: Combat Evolved for the original Xbox.


WHAT IS IN THIS FOLDER
----------------------

  HALO.EXE         MS-DOS (the DOS/32A extender is built in: nothing else
                   is needed)
  HALO95.EXE       Windows 95, 98 and Me (it runs on later Windows too)
  PLAY.BAT         runs HALO.EXE in DOS again when the menu asks for a
                   restart (APPLY AND RESTART)
  RUN-DOS.BAT      runs HALO.EXE in DOSBox-X on a Windows PC
  RUN-DOS-NET.BAT  the same, set up for hosting LAN games from DOSBox-X
  RUN-WIN95.BAT    runs HALO95.EXE (and asks once for your maps folder)
  MAPS\            your Xbox maps go here (empty)
  MAPS_CE\         Halo Custom Edition's maps go here (empty; optional)
  LICENSE.TXT      the GNU General Public License, version 3
  THIRDPTY.TXT     the licences of the parts by other people

Both programs read the same MAPS folder and share the map cache (CACHE\,
made at the first start), so they can live in one folder.


1. THE MAPS
-----------

The game plays the maps of Halo: Combat Evolved for the original Xbox: the
menu (ui.map), the 13 multiplayer maps and the 10 campaign levels, 24 files
in the "maps" folder of the game's disc. Any region works.

  - From a disc image (.iso / .xiso) of your Xbox disc: open it with an
    Xbox image tool (extract-xiso, XDVDFS tools, or similar) and copy the
    maps folder's .map files into MAPS here.
  - If you play ChupathingyCE, its "maps" folder already holds the same
    files: copy them, or point RUN-WIN95.BAT / RUN-DOS.BAT at that folder
    when they ask (they link it here as MAPS; nothing is copied).

Halo PC (retail) maps are not the Xbox maps and do not work in MAPS.

Halo Custom Edition (optional, for Custom Edition servers and CE maps):
copy Halo Custom Edition's "maps" folder into MAPS_CE (at least
bitmaps.map, sounds.map and loc.map, which every CE map needs, and the CE
maps you want). A CE server's map that is not there is downloaded when you
join it (see ONLINE PLAY).

The first time a map is played it is unpacked into CACHE\MAPS: about 35 MB
for the menu, 40-50 MB per multiplayer map, about 210 MB for a30 (Firefight
takes its Covenant from it) and 100-270 MB per campaign level. Later starts
use the unpacked copy.


2. WHAT IT NEEDS
----------------

  - A Pentium (133 MHz and up; the faster the better). It was made for
    machines from a Pentium 133 to about 1 GHz; slow machines play best at
    320x200 in 256 colours with the detail turned down.
  - Memory: 64 MB is what the tests ran with. Less has not been tried.
  - Disk: about 2 GB for the maps, plus the map cache (above).
  - Sound (optional): a Sound Blaster or compatible on DOS; any sound card
    Windows has on Windows 95.


3. MS-DOS (HALO.EXE)
--------------------

  - Plain DOS: MS-DOS 6.22 or 7.x, or Windows 95/98's "Restart in MS-DOS
    mode" (HIMEM.SYS and EMM386 may stay loaded). So far it has run in
    DOSBox-X; real DOS machines are the least tried part of this release.
  - Start it with PLAY (PLAY.BAT): then the menu's APPLY AND RESTART can
    start it again with new settings. HALO alone works too.
  - Long file names are not needed: maps unpacked without them (BLOODGUL.MAP)
    or copied from Windows (BLOODG~1.MAP) are found under those names.
  - Mouse: load the DOS mouse driver first (MOUSE.COM, CTMOUSE...).
  - Picture: 320x200 in 256 colours (VGA) by default. Bigger pictures and
    more colours need a VESA 2.0 card with a linear frame buffer (on older
    cards, UniVBE / SciTech Display Doctor). SETTINGS > DISPLAY.
  - Settings: HALODOS.CFG, written beside HALO.EXE the first time. DOS
    keeps only 126 characters of a command line: put longer option lists in
    HALODOS.CFG.
  - Network: load your network card's packet driver before HALO.EXE (the
    game finds it at interrupt 0x60 to 0x80, or set net_packet_int in
    HALODOS.CFG). The address comes from DHCP; without a DHCP server set
    net_address, net_netmask, net_gateway and net_dns in HALODOS.CFG. When
    the network starts the game asks pool.ntp.org for the time (DOS keeps no
    time zone, and the online game list needs the right time); if that
    fails, set time_zone in HALODOS.CFG to your hours from UTC (-5 New York
    in winter, 1 Berlin).
  - Sound: a Sound Blaster 16 plays 16-bit stereo, an older Sound Blaster
    (2.0, Pro) 8-bit mono. The card is found by the BLASTER variable its
    setup puts in AUTOEXEC.BAT (SET BLASTER=A220 I5 D1 H5 T6); without it
    the usual ports are tried. If it is not found, put the same text in
    HALODOS.CFG as sound_blaster = "A220 I5 D1 H5". HALO.LOG says what was
    found ("sound: Sound Blaster DSP 4.05 at 220 ...").
  - 3dfx Voodoo: SETTINGS > DISPLAY > RENDERER: 3DFX VOODOO lets a Voodoo
    Graphics card draw everything at 640x480 (through its pass-through
    cable). It has only been tried on DOSBox-X's emulated Voodoo, not on a
    real card. If a real card does not come up, two settings in HALODOS.CFG
    may help: voodoo_clock = 45 (a slower graphics clock in MHz, 16 to 60;
    50 by default) or voodoo_init = 0. A Voodoo 2 is not driven yet. If the
    screen stays dark, quit with Escape and set renderer = software in
    HALODOS.CFG.
  - Joysticks: the game port's (the sound card's 15-pin port). One
    joystick, or two (joysticks A and B: two players in split screen).
    With a four-axis pad (a Gravis GamePad Pro in analogue mode) set
    game_port = 4axis in HALODOS.CFG.

  DOSBox-X on a Windows PC: RUN-DOS.BAT finds DOSBox-X (the DOSBOX_X
  variable, a DOSBox-X folder beside it, or the installer's C:\DOSBox-X)
  and runs HALO.EXE as fast as the PC can emulate; "RUN-DOS 133" runs it as
  a 133 MHz Pentium. For the network in DOSBox-X, unpack DOSBox-X's MinGW
  portable zip (dosbox-x-mingw64-...-portable.zip from its releases) into a
  folder named DOSBox-X here: that build has the "slirp" network, and the
  launcher then gives the DOS machine an NE2000 card and loads its packet
  driver. RUN-DOS-NET HOST hosts a game for the PCs of your LAN.


4. WINDOWS 95 / 98 / ME (HALO95.EXE)
------------------------------------

  - Run RUN-WIN95.BAT, or double-click HALO95.EXE. Alt+Enter switches
    between a window and the whole screen.
  - It needs Windows' TCP/IP (WSOCK32.DLL), even to play alone. Windows 98
    and Me have it; on Windows 95 add it if the game will not start:
    Control Panel, Network, Add, Protocol, Microsoft, TCP/IP.
  - SETTINGS > DISPLAY > RENDERER:
      SOFTWARE     the processor draws, as on DOS
      OPENGL       the graphics card draws through its OpenGL driver
                   (Windows 95 has OpenGL from OSR2, or with Microsoft's
                   OpenGL 95 package)
      DIRECT3D 8   the graphics card draws through DirectX 8.0a or later,
                   and can draw as the Xbox did (below)
    The card renderers draw at the window's own resolution, in full colour,
    with smooth texture filtering. Without a working OpenGL or DirectX 8 the
    game stays with SOFTWARE (HALO.LOG says why).
  - DIRECT3D 8's Xbox look (SETTINGS > GRAPHICS, shown with that renderer):
    LIGHTMAPS XBOX (the level lit by Halo's own lightmaps), OBJECT LIGHT
    XBOX (objects and the weapon in your hands lit from the light under
    them) and SHADERS XBOX (the ground's detail maps and the skies' layers
    mixed on the card each frame, as Halo's own texture pass did). SHADERS
    XBOX needs a card with three texture units or more (GeForce 3, Radeon
    8500 and later); others bake the detail into the textures.
  - DIRECT3D 8's water is Halo's own: its ripples (four layers of ripple
    maps, moving), the reflection of its cube map in them, its tints, on a
    card with pixel shaders. Pixel shaders 1.4 (Radeon 8500 and later)
    tint every pixel by how squarely its ripple is seen, after ATI's ocean
    water program; pixel shaders 1.1 to 1.3 (GeForce 3 and 4 Ti) draw it as
    the Xbox did, one tint a draw. Setting d3d_water: 2 (the best the card
    has), 1 (the Xbox's way), 0 (as OPENGL draws it).
  - DIRECT3D 8's fog is Halo's own: each sky's atmospheric fog (its colour,
    distances and maximum density; all of it in the draw distance's last
    stretch) and the planar fog of water seen from above,
    which hides what is under the surface by its depth (The Silent
    Cartographer's sea). Setting d3d_fog 0: a fade into the sky's colour.
  - SETTINGS > DISPLAY: WINDOW, FULL SCREEN (at the desktop's resolution)
    or EXCLUSIVE (the screen switched to a mode near the picture's size:
    the fastest on an old PC); SCALING: SHARP, FILL or STRETCH. RESOLUTION
    is the picture's own size: the smaller, the faster (320x240 or 640x480
    for a real 95/98 machine).
  - Wide screens: pick a wide RESOLUTION (1280x720, 1920x1080...) and the
    view shows more to the sides, the HUD keeps to the corners and the
    menus are centred. ASPECT: AUTO, or 4:3, 16:10, 16:9, 21:9 for a
    monitor that stretches the picture (DOS too).
  - Game controllers, up to four: Xbox pads or any XInput controller where
    Windows has XInput (XP and later), and on every Windows any controller
    in Control Panel, Game Controllers. They only count while Halo's window
    is in front.
  - Settings: HALO.CFG, written beside HALO95.EXE the first time.


5. PLAYING
----------

  The menu: up/down to choose, left/right to change, Enter to select,
  Escape to go back. The mouse works too: point, click, right-click to go
  back, the wheel to scroll. With a controller: the D-pad or left stick, A
  to select, B to go back.

    CAMPAIGN      the ten levels, from The Pillar of Autumn to The Maw
    MULTIPLAYER   ONLINE        quick join, browse games, host a game
                  SPLIT SCREEN  two to four players on one screen
                  CUSTOM GAME   any map and game type, with bots
                  FIREFIGHT     waves of Covenant on a multiplayer map
    SETTINGS      PLAYER, DISPLAY, GRAPHICS, SOUND, CONTROLS, CONTROLLER

  Settings that only take effect when the game starts (the renderer, the
  resolution, the colours, the aspect, double wide/tall, texture quality,
  sound on/off) are marked * when changed; APPLY AND RESTART saves them and
  starts the game again (on DOS, when it was started with PLAY).

  The keys at first (SETTINGS > CONTROLS changes them):

    W A S D             move              mouse               aim
    left button, shift  fire              right button, G     grenade
    space, enter        jump              F, backspace        melee
    E                   action            R                   reload
    tab, mouse wheel    switch weapon     X                   switch grenades
    left ctrl, C        crouch            Z, middle button    zoom
    Q                   flashlight        F1                  scores
    arrows              turn (no mouse)   escape              pause menu

  SETTINGS > CONTROLS: every action takes up to three keys, mouse buttons
  or wheel turns. Choose an action and press Enter, then the key (Escape
  gives up, Delete clears it). Also the mouse speed, invert, and AIM
  ASSIST (off by default: on, the view's magnetism and the bullets' autoaim
  help every player, as the console game's do).

  SETTINGS > CONTROLLER: the original game's controller settings: BUTTON
  LAYOUT (DEFAULT, SOUTHPAW, JUMPY, BOXER, GREEN THUMB, BUMPER JUMPER),
  THUMBSTICK LAYOUT, LOOK SENSITIVITY (1 to 10; 3 is Halo's), INVERT LOOK,
  AIM ASSIST, AIRCRAFT CONTROLS and VIBRATION (Windows).

  SPLIT SCREEN: each player joins with a device of their own: A (or Start)
  on a controller, Enter on the keyboard (the keyboard and mouse are one
  player's). Left/right changes a player's armour, up/down their team. With
  two or more in, go on to ONLINE (all of you in one online game), CUSTOM
  GAME, FIREFIGHT or CAMPAIGN CO-OP (two players, as Halo's).

  SETTINGS > GRAPHICS: DETAIL, DRAW DISTANCE, FIELD OF VIEW (50 to 120; 70
  is Halo's), TEXTURE QUALITY (LOWEST to ORIGINAL), TEXTURE FILTER, LEVEL
  LIGHT, VIEW MODEL (the weapon in your hands with fewer triangles is
  faster), PARTICLES and the SKY. SETTINGS > DISPLAY also has the FRAME RATE
  (30 as Halo; 60, 120 or UNCAPPED draw frames between the game's 30 ticks
  a second) and an FPS counter. For slow machines with the software
  renderer, DOUBLE WIDE and DOUBLE TALL draw each pixel twice as wide or
  tall (320x200 drawn as 160x200, or 160x100 with both).


6. ONLINE PLAY
--------------

  MULTIPLAYER > ONLINE:

    QUICK JOIN     joins the best game found (the most players short of
                   full, then the shortest answer time)
    BROWSE GAMES   the list: games on your LAN, the online list's games
                   (ChupathingyCE's and OpenCE's: their players and you can
                   join each other) and Halo Custom Edition servers. Left/
                   right picks a tab (ALL, ONLINE, CHUPATHINGY, OPEN CE,
                   CUSTOM EDITION, LAN), Tab the sort, F the filters (full,
                   empty, joinable), Q quick join, R refresh.
    HOST A GAME    hosts a game: on the LAN, and on the online list unless
                   network.host_public = false in the settings file
    PLAYERS        who plays from this machine (split screen online)

  A hosted game's invite link is written to INVITE.TXT. To join a game by
  its link, put the link in JOIN.TXT beside the program before starting it.

  Halo Custom Edition servers: the game joins CE 1.10 servers as a CE
  client. It needs Custom Edition's bitmaps.map, sounds.map and loc.map in
  MAPS_CE. A server's map that is not in MAPS_CE is downloaded from the map
  repository Chimera uses (maps.halonet.net) when you join: the JOINING page
  shows the download and unpacking, and CANCEL (Escape) gives up. Settings:
  map_download = 0 turns downloading off. Servers that check CD keys refuse
  this game.


7. IF SOMETHING GOES WRONG
--------------------------

  The program writes HALO.LOG (and debug.txt) beside itself. A crash leaves
  the place it happened at the end of HALO.LOG. Report problems on GitHub
  (https://github.com/JackTulli/halo-dos/issues) with HALO.LOG, which
  program (DOS or Windows), the map, and the machine.

  - "There are no maps": put the Xbox maps in MAPS (section 1).
  - Windows 95: the game does not start at all: add TCP/IP (section 4).
  - DOS: no sound: set sound_blaster in HALODOS.CFG (section 3).
  - A setting made the picture unusable: delete HALO.CFG / HALODOS.CFG
    (the game writes a new one with the defaults).


8. CREDITS AND LICENCES
-----------------------

  The game code is ChupathingyCE's (github.com/ChupathingyCE/chupathingyce,
  GPL-3.0), which grew from halo-ce-universal. This port (DOS, Windows 95,
  the renderers, the Sound Blaster and Voodoo drivers, the Custom Edition
  client) is GPL-3.0 too. The source is at github.com/JackTulli/halo-dos.

  This product uses DOS/32 Advanced DOS Extender technology.

  Also in these programs: mTCP by Michael B. Brutman (GPL-3.0), Zstandard
  (BSD), musl's maths (MIT), PDCLib (CC0), KCP (MIT), Monocypher (CC0 /
  BSD-2-Clause) and Direct3D 8's water program after Mahmoud Galal's
  Irrlicht_Water_Shader (MIT). THIRDPTY.TXT has their licences.
