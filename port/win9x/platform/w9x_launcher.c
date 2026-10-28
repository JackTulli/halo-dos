/*
W9X_LAUNCHER.C

The settings window the Windows builds show at start (launcher.h): a
dialog over port_config.c's settings table, with the display, graphics,
sound and input settings on pages of their own and every other setting of
the build in a list, so that the whole of config.toml can be set here. Play
hands the values back and the game goes on with them; Quit ends it.

A Windows-facing unit (see w9x_backend.h): the Windows SDK only, built as
port/windows/src/win32_*.c are. The native Windows build compiles this same
file as port/windows/src/win32_launcher.c, before SDL has made anything.

The dialog is a DLGTEMPLATE put together in memory (no resource compiler
in either build) of standard controls, plus the tab control, the trackbar
and the tooltips of comctl32, which Windows 95's has: comctl32.dll is
loaded at run time for InitCommonControls and the classes come by name in
the template. Every control is created at once; the pages' controls are
shown and hidden as the tabs change. Nothing of the newer common controls
(InitCommonControlsEx, the version 6 look) is asked for.

Sizes are in dialog units of the 8-point dialog font: at 96 dpi the window
is about 554 by 420 pixels with its frame, which fits a 640x480 desktop
(port/win9x/tests/launcher checks the layout against 600x440).
*/

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "launcher.h"

/* ---------- comctl32's names, as Windows 95's comctl32 knows them */

#define LAUNCHER_TAB_CLASS "SysTabControl32"
#define LAUNCHER_TRACKBAR_CLASS "msctls_trackbar32"
#define LAUNCHER_TOOLTIPS_CLASS "tooltips_class32"

#define LAUNCHER_TCM_INSERTITEMA (0x1300 + 7)
#define LAUNCHER_TCM_GETCURSEL (0x1300 + 11)
#define LAUNCHER_TCM_SETCURSEL (0x1300 + 12)
#define LAUNCHER_TCIF_TEXT 0x0001
#define LAUNCHER_TCN_SELCHANGE ((UINT)-551)

#define LAUNCHER_TBM_GETPOS (WM_USER)
#define LAUNCHER_TBM_SETPOS (WM_USER + 5)
#define LAUNCHER_TBM_SETRANGE (WM_USER + 6)
#define LAUNCHER_TBM_SETTICFREQ (WM_USER + 20)
#define LAUNCHER_TBM_SETPAGESIZE (WM_USER + 21)
#define LAUNCHER_TBS_AUTOTICKS 0x0001

#define LAUNCHER_TTS_ALWAYSTIP 0x01
#define LAUNCHER_TTM_SETDELAYTIME (WM_USER + 3)
#define LAUNCHER_TTM_ADDTOOLA (WM_USER + 4)
#define LAUNCHER_TTM_SETMAXTIPWIDTH (WM_USER + 24)
#define LAUNCHER_TTDT_AUTOPOP 2
#define LAUNCHER_TTF_IDISHWND 0x0001
#define LAUNCHER_TTF_SUBCLASS 0x0010

/* the 4.0 structures (the SDK's grow with _WIN32_IE) */
struct launcher_tab_item
{
	UINT mask;
	DWORD state, state_mask;
	LPSTR text;
	int text_max;
	int image;
	LPARAM param;
};

struct launcher_tool_info
{
	UINT size;
	UINT flags;
	HWND window;
	UINT_PTR id;
	RECT rect;
	HINSTANCE instance;
	LPSTR text;
};

/* ---------- the layout, in dialog units */

#define DIALOG_WIDTH 364
#define DIALOG_HEIGHT 244
/* the tab control and the pages' area inside it */
#define TAB_X 6
#define TAB_Y 6
#define TAB_WIDTH 352
#define TAB_HEIGHT 206
#define PAGE_LEFT 14
#define PAGE_RIGHT 352
#define PAGE_TOP 30
#define PAGE_BOTTOM 204
/* a label at the left, its control at the right */
#define LABEL_WIDTH 74
#define CONTROL_X 92
#define CONTROL_RIGHT PAGE_RIGHT
#define EDIT_HEIGHT 12
#define CHECKBOX_HEIGHT 10
#define ROW_HEIGHT 16
#define CHECKBOX_ROW_HEIGHT 13
/* the combo's height is its dropped list's */
#define COMBO_HEIGHT 90
/* the bottom row */
#define BUTTON_Y 219
#define BUTTON_WIDTH 56
#define BUTTON_HEIGHT 14

/* the fixed controls' ids; the settings' are from CONTROL_ID_FIRST */
enum
{
	_id_play = IDOK,
	_id_quit = IDCANCEL,
	_id_defaults = 3,
	_id_tab = 100,
	_id_advanced_list,
	_id_advanced_edit,
	_id_advanced_default,
	_id_advanced_description,
	_id_renderer_explanation,
	_id_field_of_view_value,
	_id_note,
};
#define CONTROL_ID_FIRST 1000

enum launcher_page
{
	_page_display,
	_page_graphics,
	_page_audio,
	_page_input,
	_page_advanced,
	NUMBER_OF_PAGES,
	/* on no page: always shown */
	_page_none = -1,
};

static const char *const page_names[NUMBER_OF_PAGES] = { "Display", "Graphics", "Audio", "Input", "Advanced" };

enum control_kind
{
	_control_static,
	_control_button,
	_control_checkbox,
	_control_edit,
	/* a list of choices (CBS_DROPDOWNLIST); a value that is none of them
	is added as one more entry */
	_control_combo_list,
	/* choices with an edit (CBS_DROPDOWN): any text */
	_control_combo_edit,
	_control_trackbar,
	_control_listbox,
	_control_tab,
};

struct launcher_control
{
	int id;
	enum control_kind kind;
	enum launcher_page page;
	/* the setting it edits (an index into the settings), -1 for none */
	int setting;
	short x, y, width, height;
	/* "" for a standard class by atom */
	const char *class_name;
	DWORD style;
	/* the caption, or the label of a checkbox or button */
	const char *text;
	/* combos: the values, and what is shown for them (NULL: the values) */
	const char *const *choices;
	const char *const *choice_labels;
	int choice_count;
	/* trackbars: the range */
	int minimum, maximum;
	HWND window;
};

#define MAXIMUM_CONTROLS 256
#define MAXIMUM_SETTINGS 256

struct launcher_layout
{
	struct launcher_control controls[MAXIMUM_CONTROLS];
	int control_count;
	/* the settings of the Advanced page's list, in the table's order */
	int advanced[MAXIMUM_SETTINGS];
	int advanced_count;
	/* for each setting: its control's index, or -1 for the list */
	int control_of_setting[MAXIMUM_SETTINGS];
	/* labels made from the settings' names (checkboxes and labels) */
	char labels[MAXIMUM_SETTINGS][64];
};

/* ---------- the choices of the hand-made controls */

