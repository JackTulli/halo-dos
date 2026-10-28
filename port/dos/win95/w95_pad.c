/*
W95_PAD.C

Game controllers on Windows (halo_system.h's pads), up to four, two ways,
neither of which Windows 95 must have:
	- XInput (xinput1_4.dll, xinput1_3.dll or xinput9_1_0.dll, looked for
	  when the game starts, not imported: a Windows without them starts all
	  the same): Xbox controllers or any XInput pads, their triggers apart
	  and their motors (system_pad_rumble);
	- the joystick API Windows 95 has had from the start (WINMM's
	  joyGetPosEx), for every other controller Windows knows: its first two
	  axes the left stick, U and R (or Z and R) the right one, Z the
	  triggers where U is there, the hat the D-pad, buttons 1-10 as an Xbox
	  pad's (A, B, X, Y, the bumpers, Back, Start, the sticks' clicks). An
	  XInput pad is a joystick to WINMM too, named only as Windows' "Microsoft
	  PC-joystick driver": while XInput has pads, as many joysticks are left
	  out, those shaped as an XInput pad first (read twice, the joystick's
	  copy overwrote the pad's sticks every frame).
The controllers are looked for once a second, XInput's first, each keeping
its id (its kind and number) while it stays plugged in, so a split screen's
player keeps theirs (port/dos/game/dos_split.c). They count only while
Halo's window is in front (XInput answers whichever program asks: a pad
used in another program would play this one too), and their motors stop
when Halo goes behind. The setting controller = 0 turns it all off.
*/

#include <windows.h>
#include <mmsystem.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "halo_system.h"

long dos_setting_integer(const char *name, long default_value);

/* XInput's as Windows defines them (these builds compile with the Xbox
SDK's, which differ) */
struct xinput_gamepad
{
	WORD buttons;
	BYTE left_trigger, right_trigger;
	SHORT left_x, left_y, right_x, right_y;
};

struct xinput_state
{
	DWORD packet;
	struct xinput_gamepad gamepad;
};

struct xinput_vibration
{
	WORD left, right;
};

typedef DWORD (WINAPI *xinput_get_state_function)(DWORD user, struct xinput_state *state);
typedef DWORD (WINAPI *xinput_set_state_function)(DWORD user, struct xinput_vibration *vibration);

#define JOYSTICKS 16

static struct
{
	int initialized, enabled, in_front;
	HMODULE xinput;
	xinput_get_state_function get_state;
	xinput_set_state_function set_state;
	DWORD next_look;
	/* the controllers found, in order: their ids and names */
	int count;
	unsigned short ids[SYSTEM_PADS];
	char names[SYSTEM_PADS][40];
	/* each joystick's capabilities, as found */
	JOYCAPSA caps[JOYSTICKS];
	/* each XInput user's motors, as last set */
	WORD rumble[4][2];
	char description[96];
} pad = { 0 };

static void pad_initialize(void)
{
	static const char *const libraries[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };

	pad.initialized = TRUE;
	pad.enabled = dos_setting_integer("controller", 1) != 0;
	if (!pad.enabled)
		return;
	{
		void w95_pad_close(void);

		atexit(w95_pad_close);
	}
	for (int i = 0; i < (int)(sizeof(libraries) / sizeof(libraries[0])) && !pad.xinput; i++)
	{
		pad.xinput = LoadLibraryA(libraries[i]);
		if (pad.xinput)
		{
			pad.get_state = (xinput_get_state_function)GetProcAddress(pad.xinput, "XInputGetState");
			pad.set_state = (xinput_set_state_function)GetProcAddress(pad.xinput, "XInputSetState");
			if (!pad.get_state)
			{
				FreeLibrary(pad.xinput);
				pad.xinput = NULL;
			}
			else
			{
				char line[64];

				snprintf(line, sizeof(line), "controller: XInput from %s", libraries[i]);
				system_log(line);
			}
		}
	}
}

static void upper_copy(char *out, int size, const char *text)
{
	int i;

	for (i = 0; text[i] && i < size - 1; i++)
		out[i] = (char)toupper((unsigned char)text[i]);
	out[i] = 0;
}

/* an XInput pad's joystick, to WINMM (left to XInput): named so, or shaped
as Windows' XInput-compatible HID device (X and Y the left stick, Z the
triggers, U and R the right stick, the hat, ten buttons) */
static int xinput_joystick(const JOYCAPSA *caps)
{
	UINT axes = JOYCAPS_HASZ | JOYCAPS_HASR | JOYCAPS_HASU | JOYCAPS_HASV;
	char upper[40];

	upper_copy(upper, sizeof(upper), caps->szPname);
	if (strstr(upper, "XBOX") || strstr(upper, "XINPUT"))
		return TRUE;
	return (caps->wCaps & axes) == (JOYCAPS_HASZ | JOYCAPS_HASR | JOYCAPS_HASU) && (caps->wCaps & JOYCAPS_HASPOV) &&
		caps->wNumButtons == 10;
}

