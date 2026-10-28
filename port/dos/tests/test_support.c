/*
TEST_SUPPORT.C

What the DOS units ask of the game that a test program has not: the
settings (port/dos/xdk/dos_settings.c), each its default.
*/

int dos_setting_boolean(const char *name, int default_value)
{
	(void)name;
	return default_value;
}

const char *dos_setting(const char *name, const char *default_value)
{
	(void)name;
	return default_value;
}

long dos_setting_integer(const char *name, long default_value)
{
	(void)name;
	return default_value;
}

/* (internet play's state, which the start-up clears: none here) */
void dos_p2p_state_initialize(void)
{
}