static const char *const renderer_values[] = { "auto", "gl4", "gl2", "gl1" };
static const char *const renderer_labels[] =
{
	"auto",
	"gl4 (OpenGL 4.5)",
	"gl2 (OpenGL 2.0)",
	"gl1 (light, OpenGL 1.1)",
};
static const char *const renderer_explanations[] =
{
	"auto takes the first of gl4, gl2 and gl1 the display driver can run; the log says which and why.",
	"gl4 needs OpenGL 4.5: the full picture, on current cards and drivers.",
	"gl2 draws the same picture on OpenGL 2.0, for older cards and drivers and virtual machines.",
	"gl1 is the light renderer for cards without shaders (OpenGL 1.1): fewer effects, the least work.",
};
static const char *const texture_bits_values[] = { "16", "32" };
static const char *const light_pipeline_values[] = { "auto", "true", "false" };
static const char *const light_pipeline_labels[] =
{
	"auto (with the gl1 renderer)",
	"true (with every renderer)",
	"false (never)",
};
static const char *const audio_output_values[] = { "auto", "dsound", "waveout" };
static const char *const audio_output_labels[] = { "auto", "dsound (DirectSound)", "waveout (the wave mapper)" };
static const char *const audio_rate_values[] = { "0", "22050", "44100", "48000" };
static const char *const audio_rate_labels[] = { "0 (the sound card's own)", "22050", "44100", "48000" };

/* the display modes, filled from the system (display_modes_enumerate) */
#define MAXIMUM_DISPLAY_MODES 128
static char display_mode_texts[MAXIMUM_DISPLAY_MODES][24];
static const char *display_mode_values[MAXIMUM_DISPLAY_MODES];
static int display_mode_count;

/* the Display page's checkboxes, with better words than their names */
static const struct
{
	const char *name;
	const char *label;
} display_labels[] =
{
	{ "display.fullscreen", "Fullscreen" },
	{ "display.vsync", "Vsync: wait for the display between frames" },
	{ "display.interpolation", "Interpolation: a frame for every display refresh" },
	{ "display.fps_counter", "Frame rate counter in the top left" },
};

/* ---------- building the layout */

static int find_setting(const struct launcher_setting *settings, int count, const char *name)
{
	int index;

	for (index = 0; index < count; index++)
	{
		if (!strcmp(settings[index].name, name))
			return index;
	}
	return -1;
}

static int setting_in_section(const struct launcher_setting *setting, const char *section)
{
	size_t length = strlen(section);

	return !strncmp(setting->name, section, length) && setting->name[length] == '.';
}

/* "soft_shadows" -> "Soft shadows" */
static void humanize(const char *name, char *label, size_t size)
{
	const char *key = strchr(name, '.');
	size_t index;

	key = key ? key + 1 : name;
	for (index = 0; index + 1 < size && key[index]; index++)
		label[index] = key[index] == '_' ? ' ' : key[index];
	label[index] = 0;
	if (label[0] >= 'a' && label[0] <= 'z')
		label[0] = (char)(label[0] - 'a' + 'A');
}

static struct launcher_control *add_control(struct launcher_layout *layout, enum control_kind kind,
	enum launcher_page page, int setting, int x, int y, int width, int height, const char *class_name, DWORD style,
	const char *text)
{
	struct launcher_control *control;

	if (layout->control_count >= MAXIMUM_CONTROLS)
		return NULL;
	control = &layout->controls[layout->control_count];
	memset(control, 0, sizeof(*control));
	control->id = CONTROL_ID_FIRST + layout->control_count;
	control->kind = kind;
	control->page = page;
	control->setting = setting;
	control->x = (short)x;
	control->y = (short)y;
	control->width = (short)width;
	control->height = (short)height;
	control->class_name = class_name;
	control->style = style | WS_CHILD | (page == _page_none || page == _page_display ? WS_VISIBLE : 0);
	control->text = text;
	if (setting >= 0)
		layout->control_of_setting[setting] = layout->control_count;
	layout->control_count++;
	return control;
}

static void add_label(struct launcher_layout *layout, enum launcher_page page, int x, int y, int width,
	const char *text)
{
	add_control(layout, _control_static, page, -1, x, y + 2, width, 8, "", SS_LEFT, text);
}

static void add_checkbox(struct launcher_layout *layout, enum launcher_page page, int setting, int x, int y,
	int width, const char *label)
{
	add_control(layout, _control_checkbox, page, setting, x, y, width, CHECKBOX_HEIGHT, "",
		BS_AUTOCHECKBOX | WS_TABSTOP, label);
}

static void add_edit(struct launcher_layout *layout, enum launcher_page page, int setting, int x, int y,
	int width)
{
	add_control(layout, _control_edit, page, setting, x, y, width, EDIT_HEIGHT, "",
		ES_LEFT | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, "");
}

static void add_combo(struct launcher_layout *layout, enum launcher_page page, int setting, int x, int y,
	int width, int editable, const char *const *choices, const char *const *labels, int choice_count)
{
	struct launcher_control *control = add_control(layout, editable ? _control_combo_edit : _control_combo_list,
		page, setting, x, y, width, COMBO_HEIGHT, "",
		(editable ? CBS_DROPDOWN | CBS_AUTOHSCROLL : CBS_DROPDOWNLIST) | WS_VSCROLL | WS_TABSTOP, "");

	if (control)
	{
		control->choices = choices;
		control->choice_labels = labels;
		control->choice_count = choice_count;
	}
}

/* a setting as a row of a page: a checkbox, or a label and an edit;
FALSE when the page is full (the setting then goes to the list) */
static int add_generic_row(struct launcher_layout *layout, enum launcher_page page, int setting,
	const struct launcher_setting *definition, int *y)
{
	int row_height = definition->type == _launcher_boolean ? CHECKBOX_ROW_HEIGHT : ROW_HEIGHT;

	if (*y + row_height > PAGE_BOTTOM)
		return 0;
	humanize(definition->name, layout->labels[setting], sizeof(layout->labels[setting]));
	if (definition->type == _launcher_boolean)
	{
		add_checkbox(layout, page, setting, PAGE_LEFT, *y, PAGE_RIGHT - PAGE_LEFT, layout->labels[setting]);
	}
	else
	{
		add_label(layout, page, PAGE_LEFT, *y, LABEL_WIDTH, layout->labels[setting]);
		add_edit(layout, page, setting, CONTROL_X, *y, definition->type == _launcher_string ?
			CONTROL_RIGHT - CONTROL_X : 48);
	}
	*y += row_height;
	return 1;
}

static void add_to_advanced(struct launcher_layout *layout, int setting)
{
	if (layout->advanced_count < MAXIMUM_SETTINGS)
		layout->advanced[layout->advanced_count++] = setting;
	layout->control_of_setting[setting] = -1;
}

