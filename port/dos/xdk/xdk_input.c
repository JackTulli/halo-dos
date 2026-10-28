/*
XDK_INPUT.C

XInput for the game: its first Xbox controller is the PC's controls
together.

	- The keyboard's and the mouse's actions (bound on the menu's CONTROLS
	  page: halo_bindings.h, xdk_bindings.c) go to the game as actions, not
	  as the controller's buttons (port/dos/game/dos_prompts.c's
	  halo_keyboard_actions, which input_abstraction.c reads): the
	  controller's button layout moves nothing on the keyboard. Only the
	  look keys turn the right stick here (for machines without a mouse),
	  and the scores key presses Back.
	- The mouse aims directly: the game's look code asks
	  halo_linux_mouse_look for the motion since its last call and adds it
	  to the stick's facing change.
	- A game controller (halo_system.h's pads: on Windows XInput's or the
	  joysticks Windows knows, on DOS the game port's) is the controller
	  itself, its dead zones taken off (XInput's), its bumpers the Xbox's
	  white and black buttons; Start opens the pause (it is the Escape
	  key's), and while a menu is up the D-pad or the left stick, A and B
	  are its arrows, Enter and Escape, whichever controller it is.
	- Split screen (dos_split.h): Halo's local player n reads controller
	  port n, which is its player's device: the keyboard and mouse, or one
	  controller. Off, port 0 is the only one, played with everything.

The game's debug keyboard (its console) gets the keystrokes while the
console is open, and the backquote that opens it.
*/

#include "platform.h"

#include <math.h>
#include <string.h>

#include "halo_bindings.h"
#include "dos_split.h"

#define PORT_COUNT 4

/* the Windows virtual keys the game's console reads (the Xbox's debug
keyboard sends them) */
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_OEM_3_BACKQUOTE 0xC0

/* main/console.c */
extern unsigned char console_is_active(void);

/* ---------- device tables */

XPP_DEVICE_TYPE XDEVICE_TYPE_GAMEPAD_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_MEMORY_UNIT_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE;

struct controller
{
	BOOL open;
	DWORD packet_number;
	XINPUT_GAMEPAD previous;
};

static struct controller controllers[PORT_COUNT];
static struct controller keyboard_device;
static BOOL reported_keyboard;
static DWORD reported_gamepads;

/* ---------- the mouse */

static float mouse_pending_x, mouse_pending_y;
static int mouse_polls_unconsumed;
/* when the mouse last turned the view: the game's magnetism is off while
the mouse aims (dos_mouse_aiming) */
static unsigned long long mouse_moved_at;

int dos_mouse_aiming(void)
{
	return mouse_moved_at && system_microseconds() - mouse_moved_at < 2000000ULL;
}

/* the input.mouse_sensitivity and input.invert_mouse settings
(dos_settings.c) */
float dos_mouse_sensitivity(void);
int dos_invert_mouse(void);
/* the menu's (port/dos/game/dos_menu.c): while it has the keyboard the
game's controller reads nothing */
int dos_menu_has_input(void);

int halo_linux_mouse_look(short gamepad_index, float *yaw, float *pitch)
{
	/* radians a mickey at sensitivity 1 (a mouse driver's mickey is about
	twice a desktop pixel) */
	const float scale = 0.0044f;
	float x = mouse_pending_x, y = mouse_pending_y;

	*yaw = 0.0f;
	*pitch = 0.0f;
	/* (the keyboard's and mouse's player's: the first, or split screen's) */
	if (gamepad_index != dos_split_keyboard_port())
		return FALSE;
	mouse_pending_x = 0.0f;
	mouse_pending_y = 0.0f;
	mouse_polls_unconsumed = 0;
	if ((x == 0.0f && y == 0.0f) || dos_menu_has_input())
		return FALSE;
	mouse_moved_at = system_microseconds();
	*yaw = -x * scale * dos_mouse_sensitivity();
	*pitch = (dos_invert_mouse() ? y : -y) * scale * dos_mouse_sensitivity();
	return TRUE;
}

/* collects the motion the game has not asked for yet; motion nobody takes
for a few polls (menus) is dropped so that it cannot jerk the view later */
static void mouse_poll(const struct system_input *input)
{
	if (++mouse_polls_unconsumed > 4)
	{
		mouse_pending_x = 0.0f;
		mouse_pending_y = 0.0f;
	}
	mouse_pending_x += (float)input->mouse_dx;
	mouse_pending_y += (float)input->mouse_dy;
}

/* ---------- the keyboard as a controller */

static BYTE analog(int down)
{
	return down ? 0xFF : 0x00;
}

