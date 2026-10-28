/*
DOS_MAP_DOWNLOAD.H

A Custom Edition map downloaded into MAPS_CE from Chimera's map repository
(dos_map_download.c), polled each frame.
*/

#ifndef __DOS_MAP_DOWNLOAD_H
#define __DOS_MAP_DOWNLOAD_H

enum
{
	DOS_MAP_DOWNLOAD_OFF,
	DOS_MAP_DOWNLOAD_CONNECTING,
	DOS_MAP_DOWNLOAD_DOWNLOADING,
	DOS_MAP_DOWNLOAD_UNPACKING,
	DOS_MAP_DOWNLOAD_DONE,
	DOS_MAP_DOWNLOAD_FAILED /* error says why */
};

struct dos_map_download_status
{
	int state;
	char map[64];
	/* bytes come and to come (-1: the server does not say), their rate */
	long received, total, bytes_per_second;
	/* the unpacking: bytes made and to make */
	long unpacked, unpacked_total;
	/* where it comes from */
	char host[96];
	char error[96];
};

/* starts downloading the map (its name as the server has it): FALSE if it
could not start (the status says why) */
int dos_map_download_begin(const char *map);
void dos_map_download_poll(void);
void dos_map_download_cancel(void);
void dos_map_download_status(struct dos_map_download_status *status);

#endif