/* the modes of the display at 640x480 and more, 16 or 32 bits a pixel,
after "desktop" */
static void display_modes_enumerate(void)
{
	DEVMODEA mode;
	DWORD index;

	if (display_mode_count)
		return;
	strcpy(display_mode_texts[0], "desktop");
	display_mode_values[0] = display_mode_texts[0];
	display_mode_count = 1;
	for (index = 0; display_mode_count < MAXIMUM_DISPLAY_MODES; index++)
	{
		char text[24];
		int slot, position;

		memset(&mode, 0, sizeof(mode));
#ifdef HALO_WIN9X
		/* 98/ME's DEVMODE ends before the panning fields (w9x_window.c) */
		mode.dmSize = (WORD)FIELD_OFFSET(DEVMODEA, dmPanningWidth);
#else
		mode.dmSize = sizeof(mode);
#endif
		if (!EnumDisplaySettingsA(NULL, index, &mode))
			break;
		if (mode.dmPelsWidth < 640 || mode.dmPelsHeight < 480 || (mode.dmBitsPerPel != 16 && mode.dmBitsPerPel != 32))
			continue;
		snprintf(text, sizeof(text), "%lux%lux%lu", (unsigned long)mode.dmPelsWidth, (unsigned long)mode.dmPelsHeight,
			(unsigned long)mode.dmBitsPerPel);
		/* each once, in order of size */
		for (slot = 1; slot < display_mode_count; slot++)
		{
			unsigned long width = 0, height = 0, bits = 0;

			sscanf(display_mode_texts[slot], "%lux%lux%lu", &width, &height, &bits);
			if (width == mode.dmPelsWidth && height == mode.dmPelsHeight && bits == mode.dmBitsPerPel)
				break;
			if (width > mode.dmPelsWidth || (width == mode.dmPelsWidth && (height > mode.dmPelsHeight ||
				(height == mode.dmPelsHeight && bits > mode.dmBitsPerPel))))
			{
				break;
			}
		}
		if (slot < display_mode_count && !strcmp(display_mode_texts[slot], text))
			continue;
		for (position = display_mode_count; position > slot; position--)
			strcpy(display_mode_texts[position], display_mode_texts[position - 1]);
		strcpy(display_mode_texts[slot], text);
		display_mode_count++;
	}
	for (index = 0; (int)index < display_mode_count; index++)
		display_mode_values[index] = display_mode_texts[index];
}

/* the Display page: the settings named here get their own controls; the
rest of [display] goes to the list */
static void layout_display_page(struct launcher_layout *layout, const struct launcher_setting *settings, int count)
{
	int y = PAGE_TOP;
	int setting, index;

	setting = find_setting(settings, count, "display.renderer");
	if (setting >= 0)
	{
		struct launcher_control *explanation;

		add_label(layout, _page_display, PAGE_LEFT, y, LABEL_WIDTH, "Renderer");
		add_combo(layout, _page_display, setting, CONTROL_X, y, 150, 0, renderer_values, renderer_labels,
			(int)(sizeof(renderer_values) / sizeof(renderer_values[0])));
		explanation = add_control(layout, _control_static, _page_display, -1, CONTROL_X, y + 16,
			CONTROL_RIGHT - CONTROL_X, 18, "", SS_LEFT, "");
		if (explanation)
			explanation->id = _id_renderer_explanation;
		y += 36;
	}
	setting = find_setting(settings, count, "display.mode");
	if (setting >= 0)
	{
		display_modes_enumerate();
		add_label(layout, _page_display, PAGE_LEFT, y, LABEL_WIDTH, "Display mode");
		add_combo(layout, _page_display, setting, CONTROL_X, y, 100, 1, display_mode_values, NULL, display_mode_count);
		add_label(layout, _page_display, CONTROL_X + 106, y, CONTROL_RIGHT - CONTROL_X - 106,
			"for fullscreen (width x height x bits)");
		y += ROW_HEIGHT + 2;
	}
	for (index = 0; index < (int)(sizeof(display_labels) / sizeof(display_labels[0])); index++)
	{
		/* (the two about frames come after the field of view) */
		if (index == 2)
		{
			setting = find_setting(settings, count, "display.window_scale");
			if (setting >= 0)
			{
				add_label(layout, _page_display, PAGE_LEFT, y + 2, LABEL_WIDTH, "Window scale");
				add_edit(layout, _page_display, setting, CONTROL_X, y + 2, 24);
				add_label(layout, _page_display, CONTROL_X + 30, y + 2, CONTROL_RIGHT - CONTROL_X - 30,
					"times 640x480, in a window");
				y += ROW_HEIGHT + 2;
			}
			setting = find_setting(settings, count, "display.field_of_view");
			if (setting >= 0)
			{
				struct launcher_control *control;

				add_label(layout, _page_display, PAGE_LEFT, y + 2, LABEL_WIDTH, "Field of view");
				control = add_control(layout, _control_trackbar, _page_display, setting, CONTROL_X, y, 200, 16,
					LAUNCHER_TRACKBAR_CLASS, LAUNCHER_TBS_AUTOTICKS | WS_TABSTOP, "");
				if (control)
				{
					control->minimum = 40;
					control->maximum = 150;
				}
				control = add_control(layout, _control_static, _page_display, -1, CONTROL_X + 206, y + 4,
					CONTROL_RIGHT - CONTROL_X - 206, 8, "", SS_LEFT, "");
				if (control)
					control->id = _id_field_of_view_value;
				y += 20;
			}
		}
		setting = find_setting(settings, count, display_labels[index].name);
		if (setting < 0)
			continue;
		add_checkbox(layout, _page_display, setting, CONTROL_X, y, CONTROL_RIGHT - CONTROL_X,
			display_labels[index].label);
		y += CHECKBOX_ROW_HEIGHT;
	}
	setting = find_setting(settings, count, "display.gl1_texture_bits");
	if (setting >= 0)
	{
		y += 2;
		add_label(layout, _page_display, PAGE_LEFT, y, LABEL_WIDTH, "gl1 texture bits");
		add_combo(layout, _page_display, setting, CONTROL_X, y, 50, 0, texture_bits_values, NULL,
			(int)(sizeof(texture_bits_values) / sizeof(texture_bits_values[0])));
		add_label(layout, _page_display, CONTROL_X + 56, y, CONTROL_RIGHT - CONTROL_X - 56,
			"a texel, for the gl1 renderer");
		y += ROW_HEIGHT;
	}
}

/* a page of one section: its booleans as checkboxes in two columns, then
the rest in rows (a few with a list of choices) */
static void layout_section_page(struct launcher_layout *layout, const struct launcher_setting *settings, int count,
	enum launcher_page page, const char *section)
{
	int y = PAGE_TOP;
	int column = 0;
	int setting;

