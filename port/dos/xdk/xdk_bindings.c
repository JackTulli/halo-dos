/*
XDK_BINDINGS.C

The keyboard's and the mouse's controls (halo_bindings.h): each action's
keys, read from the settings file (bind_<action> = LEFT_SHIFT,MOUSE_LEFT;
read again when the settings change) or the defaults, the PC port's layout:

	W A S D       move                 mouse            aim
	space, enter  jump                 left ctrl, C     crouch
	mouse left,   fire                 mouse right, G   throw a grenade
	  shifts
	F, backspace  melee                E                action
	R             reload               Z, mouse middle  zoom
	tab, wheel    switch weapon        X                switch grenades
	Q             flashlight           F1               scores
	arrows        look (no mouse)

A wheel turn holds its binding for a moment (long enough for the game to
see one press). Escape stays the pause and the menus' back, Enter and the
arrows their keys: those are not bindings.
*/

#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "halo_bindings.h"

const char *dos_setting(const char *name, const char *default_value);
void dos_setting_set(const char *name, const char *value);
unsigned long dos_settings_generation(void);

/* halo_keyboard.h's actions (port/linux/include/halo_keyboard.h) */
enum
{
	KEYBOARD_MOVE_FORWARD,
	KEYBOARD_MOVE_BACKWARD,
	KEYBOARD_STRAFE_LEFT,
	KEYBOARD_STRAFE_RIGHT,
	KEYBOARD_JUMP,
	KEYBOARD_CROUCH,
	KEYBOARD_FIRE,
	KEYBOARD_THROW_GRENADE,
	KEYBOARD_MELEE,
	KEYBOARD_RELOAD,
	KEYBOARD_ZOOM,
	KEYBOARD_SWITCH_WEAPON,
	KEYBOARD_SWITCH_GRENADE,
	KEYBOARD_ACTION,
	KEYBOARD_FLASHLIGHT,
	KEYBOARD_SCOREBOARD,
	KEYBOARD_PAUSE
};

static const struct
{
	const char *id, *name;
	int keyboard;
	int defaults[BIND_SLOTS];
} actions[DOS_ACTION_COUNT] =
{
	{ "forward", "MOVE FORWARD", KEYBOARD_MOVE_FORWARD, { SYSTEM_KEY_W } },
	{ "back", "MOVE BACK", KEYBOARD_MOVE_BACKWARD, { SYSTEM_KEY_S } },
	{ "left", "STRAFE LEFT", KEYBOARD_STRAFE_LEFT, { SYSTEM_KEY_A } },
	{ "right", "STRAFE RIGHT", KEYBOARD_STRAFE_RIGHT, { SYSTEM_KEY_D } },
	{ "jump", "JUMP", KEYBOARD_JUMP, { SYSTEM_KEY_SPACE, SYSTEM_KEY_ENTER } },
	{ "crouch", "CROUCH", KEYBOARD_CROUCH, { SYSTEM_KEY_LEFT_CONTROL, SYSTEM_KEY_C } },
	{ "fire", "FIRE", KEYBOARD_FIRE, { BIND_MOUSE_LEFT, SYSTEM_KEY_LEFT_SHIFT, SYSTEM_KEY_RIGHT_SHIFT } },
	{ "grenade", "THROW GRENADE", KEYBOARD_THROW_GRENADE, { BIND_MOUSE_RIGHT, SYSTEM_KEY_G } },
	{ "melee", "MELEE", KEYBOARD_MELEE, { SYSTEM_KEY_F, SYSTEM_KEY_BACKSPACE } },
	{ "action", "ACTION", KEYBOARD_ACTION, { SYSTEM_KEY_E } },
	{ "reload", "RELOAD", KEYBOARD_RELOAD, { SYSTEM_KEY_R } },
	{ "zoom", "ZOOM", KEYBOARD_ZOOM, { SYSTEM_KEY_Z, BIND_MOUSE_MIDDLE } },
	{ "weapon", "SWITCH WEAPON", KEYBOARD_SWITCH_WEAPON, { SYSTEM_KEY_TAB, BIND_WHEEL_UP, BIND_WHEEL_DOWN } },
	{ "grenades", "SWITCH GRENADES", KEYBOARD_SWITCH_GRENADE, { SYSTEM_KEY_X } },
	{ "flashlight", "FLASHLIGHT", KEYBOARD_FLASHLIGHT, { SYSTEM_KEY_Q } },
	{ "scores", "SCORES", KEYBOARD_SCOREBOARD, { SYSTEM_KEY_F1 } },
	{ "look_up", "LOOK UP", -1, { SYSTEM_KEY_UP } },
	{ "look_down", "LOOK DOWN", -1, { SYSTEM_KEY_DOWN } },
	{ "look_left", "LOOK LEFT", -1, { SYSTEM_KEY_LEFT } },
	{ "look_right", "LOOK RIGHT", -1, { SYSTEM_KEY_RIGHT } },
};

