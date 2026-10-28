/*
DOS_PROMPTS.C

Game functions whose owners the DOS and Windows 95 builds leave out (the
menus' ui_widget.c, the PC ports' keyboard layer) but whose answers the HUD
needs, so a stand-in's zero will not do (tools/dos_stubs.py leaves names
defined here alone):

- fast_ftol_C, Halo's rounding of a real to a whole number, which the HUD's
  meters (shield, health, ammunition, heat) turn their values into bytes
  with: as zero every meter read empty;
- the keyboard's actions (halo_keyboard.h): the keys and mouse buttons
  bound to them (the CONTROLS page: halo_bindings.h), which the game takes
  as its first controller's player's (input_abstraction.c), and the
  prompts: the game's button prompts ("press %action to pick up") show the
  first key bound, "[E]", as the PC ports' do, after the keyboard or mouse
  was used last (after a controller, its buttons);
- draw_string_and_hack_in_icons, the text with button icons of the carnage
  report's prompts and the scenario's custom HUD messages: icons are written
  as their keys, or the button's name where no key drives it.
*/

#include "cseries.h"

#include "input/input_abstraction.h"
#include "interface/hud_draw.h"
#include "interface/ui_widget.h"
#include "main/console.h"
#include "rasterizer/rasterizer.h"
#include "text/draw_string.h"

#include "halo_keyboard.h"
#include "render/render.h"
#include "halo_bindings.h"
#include "dos_split.h"

#include <string.h>
#include <wchar.h>

/* ---------- rounding */

long fast_ftol_C(real value)
{
	return fast_ftol(value);
}

/* ---------- the keyboard's prompts */

/* our actions (halo_bindings.h) for halo_keyboard.h's */
static int bound_action(int action)
{
	switch (action)
	{
	case HALO_KEYBOARD_MOVE_FORWARD: return DOS_ACTION_FORWARD;
	case HALO_KEYBOARD_MOVE_BACKWARD: return DOS_ACTION_BACK;
	case HALO_KEYBOARD_STRAFE_LEFT: return DOS_ACTION_LEFT;
	case HALO_KEYBOARD_STRAFE_RIGHT: return DOS_ACTION_RIGHT;
	case HALO_KEYBOARD_JUMP: return DOS_ACTION_JUMP;
	case HALO_KEYBOARD_CROUCH: return DOS_ACTION_CROUCH;
	case HALO_KEYBOARD_FIRE: return DOS_ACTION_FIRE;
	case HALO_KEYBOARD_THROW_GRENADE: return DOS_ACTION_GRENADE;
	case HALO_KEYBOARD_MELEE: return DOS_ACTION_MELEE;
	case HALO_KEYBOARD_RELOAD: return DOS_ACTION_RELOAD;
	case HALO_KEYBOARD_ZOOM: return DOS_ACTION_ZOOM;
	case HALO_KEYBOARD_SWITCH_WEAPON: return DOS_ACTION_WEAPON;
	case HALO_KEYBOARD_SWITCH_GRENADE: return DOS_ACTION_GRENADES;
	case HALO_KEYBOARD_ACTION: return DOS_ACTION_ACTION;
	case HALO_KEYBOARD_FLASHLIGHT: return DOS_ACTION_FLASHLIGHT;
	case HALO_KEYBOARD_SCOREBOARD: return DOS_ACTION_SCORES;
	default: return -1;
	}
}

/* the actions held, for the first controller's player; none while a menu
or the console has the keyboard */
unsigned long halo_keyboard_actions(short controller_index)
{
	extern int dos_menu_has_input(void);

	/* (the keyboard's player: the first, or split screen's) */
	if (controller_index != dos_split_keyboard_port() || dos_menu_has_input() || console_is_active())
		return 0;
	return dos_bindings_keyboard_actions();
}

int halo_keyboard_prompts(void)
{
	return dos_keyboard_used_at() >= dos_pad_used_at();
}