	for (setting = 0; setting < count; setting++)
	{
		if (!setting_in_section(&settings[setting], section) || settings[setting].type != _launcher_boolean ||
			layout->control_of_setting[setting] != -2)
		{
			continue;
		}
		if (y + CHECKBOX_ROW_HEIGHT > PAGE_BOTTOM)
		{
			add_to_advanced(layout, setting);
			continue;
		}
		humanize(settings[setting].name, layout->labels[setting], sizeof(layout->labels[setting]));
		add_checkbox(layout, page, setting, PAGE_LEFT + column * 170, y, 164, layout->labels[setting]);
		column = !column;
		if (!column)
			y += CHECKBOX_ROW_HEIGHT;
	}
	if (column)
		y += CHECKBOX_ROW_HEIGHT;
	y += 3;
	for (setting = 0; setting < count; setting++)
	{
		const struct launcher_setting *definition = &settings[setting];
		const char *const *choices = NULL, *const *labels = NULL;
		int choice_count = 0, editable = 0;

		if (!setting_in_section(definition, section) || layout->control_of_setting[setting] != -2)
			continue;
		if (!strcmp(definition->name, "graphics.light_pipeline"))
		{
			choices = light_pipeline_values;
			labels = light_pipeline_labels;
			choice_count = (int)(sizeof(light_pipeline_values) / sizeof(light_pipeline_values[0]));
		}
		else if (!strcmp(definition->name, "audio.output"))
		{
			choices = audio_output_values;
			labels = audio_output_labels;
			choice_count = (int)(sizeof(audio_output_values) / sizeof(audio_output_values[0]));
		}
		else if (!strcmp(definition->name, "audio.rate"))
		{
			choices = audio_rate_values;
			labels = audio_rate_labels;
			choice_count = (int)(sizeof(audio_rate_values) / sizeof(audio_rate_values[0]));
			editable = 1;
		}
		if (choices)
		{
			if (y + ROW_HEIGHT > PAGE_BOTTOM)
			{
				add_to_advanced(layout, setting);
				continue;
			}
			humanize(definition->name, layout->labels[setting], sizeof(layout->labels[setting]));
			add_label(layout, page, PAGE_LEFT, y, LABEL_WIDTH, layout->labels[setting]);
			add_combo(layout, page, setting, CONTROL_X, y, 150, editable, choices, labels, choice_count);
			y += ROW_HEIGHT;
		}
		else if (!add_generic_row(layout, page, setting, definition, &y))
		{
			add_to_advanced(layout, setting);
		}
	}
}

static void layout_advanced_page(struct launcher_layout *layout, const struct launcher_setting *settings, int count)
{
	struct launcher_control *control;
	int setting;

	control = add_control(layout, _control_listbox, _page_advanced, -1, PAGE_LEFT, PAGE_TOP,
		PAGE_RIGHT - PAGE_LEFT, 96, "", LBS_NOTIFY | LBS_USETABSTOPS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL |
		WS_BORDER | WS_TABSTOP, "");
	if (control)
		control->id = _id_advanced_list;
	add_label(layout, _page_advanced, PAGE_LEFT, PAGE_TOP + 102, 24, "Value");
	control = add_control(layout, _control_edit, _page_advanced, -1, PAGE_LEFT + 26, PAGE_TOP + 100,
		PAGE_RIGHT - PAGE_LEFT - 26 - 60, EDIT_HEIGHT, "", ES_LEFT | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, "");
	if (control)
		control->id = _id_advanced_edit;
	control = add_control(layout, _control_button, _page_advanced, -1, PAGE_RIGHT - 54, PAGE_TOP + 99, 54,
		BUTTON_HEIGHT, "", BS_PUSHBUTTON | WS_TABSTOP, "Default");
	if (control)
		control->id = _id_advanced_default;
	control = add_control(layout, _control_static, _page_advanced, -1, PAGE_LEFT, PAGE_TOP + 118,
		PAGE_RIGHT - PAGE_LEFT, PAGE_BOTTOM - PAGE_TOP - 118, "", SS_LEFT | SS_NOPREFIX, "");
	if (control)
		control->id = _id_advanced_description;
	/* every setting no page took */
	for (setting = 0; setting < count; setting++)
	{
		if (layout->control_of_setting[setting] == -2)
			add_to_advanced(layout, setting);
	}
}

static void launcher_layout(struct launcher_layout *layout, const struct launcher_setting *settings, int count)
{
	struct launcher_control *control;
	int setting;

	memset(layout, 0, sizeof(*layout));
	if (count > MAXIMUM_SETTINGS)
		count = MAXIMUM_SETTINGS;
	/* -2: not placed yet */
	for (setting = 0; setting < count; setting++)
		layout->control_of_setting[setting] = -2;
	control = add_control(layout, _control_tab, _page_none, -1, TAB_X, TAB_Y, TAB_WIDTH, TAB_HEIGHT,
		LAUNCHER_TAB_CLASS, WS_CLIPSIBLINGS | WS_TABSTOP, "");
	if (control)
		control->id = _id_tab;
	layout_display_page(layout, settings, count);
	layout_section_page(layout, settings, count, _page_graphics, "graphics");
	layout_section_page(layout, settings, count, _page_audio, "audio");
	layout_section_page(layout, settings, count, _page_input, "input");
	/* the bottom row */
	setting = find_setting(settings, count, "launcher.show");
	if (setting >= 0)
		add_checkbox(layout, _page_none, setting, TAB_X, BUTTON_Y + 2, 150, "Show this window at start");
	control = add_control(layout, _control_button, _page_none, -1, DIALOG_WIDTH - TAB_X - 3 * BUTTON_WIDTH - 20,
		BUTTON_Y, BUTTON_WIDTH, BUTTON_HEIGHT, "", BS_PUSHBUTTON | WS_TABSTOP, "Defaults");
	if (control)
		control->id = _id_defaults;
	control = add_control(layout, _control_button, _page_none, -1, DIALOG_WIDTH - TAB_X - 2 * BUTTON_WIDTH - 6,
		BUTTON_Y, BUTTON_WIDTH, BUTTON_HEIGHT, "", BS_DEFPUSHBUTTON | WS_TABSTOP, "Play");
	if (control)
		control->id = _id_play;
	control = add_control(layout, _control_button, _page_none, -1, DIALOG_WIDTH - TAB_X - BUTTON_WIDTH,
		BUTTON_Y, BUTTON_WIDTH, BUTTON_HEIGHT, "", BS_PUSHBUTTON | WS_TABSTOP, "Quit");
	if (control)
		control->id = _id_quit;
	control = add_control(layout, _control_static, _page_none, -1, TAB_X, BUTTON_Y + 16, TAB_WIDTH, 8, "",
		SS_LEFT | SS_NOPREFIX, "The settings are kept in config.toml next to the game; -nolauncher skips this window.");
	if (control)
		control->id = _id_note;
	/* (last: the list takes what is left) */
	layout_advanced_page(layout, settings, count);
}

/* ---------- the dialog template */