/* the controllers there: XInput's users, then the joysticks; logged when
they change */
static void pad_look(void)
{
	unsigned short ids[SYSTEM_PADS];
	char names[SYSTEM_PADS][40];
	int count = 0, xinput_pads, left_out;
	unsigned joysticks, found[JOYSTICKS], found_count = 0;
	unsigned char skipped[JOYSTICKS];

	if (pad.get_state)
	{
		for (int user = 0; user < 4 && count < SYSTEM_PADS; user++)
		{
			struct xinput_state state;

			if (pad.get_state((DWORD)user, &state) == ERROR_SUCCESS)
			{
				ids[count] = (unsigned short)(SYSTEM_PAD_XINPUT | user);
				snprintf(names[count], sizeof(names[count]), "XBOX CONTROLLER %d", user + 1);
				count++;
			}
		}
	}
	xinput_pads = count;
	/* the joysticks there */
	joysticks = joyGetNumDevs();
	for (unsigned id = 0; id < joysticks && id < JOYSTICKS; id++)
	{
		JOYINFOEX info;

		memset(&info, 0, sizeof(info));
		info.dwSize = sizeof(info);
		info.dwFlags = JOY_RETURNALL;
		if (joyGetPosEx(id, &info) != JOYERR_NOERROR ||
			joyGetDevCapsA(id, &pad.caps[id], sizeof(pad.caps[id])) != JOYERR_NOERROR)
		{
			continue;
		}
		skipped[found_count] = FALSE;
		found[found_count++] = id;
	}
	/* XInput's pads among them, left to XInput: as many as it has, those
	shaped as its pads first, then any (an XInput pad WINMM shows another
	way) */
	left_out = 0;
	for (unsigned pass = 0; pass < 2; pass++)
	{
		for (unsigned i = 0; i < found_count && left_out < xinput_pads; i++)
		{
			if (!skipped[i] && (pass || xinput_joystick(&pad.caps[found[i]])))
			{
				skipped[i] = TRUE;
				left_out++;
			}
		}
	}
	for (unsigned i = 0; i < found_count && count < SYSTEM_PADS; i++)
	{
		unsigned id = found[i];

		if (skipped[i])
			continue;
		ids[count] = (unsigned short)(SYSTEM_PAD_JOYSTICK | id);
		upper_copy(names[count], sizeof(names[count]), pad.caps[id].szPname[0] ? pad.caps[id].szPname : "JOYSTICK");
		count++;
	}
	if (count != pad.count || memcmp(ids, pad.ids, sizeof(ids[0]) * (size_t)count))
	{
		char line[160];

		pad.description[0] = 0;
		if (left_out)
		{
			snprintf(line, sizeof(line), "controller: %d joystick%s left out (XInput's pad%s, as Windows' joystick%s)",
				left_out, left_out == 1 ? "" : "s", left_out == 1 ? "" : "s", left_out == 1 ? "" : "s");
			system_log(line);
		}
		for (int i = 0; i < count; i++)
		{
			snprintf(line, sizeof(line), "controller: %s", names[i]);
			system_log(line);
			if (strlen(pad.description) + strlen(names[i]) + 3 < sizeof(pad.description))
			{
				if (i)
					strcat(pad.description, ", ");
				strcat(pad.description, names[i]);
			}
		}
		if (!count && pad.count)
			system_log("controller: none");
	}
	pad.count = count;
	memcpy(pad.ids, ids, sizeof(ids));
	memcpy(pad.names, names, sizeof(names));
}

static short axis(DWORD value, UINT minimum, UINT maximum, int flip)
{
	long range = (long)maximum - (long)minimum, centred;

	if (range <= 0)
		return 0;
	centred = ((long)value - (long)minimum) * 65535 / range - 32768;
	if (flip)
		centred = -centred - 1;
	return (short)(centred < -32768 ? -32768 : centred > 32767 ? 32767 : centred);
}