/* the keys' names (the settings file's, with _ for the menu's spaces) */
static const struct
{
	int code;
	const char *name;
} names[] =
{
	{ 0x01, "ESCAPE" }, { 0x02, "1" }, { 0x03, "2" }, { 0x04, "3" }, { 0x05, "4" }, { 0x06, "5" }, { 0x07, "6" },
	{ 0x08, "7" }, { 0x09, "8" }, { 0x0A, "9" }, { 0x0B, "0" }, { 0x0C, "MINUS" }, { 0x0D, "EQUALS" },
	{ 0x0E, "BACKSPACE" }, { 0x0F, "TAB" }, { 0x10, "Q" }, { 0x11, "W" }, { 0x12, "E" }, { 0x13, "R" }, { 0x14, "T" },
	{ 0x15, "Y" }, { 0x16, "U" }, { 0x17, "I" }, { 0x18, "O" }, { 0x19, "P" }, { 0x1A, "LEFT_BRACKET" },
	{ 0x1B, "RIGHT_BRACKET" }, { 0x1C, "ENTER" }, { 0x1D, "LEFT_CTRL" }, { 0x1E, "A" }, { 0x1F, "S" }, { 0x20, "D" },
	{ 0x21, "F" }, { 0x22, "G" }, { 0x23, "H" }, { 0x24, "J" }, { 0x25, "K" }, { 0x26, "L" }, { 0x27, "SEMICOLON" },
	{ 0x28, "APOSTROPHE" }, { 0x29, "BACKQUOTE" }, { 0x2A, "LEFT_SHIFT" }, { 0x2B, "BACKSLASH" }, { 0x2C, "Z" },
	{ 0x2D, "X" }, { 0x2E, "C" }, { 0x2F, "V" }, { 0x30, "B" }, { 0x31, "N" }, { 0x32, "M" }, { 0x33, "COMMA" },
	{ 0x34, "PERIOD" }, { 0x35, "SLASH" }, { 0x36, "RIGHT_SHIFT" }, { 0x37, "KEYPAD_STAR" }, { 0x38, "LEFT_ALT" },
	{ 0x39, "SPACE" }, { 0x3A, "CAPS_LOCK" }, { 0x3B, "F1" }, { 0x3C, "F2" }, { 0x3D, "F3" }, { 0x3E, "F4" },
	{ 0x3F, "F5" }, { 0x40, "F6" }, { 0x41, "F7" }, { 0x42, "F8" }, { 0x43, "F9" }, { 0x44, "F10" },
	{ 0x45, "NUM_LOCK" }, { 0x46, "SCROLL_LOCK" }, { 0x47, "KEYPAD_7" }, { 0x48, "KEYPAD_8" }, { 0x49, "KEYPAD_9" },
	{ 0x4A, "KEYPAD_MINUS" }, { 0x4B, "KEYPAD_4" }, { 0x4C, "KEYPAD_5" }, { 0x4D, "KEYPAD_6" },
	{ 0x4E, "KEYPAD_PLUS" }, { 0x4F, "KEYPAD_1" }, { 0x50, "KEYPAD_2" }, { 0x51, "KEYPAD_3" }, { 0x52, "KEYPAD_0" },
	{ 0x53, "KEYPAD_PERIOD" }, { 0x57, "F11" }, { 0x58, "F12" }, { 0x80 | 0x1C, "KEYPAD_ENTER" },
	{ 0x80 | 0x1D, "RIGHT_CTRL" }, { 0x80 | 0x35, "KEYPAD_SLASH" }, { 0x80 | 0x38, "RIGHT_ALT" },
	{ 0x80 | 0x47, "HOME" }, { 0x80 | 0x48, "UP" }, { 0x80 | 0x49, "PAGE_UP" }, { 0x80 | 0x4B, "LEFT" },
	{ 0x80 | 0x4D, "RIGHT" }, { 0x80 | 0x4F, "END" }, { 0x80 | 0x50, "DOWN" }, { 0x80 | 0x51, "PAGE_DOWN" },
	{ 0x80 | 0x52, "INSERT" }, { 0x80 | 0x53, "DELETE" }, { BIND_MOUSE_LEFT, "MOUSE_LEFT" },
	{ BIND_MOUSE_RIGHT, "MOUSE_RIGHT" }, { BIND_MOUSE_MIDDLE, "MOUSE_MIDDLE" }, { BIND_WHEEL_UP, "WHEEL_UP" },
	{ BIND_WHEEL_DOWN, "WHEEL_DOWN" },
};

