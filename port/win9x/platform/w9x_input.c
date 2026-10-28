/*
W9X_INPUT.C

DirectInput on Windows 98/ME (see w9x_backend.h): the mouse while the game
has it, and gamepads, which go through winmm's joystick functions where
DirectInput is older than DirectX 5 or missing.

dinput.dll is loaded at run time and asked for the newest interface it
has: 7 (ME's), then 5, then 3 (the mouse only). This unit defines the
GUIDs it uses (INITGUID) and its own data formats, so nothing of dinput.lib
or dxguid.lib is linked: the formats are the standard mouse and joystick
ones (DIMOUSESTATE, DIJOYSTATE), with the object lists of dinput.lib's
c_dfDIMouse and c_dfDIJoystick.

Gamepads are looked for when the window opens and when Windows reports a
device change (DBT_DEVNODES_CHANGED, which 98 and ME send for USB), never
every frame: enumerating takes tens of milliseconds there. Each is read
once per pump, its axes scaled to -32768..32767, and mapped to the Xbox
controller by input.gamepad_map, in SDL's mapping syntax. Rumble goes to
pads with force feedback as a constant force on the X and Y axes, which the
dual-motor pads of the time took as their two motors; only changes are
sent, at most every 50 milliseconds (each is a driver call).
*/

#define INITGUID
#define DIRECTINPUT_VERSION 0x0700
#include "w9x_internal.h"

#include <dinput.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* dinput.lib's data formats mark the objects a device may lack */
#ifndef DIDFT_OPTIONAL
#define DIDFT_OPTIONAL 0x80000000
#endif

#define PAD_MAXIMUM 4
#define PAD_AXES 8 /* X Y Z Rx Ry Rz, sliders 0 and 1 */
#define PAD_BUTTONS 32
#define RUMBLE_INTERVAL_MS 50

typedef HRESULT (WINAPI *direct_input_create_function)(HINSTANCE instance, DWORD version, LPDIRECTINPUTA *result,
	LPUNKNOWN outer);

static CRITICAL_SECTION lock;
static HWND input_window;
static HMODULE dinput_library;
static LPDIRECTINPUTA direct_input;
static DWORD direct_input_version;
static BOOL direct_input_tried;
static LPDIRECTINPUTDEVICEA mouse;
static BOOL mouse_tried;

/* ---------- data formats */

#define AXIS(guid, offset) { &guid, offset, DIDFT_OPTIONAL | DIDFT_AXIS | DIDFT_ANYINSTANCE, DIDOI_ASPECTPOSITION }
#define POV(n) { &GUID_POV, DIJOFS_POV(n), DIDFT_OPTIONAL | DIDFT_POV | DIDFT_ANYINSTANCE, 0 }
#define BUTTON(n) { NULL, DIJOFS_BUTTON(n), DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 }