int halo_keyboard_action_shown(int action, char *shown, unsigned long size)
{
	char const *key = NULL;
	int bound = bound_action(action);

	if (!shown || size < 2)
		return FALSE;
	if (action == HALO_KEYBOARD_PAUSE)
		key = "ESC";
	else if (bound >= 0 && dos_binding(bound, 0) != BIND_NONE)
		key = dos_binding_name(dos_binding(bound, 0));
	if (!key)
		return FALSE;
	strncpy(shown, key, size - 1);
	shown[size - 1] = 0;
	return TRUE;
}

/* ---------- text with icons */

/* ui_widget.c's icon names, in its icon types' order */
static wchar_t const *const icon_names[] =
{
	L"a-button", L"b-button", L"x-button", L"y-button", L"black-button", L"white-button",
	L"left-trigger", L"right-trigger", L"dpad-up", L"dpad-down", L"dpad-left", L"dpad-right",
	L"start-button", L"back-button", L"left-thumb", L"right-thumb", L"left-stick", L"right-stick",
	L"action", L"throw-grenade", L"primary-trigger", L"integrated-light", L"jump", L"use-equipment",
	L"rotate-weapons", L"rotate-grenades", L"crouch", L"zoom", L"accept", L"back", L"move", L"look",
	L"custom-1", L"custom-2", L"custom-3", L"custom-4", L"custom-5", L"custom-6", L"custom-7", L"custom-8"
};

/* the words for icons no key stands for: the buttons' own names */
static wchar_t const *const icon_words[] =
{
	L"A", L"B", L"X", L"Y", L"Black", L"White", L"Left Trigger", L"Right Trigger", L"Up", L"Down",
	L"Left", L"Right", L"Start", L"Back", L"Left Stick", L"Right Stick", L"Left Stick", L"Right Stick"
};

enum
{
	ICON_LOOK = 31
};

short draw_string_and_hack_in_icons_end_x = 0;

static short icon_type(wchar_t const *string)
{
	for (short index = 0; index < (short)NUMBEROF(icon_names); index++)
	{
		size_t length = wcslen(icon_names[index]);
		size_t at;

		for (at = 0; at < length; at++)
		{
			wchar_t c = string[at];

			if (c >= L'A' && c <= L'Z')
				c += L'a' - L'A';
			if (c != icon_names[index][at])
				break;
		}
		if (at == length)
			return index;
	}
	return NONE;
}

void draw_string_and_hack_in_icons(
	rectangle2d *bounds,
	rectangle2d *clip,
	point2d *cursor_reference,
	short height_adjust,
	wchar_t const *instring,
	boolean ignore_icon_color)
{
	wchar_t text[1024];
	long length = 0;
	rectangle2d text_bounds, cursor_bounds;

	(void)ignore_icon_color;
	for (wchar_t const *at = instring; at && *at && length < (long)NUMBEROF(text) - 1;)
	{
		short icon = *at == L'%' ? icon_type(at + 1) : NONE;
		wchar_t key[48];
		wchar_t const *word = NULL;

		if (icon == NONE)
		{
			text[length++] = *at++;
			continue;
		}
		at += 1 + wcslen(icon_names[icon]);
		/* (the keys of the player whose view this is: split screen's
		controller players see the buttons) */
		if (icon <= ICON_LOOK &&
			input_abstraction_port_icon_key(render.local_player_index, icon, FALSE, key, NUMBEROF(key)))
			word = key;
		else if (icon < (short)NUMBEROF(icon_words))
			word = icon_words[icon];
		for (; word && *word && length < (long)NUMBEROF(text) - 1; word++)
			text[length++] = *word;
	}
	text[length] = 0;
	rasterizer_draw_unicode_string(bounds, clip, cursor_reference, height_adjust, text);
	draw_unicode_string_compute_bounds(bounds, text, &text_bounds, &cursor_bounds);
	draw_string_and_hack_in_icons_end_x = cursor_bounds.x0;
}
