/*
HALO_BINDINGS.H

The keyboard's and the mouse's controls, each action bound to up to three
keys, mouse buttons or wheel turns (port/dos/xdk/xdk_bindings.c), kept in
the settings file as bind_<action> = KEY,KEY and changed on the menu's
CONTROLS page. The game takes the actions themselves (halo_keyboard.h's,
through port/dos/game/dos_prompts.c), so a controller's button layout
(CONTROLLER page) moves nothing on the keyboard.
*/

#ifndef __HALO_BINDINGS_H
#define __HALO_BINDINGS_H

struct system_input;

/* a binding: a key's scan code (halo_system.h), or the mouse's */
#define BIND_NONE 0
#define BIND_MOUSE_LEFT 256
#define BIND_MOUSE_RIGHT 257
#define BIND_MOUSE_MIDDLE 258
#define BIND_WHEEL_UP 259
#define BIND_WHEEL_DOWN 260
#define BIND_CODES 261
#define BIND_SLOTS 3

enum
{
	DOS_ACTION_FORWARD,
	DOS_ACTION_BACK,
	DOS_ACTION_LEFT,
	DOS_ACTION_RIGHT,
	DOS_ACTION_JUMP,
	DOS_ACTION_CROUCH,
	DOS_ACTION_FIRE,
	DOS_ACTION_GRENADE,
	DOS_ACTION_MELEE,
	DOS_ACTION_ACTION,
	DOS_ACTION_RELOAD,
	DOS_ACTION_ZOOM,
	DOS_ACTION_WEAPON,
	DOS_ACTION_GRENADES,
	DOS_ACTION_FLASHLIGHT,
	DOS_ACTION_SCORES,
	DOS_ACTION_LOOK_UP,
	DOS_ACTION_LOOK_DOWN,
	DOS_ACTION_LOOK_LEFT,
	DOS_ACTION_LOOK_RIGHT,
	DOS_ACTION_COUNT
};

/* the menu's name of an action ("MOVE FORWARD") */
const char *dos_action_name(int action);
/* an action's bindings, BIND_NONE where a slot is empty */
int dos_binding(int action, int slot);
/* a binding's name ("SPACE", "MOUSE LEFT", "WHEEL UP") */
const char *dos_binding_name(int code);
/* the action's first binding becomes code, the others move down (the last
dropped); any other action bound to code loses it */
void dos_binding_assign(int action, int code);
void dos_binding_clear(int action);
void dos_bindings_reset(void);

/* the input read (xdk_input.c, once a frame): the wheel's turns held a
moment, what was newly pressed, when the keyboard or mouse was last used */
void dos_bindings_update(const struct system_input *input);
/* whether an action's binding is held */
int dos_action_held(int action);
/* halo_keyboard.h's actions held (a bit each) */
unsigned long dos_bindings_keyboard_actions(void);
/* a key, mouse button or wheel turn newly pressed in this read (for the
CONTROLS page to bind), BIND_NONE if none */
int dos_binding_pressed(void);
/* when the keyboard or mouse, and a controller, were last used
(microseconds): the HUD's prompts name the keys after the keyboard */
unsigned long long dos_keyboard_used_at(void);
unsigned long long dos_pad_used_at(void);
void dos_pad_used(void);

#endif /* __HALO_BINDINGS_H */