struct template_writer
{
	unsigned char *buffer;
	size_t size, used;
	int overflow;
};

static void template_word(struct template_writer *writer, unsigned value)
{
	if (writer->used + 2 > writer->size)
	{
		writer->overflow = 1;
		return;
	}
	writer->buffer[writer->used++] = (unsigned char)(value & 0xff);
	writer->buffer[writer->used++] = (unsigned char)((value >> 8) & 0xff);
}

static void template_dword(struct template_writer *writer, unsigned long value)
{
	template_word(writer, (unsigned)(value & 0xffff));
	template_word(writer, (unsigned)((value >> 16) & 0xffff));
}

/* a string as the template wants it: UTF-16, NUL terminated (ASCII here) */
static void template_string(struct template_writer *writer, const char *text)
{
	for (; *text; text++)
		template_word(writer, (unsigned char)*text);
	template_word(writer, 0);
}

static void template_align(struct template_writer *writer)
{
	while (writer->used & 3)
	{
		if (writer->used >= writer->size)
		{
			writer->overflow = 1;
			return;
		}
		writer->buffer[writer->used++] = 0;
	}
}

/* the atom of a standard class, 0 for one named */
static unsigned class_atom(const struct launcher_control *control)
{
	switch (control->kind)
	{
	case _control_static: return 0x0082;
	case _control_button: case _control_checkbox: return 0x0080;
	case _control_edit: return 0x0081;
	case _control_combo_list: case _control_combo_edit: return 0x0085;
	case _control_listbox: return 0x0083;
	default: return 0;
	}
}

/* the DLGTEMPLATE of the layout; the bytes it takes, 0 if size is too
small */
static size_t launcher_template(const struct launcher_layout *layout, unsigned char *buffer, size_t size)
{
	struct template_writer writer = { buffer, size, 0, 0 };
	int index;

	template_dword(&writer, DS_SETFONT | DS_MODALFRAME | DS_CENTER | DS_3DLOOK | WS_POPUP | WS_CAPTION | WS_SYSMENU);
	template_dword(&writer, 0);
	template_word(&writer, (unsigned)layout->control_count);
	template_word(&writer, 0);
	template_word(&writer, 0);
	template_word(&writer, DIALOG_WIDTH);
	template_word(&writer, DIALOG_HEIGHT);
	template_word(&writer, 0);
	template_word(&writer, 0);
	template_string(&writer, "Halo");
	template_word(&writer, 8);
	template_string(&writer, "MS Shell Dlg");
	for (index = 0; index < layout->control_count; index++)
	{
		const struct launcher_control *control = &layout->controls[index];
		unsigned atom = class_atom(control);

		template_align(&writer);
		template_dword(&writer, control->style);
		template_dword(&writer, 0);
		template_word(&writer, (unsigned)(unsigned short)control->x);
		template_word(&writer, (unsigned)(unsigned short)control->y);
		template_word(&writer, (unsigned)(unsigned short)control->width);
		template_word(&writer, (unsigned)(unsigned short)control->height);
		template_word(&writer, (unsigned)control->id);
		if (atom)
		{
			template_word(&writer, 0xffff);
			template_word(&writer, atom);
		}
		else
		{
			template_string(&writer, control->class_name);
		}
		template_string(&writer, control->text ? control->text : "");
		template_word(&writer, 0);
	}
	return writer.overflow ? 0 : writer.used;
}

#ifndef LAUNCHER_LAYOUT_ONLY

/* ---------- the dialog */

struct launcher_dialog
{
	struct launcher_setting *settings;
	int count;
	struct launcher_layout layout;
	HWND window;
	HWND tooltips;
	/* the Advanced list's values as edited */
	char *working[MAXIMUM_SETTINGS];
	/* the field of view's slider position at the start, so an untouched
	slider keeps the value's own spelling */
	int field_of_view_start;
	/* the list's edit is being filled by the dialog, not the player */
	int filling;
};

static struct launcher_dialog dialog;

/* whether text is one of the words, whatever its case */
static int text_is_one_of(const char *text, const char *const *words, int count)
{
	int index;

	for (index = 0; index < count; index++)
	{
		const char *a = text, *b = words[index];

		while (*a && *b && ((*a >= 'A' && *a <= 'Z' ? *a - 'A' + 'a' : *a) == *b))
		{
			a++;
			b++;
		}
		if (!*a && !*b)
			return 1;
	}
	return 0;
}

/* port_config.c's spellings of a boolean */
static int text_is_false(const char *text)
{
	static const char *const words[] = { "0", "false", "no", "off" };

	return text_is_one_of(text, words, 4);
}

static int text_is_true(const char *text)
{
	static const char *const words[] = { "1", "true", "yes", "on" };

	return text_is_one_of(text, words, 4);
}

/* why text is no value of the setting's type; NULL when it is one */
static const char *validate(const struct launcher_setting *setting, const char *text)
{
	char *end;

	switch (setting->type)
	{
	case _launcher_boolean:
		return text_is_true(text) || text_is_false(text) ? NULL : "should be true or false";
	case _launcher_integer:
		strtol(text, &end, 10);
		return *text && !*end ? NULL : "should be a whole number";
	case _launcher_real:
		strtod(text, &end);
		return *text && !*end ? NULL : "should be a number";
	default:
		return NULL;
	}
}

static struct launcher_control *control_of_setting(int setting)
{
	int index = dialog.layout.control_of_setting[setting];

	return index >= 0 ? &dialog.layout.controls[index] : NULL;
}

static struct launcher_control *control_by_id(int id)
{
	int index;

	for (index = 0; index < dialog.layout.control_count; index++)
	{
		if (dialog.layout.controls[index].id == id)
			return &dialog.layout.controls[index];
	}
	return NULL;
}

static void window_set_text(HWND window, const char *text)
{
	SendMessageA(window, WM_SETTEXT, 0, (LPARAM)text);
}

static void window_get_text(HWND window, char *text, int size)
{
	text[0] = 0;
	SendMessageA(window, WM_GETTEXT, (WPARAM)size, (LPARAM)text);
	text[size - 1] = 0;
}

static void field_of_view_show(int degrees)
{
	char text[32];

	snprintf(text, sizeof(text), "%d degrees", degrees);
	window_set_text(GetDlgItem(dialog.window, _id_field_of_view_value), text);
}

static void renderer_explain(const struct launcher_control *control)
{
	int selected = (int)SendMessageA(control->window, CB_GETCURSEL, 0, 0);
	const char *text = "";

	if (selected >= 0 && selected < (int)(sizeof(renderer_explanations) / sizeof(renderer_explanations[0])))
		text = renderer_explanations[selected];
	window_set_text(GetDlgItem(dialog.window, _id_renderer_explanation), text);
}

