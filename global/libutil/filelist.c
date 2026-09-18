/*
 * Copyright (c) 2014
 *	Tama Communications Corporation
 *
 * This file is part of GNU Global.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#include "checkalloc.h"
#include "die.h"
#include "filelist.h"
#include "locatestring.h"
#include "strbuf.h"
/*
 * There are two formats for the file list:
 *
 * 1. file name only
 *	<path> \n
 * 2. file ID + file name
 *	<fid> \t <path> \n 
 */
/*
 * filelist_open: load a file list
 *
 *	i)	file	file name
 *			NULL: stdin
 *	r)		filelist structure	
 *			NULL: file not found
 */
FILELIST *
filelist_open(const char *file)
{
	FILELIST *fl;
	int size, fd;

	if (file == NULL) {
		size = fd = 0;
	} else {
		struct stat st;

		if (stat(file, &st) < 0)
			return NULL;
		size = st.st_size;
		fd = open(file, 0);
		if (fd < 0)
			return NULL;
	}
	fl = check_calloc(sizeof(FILELIST), 1);
	if (fd > 0) {
		fl->start = check_malloc(size);
		if (read(fd, fl->start, size) != size) {
			free(fl);
			return NULL;
		}
		close(fd);
	} else {
		fl->ip = stdin;
		fl->sb = strbuf_open(0);
	}
	if (fd > 0) {
		fl->end = fl->start + size;
		fl->current = fl->start;
		fl->count = 0;
		for (char *p = fl->start; p < fl->end; p++)
			if (*p == '\n')
				fl->count++;
	}
	return fl;
}
/*
 * filelist_open_withargs: load file lists from multiple files.
 *
 *	i)	argc	argument count
 *	i)	argv	file names
 *	r)		filelist structure	
 *			NULL: file not found
 *
 */
#define MAXFILELIST 5
FILELIST *
filelist_open_withargs(int argc, char *argv[])
{
	int i, total, offset, size[MAXFILELIST];
	struct stat st;
	FILELIST *fl;

	/*
	 * If no file is specified, it reads from standard input.
	 */
	if (argc == 0)
		return filelist_open(NULL);
	/*
	 * Path 1: Checking for file existence and size.
	 */
	total = 0;
	for (i = 0; i < argc; i++) {
		const char *file = argv[i];

		if (stat(file, &st) < 0)
			die("cannot stat filelist '%s'.", file);
		size[i] = st.st_size;
		total += st.st_size;
	}
	/*
	 * Allocating a buffer for bulk loading.
	 */
	fl = check_calloc(sizeof(FILELIST), 1);
	fl->start = check_malloc(total);
	offset = 0;
	for (i = 0; i < argc; i++) {
		const char *file = argv[i];
		if (size[i] == 0)
			continue;
		int fd = open(file, 0);

		if (fd < 0)
			die("cannot open filelist '%s'.", file);
		if (read(fd, fl->start + offset, size[i]) != size[i])
			die("unexpected end of file. (%s)", file);
		close(fd);
		offset += size[i];
	}
	fl->end = fl->start + total;
	fl->current = fl->start;
	/*
	 * Counting lines.
	 */
	fl->count = 0;
	for (char *p = fl->start; p < fl->end; p++)
		if (*p == '\n')
			fl->count++;
	return fl;
}
/*
 * filelist_read: read next path of the filelist.
 *
 *	i)	fl	filelist structure	
 *	o)	sb	string buffer
 *	r)		path
 *
 * String buffers are used in multi-threading.
 * A buffer common to all threads is provided.
 */
char *
filelist_read(FILELIST *fl, STRBUF *sb)
{
	char *p, *path = fl->current;

	if (fl->ip) {
		if (sb)
			return strbuf_fgets(sb, fl->ip, STRBUF_NOCRLF);
		else
			return strbuf_fgets(fl->sb, fl->ip, STRBUF_NOCRLF);
	}
	if (path == NULL)
		die("filelist_read failed.");
	if (path >= fl->end)
		return NULL;
	for (p = path; *p != '\n'; p++)
		if (p >= fl->end)
			die("unexpected end of file.");
	if (*p == '\0')
		die("filelist includes null character.");
	*p++ = '\0';
	fl->current = p;
	return path;
}
/*
 * filelist_close: close filelist.
 *
 *	i)	fl	filelist structure	
 */
void
filelist_close(FILELIST *fl)
{
	if (fl->sb)
		strbuf_close(fl->sb);
	if (fl->start)
		free(fl->start);
	free(fl);
}
