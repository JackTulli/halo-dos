/*
T_LAUNCHER.C

The launcher's dialog template (port/win9x/platform/w9x_launcher.c) built
for a list of settings and taken apart again, for launcher_check.py: the
settings come one a line on standard input (name, type, default, tab
separated; the type as port_config.c numbers them, 0 boolean to 3 string),
and the output lists the dialog, every item of the template as the bytes
say (id, place, class, style, text), which setting each control of the
layout edits, and the settings the Advanced list holds. The script checks
the places, the ids and that every setting is bound once.

Built on the PC with the Windows SDK (the README next to this file); it
opens no window.
*/

#define LAUNCHER_LAYOUT_ONLY 1
#include "../../platform/w9x_launcher.c"

void platform_log(const char *format, ...)
{
	(void)format;
}

static struct launcher_setting settings[MAXIMUM_SETTINGS];
static char names[MAXIMUM_SETTINGS][64];
static char defaults[MAXIMUM_SETTINGS][128];
static char values[MAXIMUM_SETTINGS][64];
static struct launcher_layout layout;
static unsigned char template_buffer[65536];

static unsigned read_word(const unsigned char **cursor)
{
	unsigned value = (unsigned)(*cursor)[0] | ((unsigned)(*cursor)[1] << 8);

	*cursor += 2;
	return value;
}

static unsigned long read_dword(const unsigned char **cursor)
{
	unsigned long low = read_word(cursor);

	return low | ((unsigned long)read_word(cursor) << 16);
}

/* a UTF-16 string of the template into text (ASCII) */
static void read_string(const unsigned char **cursor, char *text, size_t size)
{
	size_t used = 0;

	for (;;)
	{
		unsigned character = read_word(cursor);

		if (!character)
			break;
		if (used + 1 < size)
			text[used++] = (char)(character < 0x80 ? character : '?');
	}
	text[used] = 0;
}

static const char *kind_name(enum control_kind kind)
{
	switch (kind)
	{
	case _control_static: return "static";
	case _control_button: return "button";
	case _control_checkbox: return "checkbox";
	case _control_edit: return "edit";
	case _control_combo_list: return "combo_list";
	case _control_combo_edit: return "combo_edit";
	case _control_trackbar: return "trackbar";
	case _control_listbox: return "listbox";
	case _control_tab: return "tab";
	}
	return "?";
}

int main(void)
{
	char line[512];
	int count = 0;
	size_t size;
	const unsigned char *cursor;
	unsigned items, index;
	int x, y, width, height;

	while (count < MAXIMUM_SETTINGS && fgets(line, sizeof(line), stdin))
	{
		char *fields[3];
		char *text = line;
		int field;

		line[strcspn(line, "\r\n")] = 0;
		if (!line[0])
			continue;
		for (field = 0; field < 3; field++)
		{
			fields[field] = text;
			text = strchr(text, '\t');
			if (text)
				*text++ = 0;
			else if (field < 2)
				break;
		}
		if (field < 3)
		{
			fprintf(stderr, "bad line: %s\n", line);
			return 2;
		}
		snprintf(names[count], sizeof(names[count]), "%s", fields[0]);
		snprintf(defaults[count], sizeof(defaults[count]), "%s", fields[2]);
		snprintf(values[count], sizeof(values[count]), "%s", fields[2]);
		settings[count].name = names[count];
		settings[count].type = (enum launcher_setting_type)atoi(fields[1]);
		settings[count].default_text = defaults[count];
		settings[count].comment = "";
		settings[count].value = values[count];
		settings[count].value_size = (int)sizeof(values[count]);
		count++;
	}
	launcher_layout(&layout, settings, count);
	size = launcher_template(&layout, template_buffer, sizeof(template_buffer));
	if (!size)
	{
		fprintf(stderr, "the template does not fit\n");
		return 2;
	}
	/* the header */
	cursor = template_buffer;
	printf("style\t%08lx\n", read_dword(&cursor));
	read_dword(&cursor);
	items = read_word(&cursor);
	x = (short)read_word(&cursor);
	y = (short)read_word(&cursor);
	width = (short)read_word(&cursor);
	height = (short)read_word(&cursor);
	printf("dialog\t%d\t%d\t%d\t%d\t%u\t%lu\n", x, y, width, height, items, (unsigned long)size);
	read_word(&cursor); /* menu */
	read_word(&cursor); /* class */
	read_string(&cursor, line, sizeof(line));
	printf("title\t%s\n", line);
	printf("font\t%u\t", read_word(&cursor));
	read_string(&cursor, line, sizeof(line));
	printf("%s\n", line);
	/* the items, as the bytes say */
	for (index = 0; index < items; index++)
	{
		unsigned long style;
		unsigned id, atom = 0;
		char class_name[64], text[512];

		while ((cursor - template_buffer) & 3)
			cursor++;
		style = read_dword(&cursor);
		read_dword(&cursor);
		x = (short)read_word(&cursor);
		y = (short)read_word(&cursor);
		width = (short)read_word(&cursor);
		height = (short)read_word(&cursor);
		id = read_word(&cursor);
		if (cursor[0] == 0xff && cursor[1] == 0xff)
		{
			read_word(&cursor);
			atom = read_word(&cursor);
			snprintf(class_name, sizeof(class_name), "atom%04x", atom);
		}
		else
		{
			read_string(&cursor, class_name, sizeof(class_name));
		}
		if (cursor[0] == 0xff && cursor[1] == 0xff)
		{
			read_word(&cursor);
			snprintf(text, sizeof(text), "#%u", read_word(&cursor));
		}
		else
		{
			read_string(&cursor, text, sizeof(text));
		}
		read_word(&cursor); /* creation data */
		printf("item\t%u\t%d\t%d\t%d\t%d\t%s\t%08lx\t%s\n", id, x, y, width, height, class_name, style, text);
	}
	if ((size_t)(cursor - template_buffer) != size)
	{
		fprintf(stderr, "the template's size is %lu, its items end at %lu\n", (unsigned long)size,
			(unsigned long)(cursor - template_buffer));
		return 2;
	}
	/* the layout's view: the page and the setting of each control */
	for (index = 0; (int)index < layout.control_count; index++)
	{
		const struct launcher_control *control = &layout.controls[index];

		printf("control\t%d\t%d\t%s\t%s\n", control->id, (int)control->page, kind_name(control->kind),
			control->setting >= 0 ? settings[control->setting].name : "-");
	}
	for (index = 0; (int)index < layout.advanced_count; index++)
		printf("advanced\t%s\n", settings[layout.advanced[index]].name);
	return 0;
}