/* puts a value into a control */
static void control_set(struct launcher_control *control, const char *text)
{
	int index;

	switch (control->kind)
	{
	case _control_checkbox:
		SendMessageA(control->window, BM_SETCHECK, text_is_false(text) ? BST_UNCHECKED : BST_CHECKED, 0);
		break;
	case _control_edit:
	case _control_combo_edit:
		window_set_text(control->window, text);
		break;
	case _control_combo_list:
		for (index = 0; index < control->choice_count; index++)
		{
			if (!strcmp(control->choices[index], text))
				break;
		}
		if (index == control->choice_count)
		{
			/* not one of the choices: one more entry, as it is */
			int entries = (int)SendMessageA(control->window, CB_GETCOUNT, 0, 0);
			char entry[256];

			for (index = control->choice_count; index < entries; index++)
			{
				entry[0] = 0;
				if (SendMessageA(control->window, CB_GETLBTEXTLEN, (WPARAM)index, 0) < (LRESULT)sizeof(entry))
					SendMessageA(control->window, CB_GETLBTEXT, (WPARAM)index, (LPARAM)entry);
				if (!strcmp(entry, text))
					break;
			}
			if (index == entries)
				index = (int)SendMessageA(control->window, CB_ADDSTRING, 0, (LPARAM)text);
		}
		SendMessageA(control->window, CB_SETCURSEL, (WPARAM)index, 0);
		if (control->setting >= 0 && !strcmp(dialog.settings[control->setting].name, "display.renderer"))
			renderer_explain(control);
		break;
	case _control_trackbar:
	{
		int degrees = (int)(strtod(text, NULL) + 0.5);

		if (degrees < control->minimum)
			degrees = control->minimum;
		if (degrees > control->maximum)
			degrees = control->maximum;
		SendMessageA(control->window, LAUNCHER_TBM_SETPOS, TRUE, (LPARAM)degrees);
		field_of_view_show(degrees);
		break;
	}
	default:
		break;
	}
}

/* the value a control holds, as text */
static void control_get(const struct launcher_control *control, char *text, int size)
{
	int index;

	switch (control->kind)
	{
	case _control_checkbox:
		snprintf(text, (size_t)size, "%s", SendMessageA(control->window, BM_GETCHECK, 0, 0) == BST_CHECKED ?
			"true" : "false");
		break;
	case _control_edit:
		window_get_text(control->window, text, size);
		break;
	case _control_combo_edit:
		window_get_text(control->window, text, size);
		/* a choice picked from the list leaves its label in the edit: the
		value it stands for */
		for (index = 0; control->choice_labels && index < control->choice_count; index++)
		{
			if (!strcmp(control->choice_labels[index], text))
			{
				snprintf(text, (size_t)size, "%s", control->choices[index]);
				break;
			}
		}
		break;
	case _control_combo_list:
		index = (int)SendMessageA(control->window, CB_GETCURSEL, 0, 0);
		if (index >= 0 && index < control->choice_count)
		{
			snprintf(text, (size_t)size, "%s", control->choices[index]);
		}
		else if (index >= 0 && SendMessageA(control->window, CB_GETLBTEXTLEN, (WPARAM)index, 0) < (LRESULT)size)
		{
			text[0] = 0;
			SendMessageA(control->window, CB_GETLBTEXT, (WPARAM)index, (LPARAM)text);
		}
		else
		{
			text[0] = 0;
		}
		break;
	case _control_trackbar:
	{
		int degrees = (int)SendMessageA(control->window, LAUNCHER_TBM_GETPOS, 0, 0);

		if (degrees == dialog.field_of_view_start)
			snprintf(text, (size_t)size, "%s", dialog.settings[control->setting].value);
		else
			snprintf(text, (size_t)size, "%d.0", degrees);
		break;
	}
	default:
		text[0] = 0;
		break;
	}
}

static void advanced_list_refresh(int row, int select)
{
	HWND list = GetDlgItem(dialog.window, _id_advanced_list);
	int setting = dialog.layout.advanced[row];
	char line[1200];

	snprintf(line, sizeof(line), "%s\t%s", dialog.settings[setting].name, dialog.working[setting]);
	SendMessageA(list, LB_DELETESTRING, (WPARAM)row, 0);
	SendMessageA(list, LB_INSERTSTRING, (WPARAM)row, (LPARAM)line);
	if (select)
		SendMessageA(list, LB_SETCURSEL, (WPARAM)row, 0);
}

/* the list's selection into the edit and the description */
static void advanced_select(void)
{
	HWND list = GetDlgItem(dialog.window, _id_advanced_list);
	int row = (int)SendMessageA(list, LB_GETCURSEL, 0, 0);
	const struct launcher_setting *setting;
	char description[1024];

	if (row < 0 || row >= dialog.layout.advanced_count)
	{
		dialog.filling = 1;
		window_set_text(GetDlgItem(dialog.window, _id_advanced_edit), "");
		dialog.filling = 0;
		window_set_text(GetDlgItem(dialog.window, _id_advanced_description), "");
		EnableWindow(GetDlgItem(dialog.window, _id_advanced_edit), FALSE);
		EnableWindow(GetDlgItem(dialog.window, _id_advanced_default), FALSE);
		return;
	}
	setting = &dialog.settings[dialog.layout.advanced[row]];
	EnableWindow(GetDlgItem(dialog.window, _id_advanced_edit), TRUE);
	EnableWindow(GetDlgItem(dialog.window, _id_advanced_default), TRUE);
	dialog.filling = 1;
	window_set_text(GetDlgItem(dialog.window, _id_advanced_edit), dialog.working[dialog.layout.advanced[row]]);
	dialog.filling = 0;
	snprintf(description, sizeof(description), "%s (%s; default %s%s%s)\r\n%s", setting->name,
		setting->type == _launcher_boolean ? "true or false" : setting->type == _launcher_integer ? "a whole number" :
		setting->type == _launcher_real ? "a number" : "text", setting->type == _launcher_string ? "\"" : "",
		setting->default_text, setting->type == _launcher_string ? "\"" : "", setting->comment);
	window_set_text(GetDlgItem(dialog.window, _id_advanced_description), description);
}