/* a stick's dead zone (XInput's) taken off, the rest stretched to the
whole range, in its direction */
static void stick(int x, int y, float zone, SHORT *out_x, SHORT *out_y)
{
	float magnitude = sqrtf((float)x * (float)x + (float)y * (float)y), scale;

	*out_x = *out_y = 0;
	if (magnitude <= zone)
		return;
	scale = (magnitude - zone) / (32767.0f - zone);
	if (scale > 1.0f)
		scale = 1.0f;
	scale *= 32767.0f / magnitude;
	*out_x = (SHORT)((float)x * scale);
	*out_y = (SHORT)((float)y * scale);
}

/* the keyboard's part of a controller's state (its actions go to the game
apart: halo_keyboard_actions): the look keys as the right stick, the
scores as Back */
static void controls_keyboard(XINPUT_GAMEPAD *pad)
{
	int look_x = 0, look_y = 0;

	/* the keyboard's: the look keys as the right stick, scores as Back */
	look_x = dos_action_held(DOS_ACTION_LOOK_RIGHT) - dos_action_held(DOS_ACTION_LOOK_LEFT);
	look_y = dos_action_held(DOS_ACTION_LOOK_UP) - dos_action_held(DOS_ACTION_LOOK_DOWN);
	pad->sThumbRX = (SHORT)(look_x * 24000);
	pad->sThumbRY = (SHORT)(look_y * 24000);
	if (dos_action_held(DOS_ACTION_SCORES))
		pad->wButtons |= XINPUT_GAMEPAD_BACK;
	{
		/* (testing, setting test_fire=1): the trigger pulled a tenth of a
		second in every fifth, for pictures of shots without a player */
		extern long dos_setting_integer(const char *name, long default_value);
		static int test_fire = -1;

		if (test_fire < 0)
			test_fire = dos_setting_integer("test_fire", 0) != 0;
		if (test_fire && (system_microseconds() / 100000) % 5 == 0)
			pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = 0xFF;
	}
}

/* a controller's part (its Start is the pause's: pad_keys); added to what
is there (the one player's every device at once) */
static void controls_pad(const struct system_pad *source, XINPUT_GAMEPAD *pad)
{
	{
		static const struct
		{
			unsigned short pad;
			int analog;
		} analogs[] =
		{
			{ SYSTEM_PAD_A, XINPUT_GAMEPAD_A }, { SYSTEM_PAD_B, XINPUT_GAMEPAD_B },
			{ SYSTEM_PAD_X, XINPUT_GAMEPAD_X }, { SYSTEM_PAD_Y, XINPUT_GAMEPAD_Y },
			{ SYSTEM_PAD_LEFT_BUMPER, XINPUT_GAMEPAD_WHITE }, { SYSTEM_PAD_RIGHT_BUMPER, XINPUT_GAMEPAD_BLACK },
		};
		unsigned short buttons = source->buttons;
		SHORT left_x, left_y, right_x, right_y;
		BYTE left_trigger = source->left_trigger > 30 ? source->left_trigger : 0;
		BYTE right_trigger = source->right_trigger > 30 ? source->right_trigger : 0;

		stick(source->left_x, source->left_y, 7849.0f, &left_x, &left_y);
		stick(source->right_x, source->right_y, 8689.0f, &right_x, &right_y);
		if (left_x || left_y || right_x || right_y || buttons || left_trigger || right_trigger)
			dos_pad_used();
		if (left_x || left_y)
		{
			pad->sThumbLX = left_x;
			pad->sThumbLY = left_y;
		}
		if (right_x || right_y)
		{
			pad->sThumbRX = right_x;
			pad->sThumbRY = right_y;
		}
		pad->wButtons |= buttons & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT |
			XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB);
		for (unsigned i = 0; i < sizeof(analogs) / sizeof(analogs[0]); i++)
		{
			if (buttons & analogs[i].pad)
				pad->bAnalogButtons[analogs[i].analog] = 0xFF;
		}
		if (left_trigger > pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER])
			pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = left_trigger;
		if (right_trigger > pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER])
			pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = right_trigger;
	}
}