static int read_joystick(unsigned id, struct system_pad *out)
{
	static const unsigned short buttons[10] =
	{
		SYSTEM_PAD_A, SYSTEM_PAD_B, SYSTEM_PAD_X, SYSTEM_PAD_Y, SYSTEM_PAD_LEFT_BUMPER, SYSTEM_PAD_RIGHT_BUMPER,
		SYSTEM_PAD_BACK, SYSTEM_PAD_START, SYSTEM_PAD_LEFT_THUMB, SYSTEM_PAD_RIGHT_THUMB
	};
	const JOYCAPSA *caps = &pad.caps[id];
	UINT capabilities = caps->wCaps;
	JOYINFOEX info;

	memset(&info, 0, sizeof(info));
	info.dwSize = sizeof(info);
	info.dwFlags = JOY_RETURNALL;
	if (joyGetPosEx(id, &info) != JOYERR_NOERROR)
		return FALSE;
	out->left_x = axis(info.dwXpos, caps->wXmin, caps->wXmax, FALSE);
	out->left_y = axis(info.dwYpos, caps->wYmin, caps->wYmax, TRUE);
	if ((capabilities & JOYCAPS_HASU) && (capabilities & JOYCAPS_HASR))
	{
		out->right_x = axis(info.dwUpos, caps->wUmin, caps->wUmax, FALSE);
		out->right_y = axis(info.dwRpos, caps->wRmin, caps->wRmax, TRUE);
		if (capabilities & JOYCAPS_HASZ)
		{
			/* (a pad's triggers, one axis: the left pushes it up) */
			short z = axis(info.dwZpos, caps->wZmin, caps->wZmax, FALSE);

			if (z > 0)
				out->left_trigger = (unsigned char)(z >> 7);
			else if (z < 0)
				out->right_trigger = (unsigned char)((-(long)z) >> 7 > 255 ? 255 : (-(long)z) >> 7);
		}
	}
	else if ((capabilities & JOYCAPS_HASZ) && (capabilities & JOYCAPS_HASR))
	{
		out->right_x = axis(info.dwZpos, caps->wZmin, caps->wZmax, FALSE);
		out->right_y = axis(info.dwRpos, caps->wRmin, caps->wRmax, TRUE);
	}
	if ((capabilities & JOYCAPS_HASPOV) && info.dwPOV != 0xFFFF && info.dwPOV < 36000)
	{
		DWORD angle = info.dwPOV;

		if (angle >= 31500 || angle <= 4500) out->buttons |= SYSTEM_PAD_UP;
		if (angle >= 4500 && angle <= 13500) out->buttons |= SYSTEM_PAD_RIGHT;
		if (angle >= 13500 && angle <= 22500) out->buttons |= SYSTEM_PAD_DOWN;
		if (angle >= 22500 && angle <= 31500) out->buttons |= SYSTEM_PAD_LEFT;
	}
	for (int i = 0; i < 10; i++)
	{
		if (info.dwButtons & (1UL << i))
			out->buttons |= buttons[i];
	}
	return TRUE;
}

static void motors(int user, WORD left, WORD right)
{
	struct xinput_vibration vibration;

	pad.rumble[user][0] = left;
	pad.rumble[user][1] = right;
	vibration.left = left;
	vibration.right = right;
	pad.set_state((DWORD)user, &vibration);
}

static void motors_off(void)
{
	for (int user = 0; user < 4 && pad.set_state; user++)
	{
		if (pad.rumble[user][0] || pad.rumble[user][1])
			motors(user, 0, 0);
	}
}

void w95_pad_read(struct system_input *input, int in_front)
{
	DWORD now;
	int lost = FALSE;

	input->pad_count = 0;
	memset(input->pads, 0, sizeof(input->pads));
	if (!pad.initialized)
		pad_initialize();
	if (!pad.enabled)
		return;
	if (!in_front)
	{
		if (pad.in_front)
			motors_off();
		pad.in_front = FALSE;
		return;
	}
	pad.in_front = TRUE;
	now = GetTickCount();
	if ((long)(now - pad.next_look) >= 0)
	{
		pad.next_look = now + 1000;
		pad_look();
	}
	for (int i = 0; i < pad.count; i++)
	{
		struct system_pad *out = &input->pads[input->pad_count];
		unsigned short id = pad.ids[i];

		out->id = id;
		if ((id & 0xFF00) == SYSTEM_PAD_XINPUT)
		{
			struct xinput_state state;

			if (pad.get_state((DWORD)(id & 0xFF), &state) != ERROR_SUCCESS)
			{
				memset(out, 0, sizeof(*out));
				lost = TRUE;
				continue;
			}
			out->buttons = state.gamepad.buttons;
			out->left_x = state.gamepad.left_x;
			out->left_y = state.gamepad.left_y;
			out->right_x = state.gamepad.right_x;
			out->right_y = state.gamepad.right_y;
			out->left_trigger = state.gamepad.left_trigger;
			out->right_trigger = state.gamepad.right_trigger;
		}
		else if (!read_joystick(id & 0xFF, out))
		{
			memset(out, 0, sizeof(*out));
			lost = TRUE;
			continue;
		}
		input->pad_count++;
	}
	/* (one gone: looked for again at once) */
	if (lost)
		pad.next_look = now;
}

void system_pad_rumble(unsigned short id, unsigned short left, unsigned short right)
{
	int user = id & 0xFF;

	if ((id & 0xFF00) != SYSTEM_PAD_XINPUT || user >= 4 || !pad.set_state || !pad.in_front ||
		(left == pad.rumble[user][0] && right == pad.rumble[user][1]))
	{
		return;
	}
	motors(user, left, right);
}

const char *system_pad_name(unsigned short id)
{
	for (int i = 0; i < pad.count; i++)
	{
		if (pad.ids[i] == id)
			return pad.names[i];
	}
	return "NONE";
}

const char *system_pad_description(void)
{
	if (!pad.initialized)
		return "NONE";
	if (!pad.enabled)
		return "OFF";
	return pad.count ? pad.description : "NONE";
}

/* the motors stopped at the end (an XInput pad keeps the last speed it was
given) */
void w95_pad_close(void)
{
	motors_off();
}