/* how long a wheel turn holds its binding (microseconds) */
#define WHEEL_HOLD 120000ULL

static struct
{
	unsigned long generation;
	int loaded;
	int codes[DOS_ACTION_COUNT][BIND_SLOTS];
	/* what is held now and was held at the last read */
	unsigned char held[BIND_CODES], previous[BIND_CODES];
	unsigned long long wheel_up_until, wheel_down_until;
	int pressed;
	unsigned long long keyboard_used_at, pad_used_at;
} b;

static int code_of(const char *name, size_t length)
{
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
	{
		if (strlen(names[i].name) == length && !_strnicmp(names[i].name, name, length))
			return names[i].code;
	}
	return BIND_NONE;
}

static const char *raw_name(int code)
{
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
	{
		if (names[i].code == code)
			return names[i].name;
	}
	return NULL;
}

const char *dos_binding_name(int code)
{
	static char buffers[4][24];
	static int next;
	const char *name = raw_name(code);
	char *out = buffers[next++ & 3];
	int i;

	if (!name)
	{
		snprintf(out, 24, "KEY %d", code);
		return out;
	}
	for (i = 0; name[i] && i < 23; i++)
		out[i] = name[i] == '_' ? ' ' : name[i];
	out[i] = 0;
	return out;
}

const char *dos_action_name(int action)
{
	return action >= 0 && action < DOS_ACTION_COUNT ? actions[action].name : "";
}

/* the bindings as the settings say (or the defaults) */
static void load(void)
{
	unsigned long generation = dos_settings_generation();

	if (b.loaded && generation == b.generation)
		return;
	b.loaded = TRUE;
	b.generation = generation;
	for (int action = 0; action < DOS_ACTION_COUNT; action++)
	{
		char key[32];
		const char *value;

		snprintf(key, sizeof(key), "bind_%s", actions[action].id);
		value = dos_setting(key, NULL);
		memset(b.codes[action], 0, sizeof(b.codes[action]));
		if (!value)
		{
			memcpy(b.codes[action], actions[action].defaults, sizeof(b.codes[action]));
			continue;
		}
		/* (a list of names, commas between; NONE or nothing: unbound) */
		for (int slot = 0; *value && slot < BIND_SLOTS;)
		{
			size_t length = 0;
			int code;

			while (*value == ' ' || *value == ',' || *value == '"')
				value++;
			while (value[length] && value[length] != ',' && value[length] != ' ' && value[length] != '"')
				length++;
			if (!length)
				break;
			code = code_of(value, length);
			if (code != BIND_NONE)
				b.codes[action][slot++] = code;
			value += length;
		}
	}
}

static void save(int action)
{
	char key[32], value[96] = "";

	snprintf(key, sizeof(key), "bind_%s", actions[action].id);
	for (int slot = 0; slot < BIND_SLOTS; slot++)
	{
		const char *name = raw_name(b.codes[action][slot]);

		if (!name)
			continue;
		if (value[0])
			strncat(value, ",", sizeof(value) - strlen(value) - 1);
		strncat(value, name, sizeof(value) - strlen(value) - 1);
	}
	dos_setting_set(key, value[0] ? value : "NONE");
	/* (the settings' generation moved: what was just set is what it says) */
	b.generation = dos_settings_generation();
}

int dos_binding(int action, int slot)
{
	load();
	if (action < 0 || action >= DOS_ACTION_COUNT || slot < 0 || slot >= BIND_SLOTS)
		return BIND_NONE;
	return b.codes[action][slot];
}

void dos_binding_assign(int action, int code)
{
	load();
	if (action < 0 || action >= DOS_ACTION_COUNT || code <= BIND_NONE || code >= BIND_CODES)
		return;
	/* (one action a key: taken from any other) */
	for (int other = 0; other < DOS_ACTION_COUNT; other++)
	{
		int kept = 0, changed = FALSE;

		for (int slot = 0; slot < BIND_SLOTS; slot++)
		{
			if (b.codes[other][slot] == code)
				changed = TRUE;
			else
				b.codes[other][kept++] = b.codes[other][slot];
		}
		while (kept < BIND_SLOTS)
			b.codes[other][kept++] = BIND_NONE;
		if (changed && other != action)
			save(other);
	}
	memmove(&b.codes[action][1], &b.codes[action][0], (BIND_SLOTS - 1) * sizeof(int));
	b.codes[action][0] = code;
	save(action);
}