/* a controller's buttons as the keys the menus and the pause read: Start
is Escape (the pause, and back out of it); while a menu is up, the D-pad
or left stick its arrows, A Enter and B Escape */
static void pad_keys(struct system_input *input)
{
	/* the menu's split screen page reads each controller itself (which one
	pressed A is who joins) */
	extern int dos_menu_reads_pads(void);
	int raw = dos_menu_reads_pads();

	for (int i = 0; i < input->pad_count; i++)
	{
		const struct system_pad *pad = &input->pads[i];
		unsigned short buttons = pad->buttons;

		if (raw)
			continue;
		if (buttons & SYSTEM_PAD_START)
			input->keys[SYSTEM_KEY_ESCAPE] = 1;
		if (!dos_menu_has_input())
			continue;
		if (pad->left_y > 16000) buttons |= SYSTEM_PAD_UP;
		if (pad->left_y < -16000) buttons |= SYSTEM_PAD_DOWN;
		if (pad->left_x < -16000) buttons |= SYSTEM_PAD_LEFT;
		if (pad->left_x > 16000) buttons |= SYSTEM_PAD_RIGHT;
		if (buttons & SYSTEM_PAD_UP) input->keys[SYSTEM_KEY_UP] = 1;
		if (buttons & SYSTEM_PAD_DOWN) input->keys[SYSTEM_KEY_DOWN] = 1;
		if (buttons & SYSTEM_PAD_LEFT) input->keys[SYSTEM_KEY_LEFT] = 1;
		if (buttons & SYSTEM_PAD_RIGHT) input->keys[SYSTEM_KEY_RIGHT] = 1;
		if (buttons & SYSTEM_PAD_A) input->keys[SYSTEM_KEY_ENTER] = 1;
		if (buttons & SYSTEM_PAD_B) input->keys[SYSTEM_KEY_ESCAPE] = 1;
		/* (the game list's: X its FILTERS, Y its order, BACK QUICK JOIN, the
		right bumper REFRESH) */
		if (buttons & SYSTEM_PAD_X) input->keys[SYSTEM_KEY_F] = 1;
		if (buttons & SYSTEM_PAD_Y) input->keys[SYSTEM_KEY_TAB] = 1;
		if (buttons & SYSTEM_PAD_BACK) input->keys[SYSTEM_KEY_Q] = 1;
		if (buttons & SYSTEM_PAD_RIGHT_BUMPER) input->keys[SYSTEM_KEY_R] = 1;
		if (buttons)
			dos_pad_used();
	}
}

/* ---------- reading */

static struct system_input input;
static DWORD input_frame;

/* the system's input, read once a frame (the game asks every pad) */
static void read_input(void)
{
	extern long dos_frame_number(void);
	long frame = dos_frame_number();

	if ((DWORD)frame == input_frame && frame)
		return;
	input_frame = (DWORD)frame;
	system_input_read(&input);
	mouse_poll(&input);
	/* (the bindings see the real keys; the controller's then pressed for
	the menus) */
	dos_bindings_update(&input);
	pad_keys(&input);
}

/* for the game-side glue: what the player is holding (the menus, the debug
keys) */
const struct system_input *dos_input(void)
{
	read_input();
	return &input;
}

/* ---------- XAPI */

VOID WINAPI XInitDevices(DWORD preallocation_type_count, PXDEVICE_PREALLOC_TYPE preallocation_types)
{
	(void)preallocation_type_count;
	(void)preallocation_types;
}

BOOL WINAPI XGetDeviceChanges(PXPP_DEVICE_TYPE device_type, PDWORD insertions, PDWORD removals)
{
	*insertions = 0;
	*removals = 0;
	if (device_type == XDEVICE_TYPE_GAMEPAD)
	{
		/* (one port, or split screen's players' ports) */
		int players = dos_split_count();
		DWORD connected = players > 1 ? (DWORD)((1 << players) - 1) : XDEVICE_PORT0_MASK;

		*insertions = connected & ~reported_gamepads;
		*removals = reported_gamepads & ~connected;
		reported_gamepads = connected;
	}
	else if (device_type == XDEVICE_TYPE_DEBUG_KEYBOARD)
	{
		if (!reported_keyboard)
		{
			*insertions = 1;
			reported_keyboard = TRUE;
		}
	}
	return *insertions || *removals;
}

HANDLE WINAPI XInputOpen(PXPP_DEVICE_TYPE device_type, DWORD port, DWORD slot,
	PXINPUT_POLLING_PARAMETERS polling_parameters)
{
	(void)slot;
	(void)polling_parameters;
	if (device_type == XDEVICE_TYPE_GAMEPAD && port < PORT_COUNT)
	{
		memset(&controllers[port], 0, sizeof(controllers[port]));
		controllers[port].open = TRUE;
		return (HANDLE)&controllers[port];
	}
	if (device_type == XDEVICE_TYPE_DEBUG_KEYBOARD && port == 0)
	{
		keyboard_device.open = TRUE;
		return (HANDLE)&keyboard_device;
	}
	SetLastError(ERROR_DEVICE_NOT_CONNECTED);
	return NULL;
}

VOID WINAPI XInputClose(HANDLE device)
{
	struct controller *controller = (struct controller *)device;

	if (controller)
		controller->open = FALSE;
}

static int controller_port(HANDLE device)
{
	for (int port = 0; port < PORT_COUNT; port++)
	{
		if (device == (HANDLE)&controllers[port] && controllers[port].open)
			return port;
	}
	return -1;
}