/* the edit's text into the selected setting's working value and its line */
static void advanced_edited(void)
{
	HWND list = GetDlgItem(dialog.window, _id_advanced_list);
	int row = (int)SendMessageA(list, LB_GETCURSEL, 0, 0);
	int setting;

	if (dialog.filling || row < 0 || row >= dialog.layout.advanced_count)
		return;
	setting = dialog.layout.advanced[row];
	window_get_text(GetDlgItem(dialog.window, _id_advanced_edit), dialog.working[setting],
		dialog.settings[setting].value_size);
	/* (the list redraws the line; the selection is kept, without another
	selection notice: the edit keeps the focus) */
	SendMessageA(list, WM_SETREDRAW, FALSE, 0);
	advanced_list_refresh(row, 1);
	SendMessageA(list, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(list, NULL, TRUE);
}

static void show_page(int page)
{
	int index;

	for (index = 0; index < dialog.layout.control_count; index++)
	{
		const struct launcher_control *control = &dialog.layout.controls[index];

		if (control->page != _page_none)
			ShowWindow(control->window, control->page == page ? SW_SHOW : SW_HIDE);
	}
}

static void tooltip_add(HWND window, const char *text)
{
	struct launcher_tool_info tool;

	if (!dialog.tooltips || !window)
		return;
	memset(&tool, 0, sizeof(tool));
	tool.size = sizeof(tool);
	tool.flags = LAUNCHER_TTF_IDISHWND | LAUNCHER_TTF_SUBCLASS;
	tool.window = dialog.window;
	tool.id = (UINT_PTR)window;
	tool.text = (LPSTR)text;
	SendMessageA(dialog.tooltips, LAUNCHER_TTM_ADDTOOLA, 0, (LPARAM)&tool);
}

static void dialog_fill(void)
{
	int index, setting;

	for (index = 0; index < dialog.layout.control_count; index++)
	{
		struct launcher_control *control = &dialog.layout.controls[index];
		int entry;

		control->window = GetDlgItem(dialog.window, control->id);
		if (!control->window)
			continue;
		switch (control->kind)
		{
		case _control_combo_list:
		case _control_combo_edit:
			for (entry = 0; entry < control->choice_count; entry++)
			{
				SendMessageA(control->window, CB_ADDSTRING, 0,
					(LPARAM)(control->choice_labels ? control->choice_labels[entry] : control->choices[entry]));
			}
			break;
		case _control_trackbar:
			SendMessageA(control->window, LAUNCHER_TBM_SETRANGE, FALSE,
				MAKELPARAM(control->minimum, control->maximum));
			SendMessageA(control->window, LAUNCHER_TBM_SETTICFREQ, 10, 0);
			SendMessageA(control->window, LAUNCHER_TBM_SETPAGESIZE, 0, 5);
			break;
		case _control_edit:
			SendMessageA(control->window, EM_LIMITTEXT, control->setting >= 0 ?
				(WPARAM)(dialog.settings[control->setting].value_size - 1) : 1023, 0);
			break;
		default:
			break;
		}
		if (control->setting >= 0)
			tooltip_add(control->window, dialog.settings[control->setting].comment);
	}
	for (setting = 0; setting < dialog.count; setting++)
	{
		struct launcher_control *control = control_of_setting(setting);

		if (control && control->kind == _control_trackbar)
			dialog.field_of_view_start = (int)(strtod(dialog.settings[setting].value, NULL) + 0.5);
		if (control)
			control_set(control, dialog.settings[setting].value);
	}
	/* the Advanced list */
	{
		HWND list = GetDlgItem(dialog.window, _id_advanced_list);
		/* the values' column, in dialog units (as LB_SETTABSTOPS takes) */
		int tab_stop = 112;
		int row;

		SendMessageA(list, LB_SETTABSTOPS, 1, (LPARAM)&tab_stop);
		for (row = 0; row < dialog.layout.advanced_count; row++)
		{
			char line[1200];

			snprintf(line, sizeof(line), "%s\t%s", dialog.settings[dialog.layout.advanced[row]].name,
				dialog.working[dialog.layout.advanced[row]]);
			SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)line);
		}
		SendMessageA(GetDlgItem(dialog.window, _id_advanced_edit), EM_LIMITTEXT, 1023, 0);
		if (dialog.layout.advanced_count)
			SendMessageA(list, LB_SETCURSEL, 0, 0);
		advanced_select();
	}
}

static void dialog_defaults(void)
{
	int setting;

	for (setting = 0; setting < dialog.count; setting++)
	{
		struct launcher_control *control = control_of_setting(setting);

		if (control)
		{
			control_set(control, dialog.settings[setting].default_text);
		}
		else
		{
			int row;

			snprintf(dialog.working[setting], (size_t)dialog.settings[setting].value_size, "%s",
				dialog.settings[setting].default_text);
			for (row = 0; row < dialog.layout.advanced_count; row++)
			{
				if (dialog.layout.advanced[row] == setting)
					advanced_list_refresh(row, 0);
			}
		}
	}
	advanced_select();
}

/* every value checked and taken; FALSE (with the box said) when one is no
value of its type */
static int dialog_collect(void)
{
	int setting, row;

	for (setting = 0; setting < dialog.count; setting++)
	{
		const struct launcher_control *control = control_of_setting(setting);
		struct launcher_setting *definition = &dialog.settings[setting];
		char text[1024];
		const char *problem;

		if (control)
			control_get(control, text, (int)sizeof(text));
		else
			snprintf(text, sizeof(text), "%s", dialog.working[setting]);
		problem = validate(definition, text);
		if (problem)
		{
			char message[256];

			snprintf(message, sizeof(message), "%s %s (it is \"%s\").", definition->name, problem, text);
			if (control)
			{
				SendMessageA(GetDlgItem(dialog.window, _id_tab), LAUNCHER_TCM_SETCURSEL, (WPARAM)control->page, 0);
				show_page(control->page);
				SetFocus(control->window);
			}
			else
			{
				HWND list = GetDlgItem(dialog.window, _id_advanced_list);

				SendMessageA(GetDlgItem(dialog.window, _id_tab), LAUNCHER_TCM_SETCURSEL, _page_advanced, 0);
				show_page(_page_advanced);
				for (row = 0; row < dialog.layout.advanced_count; row++)
				{
					if (dialog.layout.advanced[row] == setting)
						SendMessageA(list, LB_SETCURSEL, (WPARAM)row, 0);
				}
				advanced_select();
				SetFocus(GetDlgItem(dialog.window, _id_advanced_edit));
			}
			MessageBoxA(dialog.window, message, "Halo", MB_OK | MB_ICONEXCLAMATION);
			return 0;
		}
		snprintf(definition->value, (size_t)definition->value_size, "%s", text);
	}
	return 1;
}

