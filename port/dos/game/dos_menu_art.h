/*
DOS_MENU_ART.H

Halo's own menu art (ui.map's bitmaps: the campaign's and the multiplayer
maps' pictures, the game types' and difficulties' icons, the controller
buttons, the panels and bars of its pages), read the first time ui.map is
up and kept as textures for the program's life: the menus (dos_menu.c) and
the loading screens (dos_render_game.c) draw them whatever map is loaded.
Drawn in the menus' 640x480 units, scaled to the picture.
*/

#ifndef __DOS_MENU_ART_H
#define __DOS_MENU_ART_H

enum dos_art
{
	/* ui\shell\bitmaps\sp_levels: the campaign's levels, a10 to d40 (10 a lock) */
	DOS_ART_LEVELS,
	/* mp_map_grafix: the multiplayer maps in Halo's Xbox order (dos_game.c's) */
	DOS_ART_MAPS,
	/* game_type_grafix: capture the flag, king, slayer, oddball, race, unknown */
	DOS_ART_GAME_TYPES,
	/* difficulty_select\difficulty_options: easy to legendary */
	DOS_ART_DIFFICULTIES,
	/* controller_graphix: none, one, none, then players 1 to 4, three each */
	DOS_ART_PADS,
	DOS_ART_BUTTON_A,
	DOS_ART_BUTTON_B,
	DOS_ART_BUTTON_X,
	DOS_ART_BUTTON_Y,
	/* list_field: a framed panel */
	DOS_ART_PANEL,
	/* alert_bkd: a framed panel with a band */
	DOS_ART_ALERT,
	/* split_screen_bkd: a framed panel */
	DOS_ART_SPLIT_PANEL,
	/* option_bkds: a row's bar, 0 dark, 1 framed */
	DOS_ART_ROW,
	/* arrow_sm_left, arrow_sm_right */
	DOS_ART_ARROW_LEFT,
	DOS_ART_ARROW_RIGHT,
	/* console_grafix: none, a console */
	DOS_ART_CONSOLE,
	DOS_ART_COUNT
};

/* the art read from the loaded map where it is not yet (ui.map: once);
looked for once a map (dos_art_new_map, when one is loaded) */
void dos_art_load(void);
void dos_art_new_map(void);
/* whether a picture is there */
int dos_art_has(enum dos_art art, int index);
/* a picture's part (its texels x0, y0 to x1, y1; NULL the box its visible
texels fill) drawn in a rectangle of the screen, its colour times color
(0xFFFFFFFF as it is: the alpha fades it) */
void dos_art_draw(enum dos_art art, int index, const int *crop, float x0, float y0, float x1, float y1,
	unsigned long color);
/* a framed picture scaled in nine parts: its corners border texels big
drawn border units big, its sides and middle stretched */
void dos_art_frame(enum dos_art art, int index, const int *crop, int border_texels, float border, float x0, float y0,
	float x1, float y1, unsigned long color);

/* the campaign's and the maps' pictures' crop (the rest of each bitmap is
opaque padding) */
extern const int dos_art_picture_crop[4];

#endif /* __DOS_MENU_ART_H */