DWORD WINAPI XInputGetState(HANDLE device, PXINPUT_STATE state)
{
	int port = controller_port(device);

	memset(state, 0, sizeof(*state));
	if (port < 0)
		return ERROR_DEVICE_NOT_CONNECTED;
	read_input();
	if (!console_is_active() && !dos_menu_has_input())
	{
		const struct dos_split_player *player = dos_split_player(port);

		if (!dos_split_count())
		{
			/* (the one player: the keyboard and every controller) */
			if (port == 0)
			{
				controls_keyboard(&state->Gamepad);
				for (int i = 0; i < input.pad_count; i++)
					controls_pad(&input.pads[i], &state->Gamepad);
			}
		}
		else if (player && player->device == DOS_DEVICE_KEYBOARD)
		{
			controls_keyboard(&state->Gamepad);
		}
		else if (player && player->device == DOS_DEVICE_PAD)
		{
			for (int i = 0; i < input.pad_count; i++)
			{
				if (input.pads[i].id == player->pad)
					controls_pad(&input.pads[i], &state->Gamepad);
			}
		}
	}
	if (memcmp(&state->Gamepad, &controllers[port].previous, sizeof(state->Gamepad)))
	{
		controllers[port].packet_number++;
		controllers[port].previous = state->Gamepad;
	}
	state->dwPacketNumber = controllers[port].packet_number;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputSetState(HANDLE device, PXINPUT_FEEDBACK feedback)
{
	if (!feedback)
		return ERROR_INVALID_PARAMETER;
	/* (the port's controller's motors: split screen's player's, or the one
	player's every controller) */
	{
		int port = controller_port(device);
		const struct dos_split_player *player = dos_split_player(port);

		if (player && player->device == DOS_DEVICE_PAD)
			system_pad_rumble(player->pad, feedback->Rumble.wLeftMotorSpeed, feedback->Rumble.wRightMotorSpeed);
		else if (!dos_split_count() && port == 0)
		{
			for (int i = 0; i < input.pad_count; i++)
				system_pad_rumble(input.pads[i].id, feedback->Rumble.wLeftMotorSpeed, feedback->Rumble.wRightMotorSpeed);
		}
	}
	feedback->Header.dwStatus = ERROR_SUCCESS;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugInitKeyboardQueue(PXINPUT_DEBUG_KEYQUEUE_PARAMETERS parameters)
{
	(void)parameters;
	return ERROR_SUCCESS;
}

/* scan codes as the Windows virtual keys the console wants */
static BYTE virtual_key(int key, int character)
{
	if (character >= 'a' && character <= 'z')
		return (BYTE)(character - 'a' + 'A');
	if ((character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9'))
		return (BYTE)character;
	switch (key)
	{
	case SYSTEM_KEY_BACKQUOTE: return VK_OEM_3_BACKQUOTE;
	case SYSTEM_KEY_ENTER: case SYSTEM_KEY_KEYPAD_ENTER: return VK_RETURN;
	case SYSTEM_KEY_ESCAPE: return VK_ESCAPE;
	case SYSTEM_KEY_BACKSPACE: return VK_BACK;
	case SYSTEM_KEY_TAB: return VK_TAB;
	case SYSTEM_KEY_SPACE: return VK_SPACE;
	case SYSTEM_KEY_UP: return VK_UP;
	case SYSTEM_KEY_DOWN: return VK_DOWN;
	case SYSTEM_KEY_LEFT: return VK_LEFT;
	case SYSTEM_KEY_RIGHT: return VK_RIGHT;
	case SYSTEM_KEY_LEFT_SHIFT: case SYSTEM_KEY_RIGHT_SHIFT: return VK_SHIFT;
	case SYSTEM_KEY_LEFT_CONTROL: case SYSTEM_KEY_RIGHT_CONTROL: return VK_CONTROL;
	default: return (BYTE)character;
	}
}

DWORD WINAPI XInputDebugGetKeystroke(PXINPUT_DEBUG_KEYSTROKE keystroke)
{
	int key, character, down;

	memset(keystroke, 0, sizeof(*keystroke));
	while (system_next_keystroke(&key, &character, &down))
	{
		BYTE code = virtual_key(key, character);

		/* key ups always pass, so no key is left latched down */
		if (!down || code == VK_OEM_3_BACKQUOTE || console_is_active())
		{
			keystroke->VirtualKey = code;
			keystroke->Ascii = (CHAR)(down ? character : 0);
			keystroke->Flags = down ? 0 : XINPUT_DEBUG_KEYSTROKE_FLAG_KEYUP;
			return ERROR_SUCCESS;
		}
	}
	return ERROR_HANDLE_EOF;
}