static INT_PTR CALLBACK dialog_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message)
	{
	case WM_INITDIALOG:
	{
		HWND tab = GetDlgItem(window, _id_tab);
		HICON icon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(1));
		int page;

		dialog.window = window;
		if (icon)
			SendMessageA(window, WM_SETICON, 1, (LPARAM)icon);
		for (page = 0; page < NUMBER_OF_PAGES; page++)
		{
			struct launcher_tab_item item;

			memset(&item, 0, sizeof(item));
			item.mask = LAUNCHER_TCIF_TEXT;
			item.text = (LPSTR)page_names[page];
			SendMessageA(tab, LAUNCHER_TCM_INSERTITEMA, (WPARAM)page, (LPARAM)&item);
		}
		dialog.tooltips = CreateWindowExA(0, LAUNCHER_TOOLTIPS_CLASS, NULL, WS_POPUP | LAUNCHER_TTS_ALWAYSTIP,
			CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window, NULL, GetModuleHandleA(NULL), NULL);
		if (dialog.tooltips)
		{
			SendMessageA(dialog.tooltips, LAUNCHER_TTM_SETMAXTIPWIDTH, 0, 420);
			SendMessageA(dialog.tooltips, LAUNCHER_TTM_SETDELAYTIME, LAUNCHER_TTDT_AUTOPOP, 30000);
		}
		dialog_fill();
		show_page(_page_display);
		return TRUE;
	}
	case WM_NOTIFY:
	{
		const NMHDR *header = (const NMHDR *)lparam;

		if (header->idFrom == _id_tab && header->code == LAUNCHER_TCN_SELCHANGE)
			show_page((int)SendMessageA(header->hwndFrom, LAUNCHER_TCM_GETCURSEL, 0, 0));
		return FALSE;
	}
	case WM_HSCROLL:
	{
		const struct launcher_control *control;
		int index;

		for (index = 0; index < dialog.layout.control_count; index++)
		{
			control = &dialog.layout.controls[index];
			if (control->kind == _control_trackbar && control->window == (HWND)lparam)
				field_of_view_show((int)SendMessageA(control->window, LAUNCHER_TBM_GETPOS, 0, 0));
		}
		return TRUE;
	}
	case WM_COMMAND:
	{
		int id = LOWORD(wparam), notice = HIWORD(wparam);
		const struct launcher_control *control;

		switch (id)
		{
		case _id_play:
			if (dialog_collect())
				EndDialog(window, 1);
			return TRUE;
		case _id_quit:
			EndDialog(window, 0);
			return TRUE;
		case _id_defaults:
			dialog_defaults();
			return TRUE;
		case _id_advanced_list:
			if (notice == LBN_SELCHANGE)
				advanced_select();
			return TRUE;
		case _id_advanced_edit:
			if (notice == EN_CHANGE)
				advanced_edited();
			return TRUE;
		case _id_advanced_default:
		{
			HWND list = GetDlgItem(window, _id_advanced_list);
			int row = (int)SendMessageA(list, LB_GETCURSEL, 0, 0);

			if (row >= 0 && row < dialog.layout.advanced_count)
			{
				window_set_text(GetDlgItem(window, _id_advanced_edit),
					dialog.settings[dialog.layout.advanced[row]].default_text);
			}
			return TRUE;
		}
		default:
			break;
		}
		control = control_by_id(id);
		if (control && control->kind == _control_combo_list && notice == CBN_SELCHANGE && control->setting >= 0 &&
			!strcmp(dialog.settings[control->setting].name, "display.renderer"))
		{
			renderer_explain(control);
		}
		return FALSE;
	}
	default:
		return FALSE;
	}
}

#ifndef HALO_WIN9X
/* The native Windows build: SDL declares the process aware of the
display's dpi when it starts its video, and Windows refuses that once the
process has made a window; shown first, this dialog would leave the whole
game drawn at 96 dpi and scaled up, blurred, on a display set above it.
So the same declaration comes before the dialog, in SDL's order
(SDL_windowsvideo.c): Windows 10's per-monitor contexts, 8.1's
SetProcessDpiAwareness, Vista's SetProcessDPIAware, each looked up by name
so that older Windows runs on. The dialog then scales with the display as
the dialog manager scales a template's units by its font. */
static void declare_dpi_awareness(void)
{
	typedef BOOL (WINAPI *set_context_function)(HANDLE context);
	typedef LONG (WINAPI *set_awareness_function)(int awareness);
	typedef BOOL (WINAPI *set_aware_function)(void);
	HMODULE user32 = GetModuleHandleA("user32.dll");
	HMODULE shcore = LoadLibraryA("shcore.dll");
	set_context_function set_context = user32 ?
		(set_context_function)GetProcAddress(user32, "SetProcessDpiAwarenessContext") : NULL;
	set_awareness_function set_awareness = shcore ?
		(set_awareness_function)GetProcAddress(shcore, "SetProcessDpiAwareness") : NULL;
	set_aware_function set_aware = user32 ? (set_aware_function)GetProcAddress(user32, "SetProcessDPIAware") : NULL;
	int declared = 0;

	/* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is -4 and
	DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE -3;
	PROCESS_PER_MONITOR_DPI_AWARE is 2 */
	if (set_context && (set_context((HANDLE)(INT_PTR)-4) || set_context((HANDLE)(INT_PTR)-3)))
		declared = 1;
	else if (set_awareness && set_awareness(2) >= 0)
		declared = 1;
	else if (set_aware && set_aware())
		declared = 1;
	if (shcore)
		FreeLibrary(shcore);
	/* (refused when it is already declared: by SDL, when a window of the
	game's came first, or a manifest) */
	if (!declared)
		platform_log("launcher: dpi awareness not declared here (error %lu)", (unsigned long)GetLastError());
}
#endif

int launcher_show(struct launcher_setting *settings, int count)
{
	typedef void (WINAPI *init_common_controls_function)(void);
	HMODULE comctl = LoadLibraryA("comctl32.dll");
	init_common_controls_function init_common_controls;
	unsigned char *template_buffer;
	size_t template_size;
	INT_PTR result;
	int setting;

	if (count > MAXIMUM_SETTINGS)
		count = MAXIMUM_SETTINGS;
#ifndef HALO_WIN9X
	declare_dpi_awareness();
#endif
	init_common_controls = comctl ? (init_common_controls_function)GetProcAddress(comctl, "InitCommonControls") :
		NULL;
	if (!init_common_controls)
	{
		platform_log("launcher: no comctl32.dll: the settings window cannot be shown");
		return 1;
	}
	init_common_controls();
	memset(&dialog, 0, sizeof(dialog));
	dialog.settings = settings;
	dialog.count = count;
	launcher_layout(&dialog.layout, settings, count);
	for (setting = 0; setting < count; setting++)
	{
		dialog.working[setting] = malloc((size_t)settings[setting].value_size);
		if (dialog.working[setting])
			snprintf(dialog.working[setting], (size_t)settings[setting].value_size, "%s", settings[setting].value);
	}
	template_buffer = malloc(65536);
	template_size = template_buffer ? launcher_template(&dialog.layout, template_buffer, 65536) : 0;
	if (!template_size)
	{
		platform_log("launcher: the settings window could not be built");
		result = 1;
	}
	else
	{
		result = DialogBoxIndirectParamA(GetModuleHandleA(NULL), (LPCDLGTEMPLATEA)template_buffer, NULL,
			dialog_procedure, 0);
		if (result == -1)
		{
			platform_log("launcher: the settings window could not be shown (error %lu)", (unsigned long)GetLastError());
			result = 1;
		}
	}
	free(template_buffer);
	for (setting = 0; setting < count; setting++)
		free(dialog.working[setting]);
	return result ? 1 : 0;
}

#endif