void dos_binding_clear(int action)
{
	load();
	if (action < 0 || action >= DOS_ACTION_COUNT)
		return;
	memset(b.codes[action], 0, sizeof(b.codes[action]));
	save(action);
}

void dos_bindings_reset(void)
{
	load();
	for (int action = 0; action < DOS_ACTION_COUNT; action++)
	{
		memcpy(b.codes[action], actions[action].defaults, sizeof(b.codes[action]));
		save(action);
	}
}

void dos_bindings_update(const struct system_input *input)
{
	unsigned long long now = system_microseconds();
	int used = input->mouse_dx || input->mouse_dy || input->mouse_wheel;

	load();
	memcpy(b.previous, b.held, sizeof(b.held));
	for (int key = 1; key < SYSTEM_KEY_COUNT; key++)
	{
		b.held[key] = input->keys[key] != 0;
		used |= b.held[key];
	}
	if (input->mouse_wheel > 0)
		b.wheel_up_until = now + WHEEL_HOLD;
	if (input->mouse_wheel < 0)
		b.wheel_down_until = now + WHEEL_HOLD;
	b.held[BIND_MOUSE_LEFT] = (input->mouse_buttons & SYSTEM_MOUSE_LEFT) != 0;
	b.held[BIND_MOUSE_RIGHT] = (input->mouse_buttons & SYSTEM_MOUSE_RIGHT) != 0;
	b.held[BIND_MOUSE_MIDDLE] = (input->mouse_buttons & SYSTEM_MOUSE_MIDDLE) != 0;
	b.held[BIND_WHEEL_UP] = now < b.wheel_up_until;
	b.held[BIND_WHEEL_DOWN] = now < b.wheel_down_until;
	{
		/* (testing, setting test_hold=W,MOUSE_LEFT: those held all along) */
		static const char *hold;

		if (!hold)
			hold = dos_setting("test_hold", "");
		for (const char *at = hold; *at;)
		{
			size_t length = 0;
			int code;

			while (at[length] && at[length] != ',')
				length++;
			code = code_of(at, length);
			if (code != BIND_NONE)
				b.held[code] = 1;
			at += at[length] ? length + 1 : length;
		}
	}
	{
		/* (testing, setting test_tap=Z@230,Z@400: each key held for four
		frames from the frame given) */
		extern long dos_frame_number(void);
		static const char *tap;
		long frame = dos_frame_number();

		if (!tap)
			tap = dos_setting("test_tap", "");
		for (const char *at = tap; *at;)
		{
			size_t length = 0, name = 0;
			long from;
			int code;

			while (at[length] && at[length] != ',')
				length++;
			while (name < length && at[name] != '@')
				name++;
			code = code_of(at, name);
			from = name < length ? atol(at + name + 1) : 0;
			if (code != BIND_NONE && frame >= from && frame < from + 4)
				b.held[code] = 1;
			at += at[length] ? length + 1 : length;
		}
	}
	used |= input->mouse_buttons != 0;
	if (used)
		b.keyboard_used_at = now;
	b.pressed = BIND_NONE;
	/* (a wheel turn counts as pressed on the read it came in) */
	if (input->mouse_wheel > 0)
		b.pressed = BIND_WHEEL_UP;
	else if (input->mouse_wheel < 0)
		b.pressed = BIND_WHEEL_DOWN;
	for (int code = 1; code < BIND_CODES && !b.pressed; code++)
	{
		if (code != BIND_WHEEL_UP && code != BIND_WHEEL_DOWN && b.held[code] && !b.previous[code])
			b.pressed = code;
	}
}

int dos_action_held(int action)
{
	load();
	if (action < 0 || action >= DOS_ACTION_COUNT)
		return FALSE;
	for (int slot = 0; slot < BIND_SLOTS; slot++)
	{
		int code = b.codes[action][slot];

		if (code > BIND_NONE && code < BIND_CODES && b.held[code])
			return TRUE;
	}
	return FALSE;
}

unsigned long dos_bindings_keyboard_actions(void)
{
	unsigned long held = 0;

	for (int action = 0; action < DOS_ACTION_COUNT; action++)
	{
		if (actions[action].keyboard >= 0 && dos_action_held(action))
			held |= 1UL << actions[action].keyboard;
	}
	return held;
}

int dos_binding_pressed(void)
{
	return b.pressed;
}

unsigned long long dos_keyboard_used_at(void)
{
	return b.keyboard_used_at;
}

unsigned long long dos_pad_used_at(void)
{
	return b.pad_used_at;
}

void dos_pad_used(void)
{
	b.pad_used_at = system_microseconds();
}