static DIOBJECTDATAFORMAT mouse_objects[] =
{
	{ &GUID_XAxis, DIMOFS_X, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 },
	{ &GUID_YAxis, DIMOFS_Y, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 },
	{ &GUID_ZAxis, DIMOFS_Z, DIDFT_OPTIONAL | DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 },
	{ NULL, DIMOFS_BUTTON0, DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
	{ NULL, DIMOFS_BUTTON1, DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
	{ NULL, DIMOFS_BUTTON2, DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
	{ NULL, DIMOFS_BUTTON3, DIDFT_OPTIONAL | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 },
};

static const DIDATAFORMAT mouse_format =
{
	sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_RELAXIS, sizeof(DIMOUSESTATE),
	sizeof(mouse_objects) / sizeof(mouse_objects[0]), mouse_objects
};

static DIOBJECTDATAFORMAT joystick_objects[] =
{
	AXIS(GUID_XAxis, DIJOFS_X), AXIS(GUID_YAxis, DIJOFS_Y), AXIS(GUID_ZAxis, DIJOFS_Z),
	AXIS(GUID_RxAxis, DIJOFS_RX), AXIS(GUID_RyAxis, DIJOFS_RY), AXIS(GUID_RzAxis, DIJOFS_RZ),
	AXIS(GUID_Slider, DIJOFS_SLIDER(0)), AXIS(GUID_Slider, DIJOFS_SLIDER(1)),
	POV(0), POV(1), POV(2), POV(3),
	BUTTON(0), BUTTON(1), BUTTON(2), BUTTON(3), BUTTON(4), BUTTON(5), BUTTON(6), BUTTON(7),
	BUTTON(8), BUTTON(9), BUTTON(10), BUTTON(11), BUTTON(12), BUTTON(13), BUTTON(14), BUTTON(15),
	BUTTON(16), BUTTON(17), BUTTON(18), BUTTON(19), BUTTON(20), BUTTON(21), BUTTON(22), BUTTON(23),
	BUTTON(24), BUTTON(25), BUTTON(26), BUTTON(27), BUTTON(28), BUTTON(29), BUTTON(30), BUTTON(31),
};

static const DIDATAFORMAT joystick_format =
{
	sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS, sizeof(DIJOYSTATE),
	sizeof(joystick_objects) / sizeof(joystick_objects[0]), joystick_objects
};

__attribute__((constructor))
static void input_startup(void)
{
	InitializeCriticalSection(&lock);
}

/* ---------- DirectInput */

static BOOL direct_input_start(void)
{
	static const DWORD versions[] = { 0x0700, 0x0500, 0x0300 };
	direct_input_create_function create;
	struct w9x_fpu fpu;
	int index;

	if (direct_input || direct_input_tried)
		return direct_input != NULL;
	direct_input_tried = TRUE;
	w9x_fpu_save(&fpu);
	dinput_library = LoadLibraryA("dinput.dll");
	w9x_fpu_check(&fpu, "LoadLibraryA(dinput.dll)");
	create = dinput_library ? (direct_input_create_function)GetProcAddress(dinput_library, "DirectInputCreateA") : NULL;
	if (!create)
	{
		platform_log("input: no DirectInput (the mouse is read through the cursor, joysticks through winmm)");
		return FALSE;
	}
	for (index = 0; index < (int)(sizeof(versions) / sizeof(versions[0])); index++)
	{
		HRESULT result;

		w9x_fpu_save(&fpu);
		result = create(w9x_instance(), versions[index], &direct_input, NULL);
		w9x_fpu_check(&fpu, "DirectInputCreateA");
		if (SUCCEEDED(result))
		{
			direct_input_version = versions[index];
			platform_log("input: DirectInput %lu.%lu", direct_input_version >> 8, direct_input_version & 0xff);
			return TRUE;
		}
		direct_input = NULL;
	}
	platform_log("input: DirectInput would not start");
	return FALSE;
}

/* ---------- the mouse */

static BOOL mouse_open(void)
{
	if (mouse)
		return TRUE;
	if (mouse_tried || !input_window || !direct_input_start())
		return FALSE;
	mouse_tried = TRUE;
	if (FAILED(direct_input->lpVtbl->CreateDevice(direct_input, &GUID_SysMouse, &mouse, NULL)))
	{
		mouse = NULL;
		return FALSE;
	}
	if (FAILED(mouse->lpVtbl->SetDataFormat(mouse, &mouse_format)) ||
		FAILED(mouse->lpVtbl->SetCooperativeLevel(mouse, input_window, DISCL_FOREGROUND | DISCL_EXCLUSIVE)))
	{
		mouse->lpVtbl->Release(mouse);
		mouse = NULL;
		return FALSE;
	}
	return TRUE;
}

int w9x_mouse_acquire(int acquire)
{
	if (!acquire)
	{
		if (mouse)
			mouse->lpVtbl->Unacquire(mouse);
		return TRUE;
	}
	if (!mouse_open())
		return FALSE;
	return SUCCEEDED(mouse->lpVtbl->Acquire(mouse));
}

int w9x_mouse_read(long *dx, long *dy, long *dz, unsigned char buttons[4])
{
	DIMOUSESTATE state;
	HRESULT result;
	int index;

	if (!mouse)
		return FALSE;
	result = mouse->lpVtbl->GetDeviceState(mouse, sizeof(state), &state);
	if (result == DIERR_INPUTLOST || result == DIERR_NOTACQUIRED)
	{
		/* (lost to another program, or to a mode change) */
		mouse->lpVtbl->Acquire(mouse);
		result = mouse->lpVtbl->GetDeviceState(mouse, sizeof(state), &state);
	}
	if (FAILED(result))
		return FALSE;
	*dx = state.lX;
	*dy = state.lY;
	*dz = state.lZ;
	for (index = 0; index < 4; index++)
		buttons[index] = (state.rgbButtons[index] & 0x80) != 0;
	return TRUE;
}

/* ---------- the gamepads' layout */

enum
{
	_map_a, _map_b, _map_x, _map_y, _map_left_shoulder, _map_right_shoulder,
	_map_left_trigger, _map_right_trigger, _map_back, _map_start, _map_left_stick, _map_right_stick,
	_map_dpad_up, _map_dpad_down, _map_dpad_left, _map_dpad_right,
	_map_left_x, _map_left_y, _map_right_x, _map_right_y,
	NUMBER_OF_MAP_CONTROLS
};

static const char *const map_names[NUMBER_OF_MAP_CONTROLS] =
{
	"a", "b", "x", "y", "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger", "back", "start",
	"leftstick", "rightstick", "dpup", "dpdown", "dpleft", "dpright", "leftx", "lefty", "rightx", "righty",
};

/* what a button reads as on the Xbox controller (the Duke's white and
black buttons sit where later pads have shoulders, as in xinput_sdl.c) */
static const unsigned long map_buttons[NUMBER_OF_MAP_CONTROLS] =
{
	W9X_PAD_A, W9X_PAD_B, W9X_PAD_X, W9X_PAD_Y, W9X_PAD_WHITE, W9X_PAD_BLACK, 0, 0, W9X_PAD_BACK, W9X_PAD_START,
	W9X_PAD_LEFT_THUMB, W9X_PAD_RIGHT_THUMB, W9X_PAD_DPAD_UP, W9X_PAD_DPAD_DOWN, W9X_PAD_DPAD_LEFT,
	W9X_PAD_DPAD_RIGHT, 0, 0, 0, 0,
};

struct binding
{
	/* 'b' button, 'a' axis, 'h' hat, 0 nothing */
	char kind;
	/* '+' or '-': half of the axis; 0 all of it */
	char half;
	char inverted;
	unsigned char index;
	/* the hat's direction: SDL's 1 up, 2 right, 4 down, 8 left */
	unsigned char hat_mask;
};

/* the common dual-analog layout of the time: buttons 1-4 the face buttons,
5-8 the shoulders and triggers, 9-10 select and start, 11-12 the sticks'
clicks, the right stick on Z and Rz */
static const char default_map[] =
	"a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,back:b8,start:b9,"
	"leftstick:b10,rightstick:b11,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"
	"leftx:a0,lefty:a1,rightx:a2,righty:a5";

static struct binding bindings[NUMBER_OF_MAP_CONTROLS];

static void map_parse(const char *text)
{
	memset(bindings, 0, sizeof(bindings));
	while (text && *text)
	{
		const char *end = strchr(text, ',');
		const char *colon = strchr(text, ':');
		int control;

		if (!end)
			end = text + strlen(text);
		/* (an SDL_GameControllerDB line's GUID and name have no colon) */
		if (colon && colon < end)
		{
			for (control = 0; control < NUMBER_OF_MAP_CONTROLS; control++)
			{
				size_t length = strlen(map_names[control]);

				if ((size_t)(colon - text) == length && !strncmp(text, map_names[control], length))
				{
					struct binding *binding = &bindings[control];
					const char *value = colon + 1;

					memset(binding, 0, sizeof(*binding));
					if (*value == '+' || *value == '-')
						binding->half = *value++;
					if (*value != 'a' && *value != 'b' && *value != 'h')
						break;
					binding->kind = *value++;
					binding->index = (unsigned char)atoi(value);
					while (*value >= '0' && *value <= '9')
						value++;
					if (binding->kind == 'h' && *value == '.')
						binding->hat_mask = (unsigned char)atoi(value + 1);
					while (value < end && *value != '~')
						value++;
					binding->inverted = value < end && *value == '~';
					break;
				}
			}
		}
		text = *end ? end + 1 : end;
	}
}

void w9x_input_configure(const char *mapping)
{
	EnterCriticalSection(&lock);
	map_parse(mapping && *mapping ? mapping : default_map);
	LeaveCriticalSection(&lock);
}

/* ---------- the gamepads */

struct pad
{
	char name[MAX_PATH];
	/* NULL for winmm's joystick joystick_id */
	LPDIRECTINPUTDEVICE2A device;
	UINT joystick_id;
	JOYCAPSA capabilities;
	LPDIRECTINPUTEFFECT rumble;
	DWORD rumble_axes[2];
	unsigned short rumble_left, rumble_right;
	DWORD rumble_sent;
	/* -32768 to 32767; buttons 0 or 1; the hat in hundredths of a degree
	clockwise from up, or -1 */
	long axes[PAD_AXES];
	unsigned char buttons[PAD_BUTTONS];
	long hat;
	struct w9x_pad_state state;
};

static struct pad pads[PAD_MAXIMUM];
static int pad_count;
static BOOL pads_wanted;

struct pad_found
{
	GUID instance;
	char name[MAX_PATH];
};

static struct pad_found pads_found[PAD_MAXIMUM];
static int pads_found_count;

static BOOL CALLBACK pad_enumerated(LPCDIDEVICEINSTANCEA device, LPVOID context)
{
	(void)context;
	if (pads_found_count == PAD_MAXIMUM)
		return DIENUM_STOP;
	pads_found[pads_found_count].instance = device->guidInstance;
	lstrcpynA(pads_found[pads_found_count].name, device->tszProductName, MAX_PATH);
	pads_found_count++;
	return DIENUM_CONTINUE;
}

static void pad_axis_range(LPDIRECTINPUTDEVICE2A device, DWORD offset)
{
	DIPROPRANGE range;

	range.diph.dwSize = sizeof(range);
	range.diph.dwHeaderSize = sizeof(range.diph);
	range.diph.dwObj = offset;
	range.diph.dwHow = DIPH_BYOFFSET;
	range.lMin = -32768;
	range.lMax = 32767;
	/* (fails for axes the pad lacks) */
	device->lpVtbl->SetProperty(device, DIPROP_RANGE, &range.diph);
}

static void pad_rumble_create(struct pad *pad)
{
	DIPROPDWORD centre;
	DICONSTANTFORCE force;
	LONG direction[2] = { 0, 0 };
	DIEFFECT_DX5 effect;

	centre.diph.dwSize = sizeof(centre);
	centre.diph.dwHeaderSize = sizeof(centre.diph);
	centre.diph.dwObj = 0;
	centre.diph.dwHow = DIPH_DEVICE;
	centre.dwData = DIPROPAUTOCENTER_OFF;
	pad->device->lpVtbl->SetProperty(pad->device, DIPROP_AUTOCENTER, &centre.diph);
	pad->rumble_axes[0] = DIJOFS_X;
	pad->rumble_axes[1] = DIJOFS_Y;
	force.lMagnitude = 0;
	/* DirectX 5's size: its runtime refuses DirectX 6's larger one */
	memset(&effect, 0, sizeof(effect));
	effect.dwSize = sizeof(effect);
	effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
	effect.dwDuration = INFINITE;
	effect.dwGain = DI_FFNOMINALMAX;
	effect.dwTriggerButton = DIEB_NOTRIGGER;
	effect.cAxes = 2;
	effect.rgdwAxes = pad->rumble_axes;
	effect.rglDirection = direction;
	effect.cbTypeSpecificParams = sizeof(force);
	effect.lpvTypeSpecificParams = &force;
	if (FAILED(pad->device->lpVtbl->CreateEffect(pad->device, &GUID_ConstantForce, (LPCDIEFFECT)&effect,
		&pad->rumble, NULL)))
	{
		pad->rumble = NULL;
	}
}

static void pads_close(void)
{
	int index;

	for (index = 0; index < pad_count; index++)
	{
		if (pads[index].rumble)
			pads[index].rumble->lpVtbl->Release(pads[index].rumble);
		if (pads[index].device)
		{
			pads[index].device->lpVtbl->Unacquire(pads[index].device);
			pads[index].device->lpVtbl->Release(pads[index].device);
		}
	}
	memset(pads, 0, sizeof(pads));
	pad_count = 0;
}

static BOOL pad_open_direct_input(struct pad *pad, const struct pad_found *found)
{
	LPDIRECTINPUTDEVICEA device;
	DIDEVCAPS capabilities;
	HRESULT result;
	BOOL force_feedback;
	int index;

	if (FAILED(direct_input->lpVtbl->CreateDevice(direct_input, &found->instance, &device, NULL)))
		return FALSE;
	/* Poll and the effects are DirectX 5's */
	result = device->lpVtbl->QueryInterface(device, &IID_IDirectInputDevice2A, (void **)&pad->device);
	device->lpVtbl->Release(device);
	if (FAILED(result))
	{
		pad->device = NULL;
		return FALSE;
	}
	if (FAILED(pad->device->lpVtbl->SetDataFormat(pad->device, &joystick_format)))
	{
		pad->device->lpVtbl->Release(pad->device);
		pad->device = NULL;
		return FALSE;
	}
	memset(&capabilities, 0, sizeof(capabilities));
	capabilities.dwSize = sizeof(capabilities);
	force_feedback = SUCCEEDED(pad->device->lpVtbl->GetCapabilities(pad->device, &capabilities)) &&
		(capabilities.dwFlags & DIDC_FORCEFEEDBACK);
	/* force feedback needs the device to itself; other pads share it */
	pad->device->lpVtbl->SetCooperativeLevel(pad->device, input_window,
		DISCL_FOREGROUND | (force_feedback ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE));
	pad_axis_range(pad->device, DIJOFS_X);
	pad_axis_range(pad->device, DIJOFS_Y);
	pad_axis_range(pad->device, DIJOFS_Z);
	pad_axis_range(pad->device, DIJOFS_RX);
	pad_axis_range(pad->device, DIJOFS_RY);
	pad_axis_range(pad->device, DIJOFS_RZ);
	pad_axis_range(pad->device, DIJOFS_SLIDER(0));
	pad_axis_range(pad->device, DIJOFS_SLIDER(1));
	if (force_feedback)
		pad_rumble_create(pad);
	pad->device->lpVtbl->Acquire(pad->device);
	lstrcpynA(pad->name, found->name, MAX_PATH);
	for (index = 0; index < PAD_AXES; index++)
		pad->axes[index] = 0;
	pad->hat = -1;
	return TRUE;
}

/* the attached pads, again */
static void pads_enumerate(void)
{
	struct w9x_fpu fpu;
	int index;

	pads_close();
	w9x_fpu_save(&fpu);
	if (direct_input_start() && direct_input_version >= 0x0500)
	{
		pads_found_count = 0;
		direct_input->lpVtbl->EnumDevices(direct_input, DIDEVTYPE_JOYSTICK, pad_enumerated, NULL,
			DIEDFL_ATTACHEDONLY);
		for (index = 0; index < pads_found_count; index++)
		{
			if (pad_open_direct_input(&pads[pad_count], &pads_found[index]))
			{
				platform_log("input: gamepad %d: %s (DirectInput%s)", pad_count + 1, pads[pad_count].name,
					pads[pad_count].rumble ? ", force feedback" : "");
				pad_count++;
			}
		}
	}
	else
	{
		/* any joystick the Game Controllers control panel knows */
		UINT count = joyGetNumDevs();
		UINT id;

		for (id = 0; id < count && pad_count < PAD_MAXIMUM; id++)
		{
			struct pad *pad = &pads[pad_count];
			JOYINFOEX state;

			memset(&state, 0, sizeof(state));
			state.dwSize = sizeof(state);
			state.dwFlags = JOY_RETURNALL;
			if (joyGetPosEx(id, &state) != JOYERR_NOERROR ||
				joyGetDevCapsA(id, &pad->capabilities, sizeof(pad->capabilities)) != JOYERR_NOERROR)
			{
				continue;
			}
			pad->joystick_id = id;
			pad->hat = -1;
			lstrcpynA(pad->name, pad->capabilities.szPname, MAX_PATH);
			platform_log("input: gamepad %d: %s (winmm)", pad_count + 1, pad->name);
			pad_count++;
		}
	}
	w9x_fpu_check(&fpu, "the gamepads' enumeration");
}

static long winmm_axis(DWORD value, UINT minimum, UINT maximum)
{
	if (maximum <= minimum)
		return 0;
	return (long)(((double)value - minimum) * 65535.0 / (double)(maximum - minimum)) - 32768;
}

static void pad_read_device(struct pad *pad)
{
	int index;

	if (pad->device)
	{
		DIJOYSTATE state;
		HRESULT result;

		pad->device->lpVtbl->Poll(pad->device);
		result = pad->device->lpVtbl->GetDeviceState(pad->device, sizeof(state), &state);
		if (result == DIERR_INPUTLOST || result == DIERR_NOTACQUIRED)
		{
			pad->device->lpVtbl->Acquire(pad->device);
			result = pad->device->lpVtbl->GetDeviceState(pad->device, sizeof(state), &state);
		}
		if (FAILED(result))
		{
			/* (another program in front: nothing held) */
			memset(pad->axes, 0, sizeof(pad->axes));
			memset(pad->buttons, 0, sizeof(pad->buttons));
			pad->hat = -1;
			return;
		}
		pad->axes[0] = state.lX;
		pad->axes[1] = state.lY;
		pad->axes[2] = state.lZ;
		pad->axes[3] = state.lRx;
		pad->axes[4] = state.lRy;
		pad->axes[5] = state.lRz;
		pad->axes[6] = state.rglSlider[0];
		pad->axes[7] = state.rglSlider[1];
		for (index = 0; index < PAD_BUTTONS; index++)
			pad->buttons[index] = (state.rgbButtons[index] & 0x80) != 0;
		pad->hat = LOWORD(state.rgdwPOV[0]) == 0xffff ? -1 : (long)state.rgdwPOV[0];
	}
	else
	{
		const JOYCAPSA *capabilities = &pad->capabilities;
		JOYINFOEX state;

		memset(&state, 0, sizeof(state));
		state.dwSize = sizeof(state);
		state.dwFlags = JOY_RETURNALL;
		if (joyGetPosEx(pad->joystick_id, &state) != JOYERR_NOERROR)
			return;
		/* winmm's X Y Z R U V, where DirectInput's Rz is R */
		pad->axes[0] = winmm_axis(state.dwXpos, capabilities->wXmin, capabilities->wXmax);
		pad->axes[1] = winmm_axis(state.dwYpos, capabilities->wYmin, capabilities->wYmax);
		pad->axes[2] = winmm_axis(state.dwZpos, capabilities->wZmin, capabilities->wZmax);
		pad->axes[5] = winmm_axis(state.dwRpos, capabilities->wRmin, capabilities->wRmax);
		pad->axes[6] = winmm_axis(state.dwUpos, capabilities->wUmin, capabilities->wUmax);
		pad->axes[7] = winmm_axis(state.dwVpos, capabilities->wVmin, capabilities->wVmax);
		for (index = 0; index < PAD_BUTTONS; index++)
			pad->buttons[index] = (state.dwButtons >> index) & 1;
		pad->hat = (capabilities->wCaps & JOYCAPS_HASPOV) && state.dwPOV != JOY_POVCENTERED ? (long)state.dwPOV : -1;
	}
}

static BOOL hat_has(long hat, int mask)
{
	/* eight positions; a diagonal is both of its directions */
	if (hat < 0)
		return FALSE;
	hat %= 36000;
	switch (mask)
	{
	case 1: return hat > 29250 || hat < 6750;
	case 2: return hat > 2250 && hat < 15750;
	case 4: return hat > 11250 && hat < 24750;
	case 8: return hat > 20250 && hat < 33750;
	default: return FALSE;
	}
}

/* a control's reading: an axis -32768..32767; anything else 0..32767 */
static long binding_value(const struct pad *pad, const struct binding *binding, BOOL as_axis)
{
	long value;

	switch (binding->kind)
	{
	case 'b':
		value = binding->index < PAD_BUTTONS && pad->buttons[binding->index] ? 32767 : 0;
		break;
	case 'h':
		value = hat_has(pad->hat, binding->hat_mask) ? 32767 : 0;
		break;
	case 'a':
		value = binding->index < PAD_AXES ? pad->axes[binding->index] : 0;
		if (binding->inverted)
			value = -value - 1;
		if (binding->half == '+')
			value = value > 0 ? value : 0;
		else if (binding->half == '-')
			value = value < 0 ? -value - 1 : 0;
		else if (!as_axis)
			/* a whole axis as a trigger or button: its full travel */
			value = (value + 32768) / 2;
		break;
	default:
		value = 0;
		break;
	}
	if (value > 32767)
		value = 32767;
	if (value < -32768)
		value = -32768;
	return value;
}

static void pad_map(struct pad *pad)
{
	struct w9x_pad_state *state = &pad->state;
	int control;

	memset(state, 0, sizeof(*state));
	for (control = 0; control < NUMBER_OF_MAP_CONTROLS; control++)
	{
		if (map_buttons[control] && binding_value(pad, &bindings[control], FALSE) > 16384)
			state->buttons |= map_buttons[control];
	}
	state->left_trigger = (unsigned char)(binding_value(pad, &bindings[_map_left_trigger], FALSE) * 255 / 32767);
	state->right_trigger = (unsigned char)(binding_value(pad, &bindings[_map_right_trigger], FALSE) * 255 / 32767);
	state->left_x = (short)binding_value(pad, &bindings[_map_left_x], TRUE);
	state->left_y = (short)binding_value(pad, &bindings[_map_left_y], TRUE);
	state->right_x = (short)binding_value(pad, &bindings[_map_right_x], TRUE);
	state->right_y = (short)binding_value(pad, &bindings[_map_right_y], TRUE);
}

/* ---------- public code */

void w9x_input_window_opened(HWND window)
{
	EnterCriticalSection(&lock);
	input_window = window;
	pads_wanted = TRUE;
	if (!bindings[_map_a].kind && !bindings[_map_left_x].kind)
		map_parse(default_map);
	LeaveCriticalSection(&lock);
}

void w9x_input_devices_changed(void)
{
	pads_wanted = TRUE;
}

void w9x_input_poll(void)
{
	int index;

	EnterCriticalSection(&lock);
	if (pads_wanted && input_window)
	{
		pads_wanted = FALSE;
		pads_enumerate();
	}
	for (index = 0; index < pad_count; index++)
	{
		pad_read_device(&pads[index]);
		pad_map(&pads[index]);
	}
	LeaveCriticalSection(&lock);
}

int w9x_pad_count(void)
{
	return pad_count;
}

int w9x_pad_read(int index, struct w9x_pad_state *state)
{
	int found;

	EnterCriticalSection(&lock);
	found = index >= 0 && index < pad_count;
	if (found)
		*state = pads[index].state;
	else
		memset(state, 0, sizeof(*state));
	LeaveCriticalSection(&lock);
	return found;
}

void w9x_pad_rumble(int index, unsigned short left, unsigned short right)
{
	struct pad *pad;
	DWORD now = GetTickCount();
	LONG direction[2];
	DICONSTANTFORCE force;
	DIEFFECT_DX5 effect;

	EnterCriticalSection(&lock);
	pad = index >= 0 && index < pad_count ? &pads[index] : NULL;
	if (!pad || !pad->rumble || (left == pad->rumble_left && right == pad->rumble_right) ||
		now - pad->rumble_sent < RUMBLE_INTERVAL_MS)
	{
		LeaveCriticalSection(&lock);
		return;
	}
	pad->rumble_left = left;
	pad->rumble_right = right;
	pad->rumble_sent = now;
	direction[0] = (LONG)left;
	direction[1] = (LONG)right;
	force.lMagnitude = (LONG)((DWORD)(left > right ? left : right) * (DWORD)DI_FFNOMINALMAX / 65535);
	memset(&effect, 0, sizeof(effect));
	effect.dwSize = sizeof(effect);
	effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
	effect.cAxes = 2;
	effect.rglDirection = direction;
	effect.cbTypeSpecificParams = sizeof(force);
	effect.lpvTypeSpecificParams = &force;
	if (force.lMagnitude)
	{
		pad->rumble->lpVtbl->SetParameters(pad->rumble, (LPCDIEFFECT)&effect,
			DIEP_DIRECTION | DIEP_TYPESPECIFICPARAMS | DIEP_START);
	}
	else
	{
		pad->rumble->lpVtbl->Stop(pad->rumble);
	}
	LeaveCriticalSection(&lock);
}
