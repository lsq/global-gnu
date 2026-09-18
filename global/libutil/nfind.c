/*
 * Copyright (c) 2019
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "abs2rel.h"
#include "conf.h"
#include "char.h"
#include "checkalloc.h"
#include "die.h"
#include "langmap.h"
#include "locatestring.h"
#include "path.h"
#include "strbuf.h"
#include "strhash.h"
#include "strlimcpy.h"
#include "test.h"
#include "gparam.h"

static void find_init(void);
static int find_init_done;
/*
 * Static objects should begin with an underscore (_).
 */
/*--------------------------------------------------------------*/
/* Skip list							*/
/*--------------------------------------------------------------*/
#define SKIP_GLOB	0
#define SKIP_DIR	1
#define SKIP_PATH	2
#define PATH_SEP	'/'

/**
 * _open_skiplist: initialize the skip list
 */
static const char *_skip;
static void
_open_skiplist(const char *list) {
	_skip = list;
}
/**
 * _next_skiplist: get the next skip item.
 *
 *	@param[out]	sb	the next entry of the skip list
 *	@param[in,out]	_skip	skip list
 *	@return		type	type
 *
 *		0: glob pattern for file
 *		1: glob pattern for directory
 *		2: path of file
 *		3: path of directory
 *		-1: end of skip list
 *
 *		the first bit	file(0)/directory(1)
 *		the second bit	unit(0)/path(1)
 *
 */
static int
_next_skiplist(STRBUF *sb) {
	const char *p = _skip;
	const char *start;
	int type = 0;

	/*
	 * Cueing up the next entry.
	 * Do nothing on the first iteration. From the second iteration onwards,
	 * skip one comma. Terminate with 0 at the end.
	 */
	for (; *p && *p == ','; p++)
		;
	if (*p == 0) {
		type = -1;
		goto ext;
	}
	/* Extracting entries */
	start = p;	/* for display */
	strbuf_reset(sb);
	/*
	 * Paths starting with '/' are treated as path specifications.
	 * For processing reasons, the leading character is changed to './'.
	 */
	if (*p == PATH_SEP) {
		strbuf_putc(sb, '.');
		type = SKIP_PATH;
	}
	for (; *p; p++) {
		if (*p == ',')
			break;
		if (*p == PATH_SEP && type != SKIP_PATH) {
			const char *n = p + 1;
			/*
			 * The use of '/' is not permitted except when specifying a path;
			 * however, a trailing '/' is acceptable as it denotes a directory.
			 */
			if (*n != ',' && *n != 0)
				die("glob pattern must not include '/'. (%s)", start);
		}
		/* Copy quoted commas as-is. */
		if (*p == '\\' && *(p + 1) == ',')
			p++;
		strbuf_putc(sb, *p);
	}
	/*
	 * If it ends with '/', it is a directory.
	 * Remove the trailing '/' and report that it is a directory.
	 */
	if (strbuf_unputc(sb, '/'))
		type |= SKIP_DIR;
ext:
	/* Update the starting point of the internal skip list. */
	_skip = p;
	return type;
}
/*
 * Convert a glob pattern to a regular expression.
 *
 *	@param[in]	glob	glob pattern
 *	@param[out]	regex	Regular expression
 *	@return			Regular expression
 */
static char *
_glob2regex(const char *glob, STRBUF *regex) {
	const char *p;

	strbuf_reset(regex);
	for (p = glob; *p; p++) {
		/*
		 * replaces wild cards into regular expressions.
		 *
		 * '*' -> '.*'
		 * '?' -> '.'
		 * '[...]' -> '[...]'
		 * '[!...]' -> '[^...]'
		 */
		if (*p == '*')
			strbuf_puts(regex, ".*");
		else if (*p == '?')
			strbuf_putc(regex, '.');
		else if (*p == '[') {
			strbuf_putc(regex, *p++);		/* '[' */
			if (*p == '\0')
				die("unterminated class ([...).");
			if (*p == '!') {
				strbuf_putc(regex, '^');
				p++;
			}
			while (*p && *p != ']') {
				strbuf_putc(regex, *p++);
			}
			if (*p != ']')
				die("unterminated class ([...).");
			strbuf_putc(regex, *p);
		} else if (isregexchar(*p)) {
			strbuf_putc(regex, '\\');
			strbuf_putc(regex, *p);
		} else if (*p == '\\' && *(p + 1) != '\0') {
			strbuf_putc(regex, *p++);
			strbuf_putc(regex, *p);
		} else
			strbuf_putc(regex, *p);
	}
	return strbuf_value(regex);
}
/*
 * Create a rule base from a skip list.
 *
 *	@param[in]	skiplist	skip list
 *	@param[out]	unit_dir	regular expression for skip directories
 *	@param[out]	unit_file	regular expression for skip files
 *	@param[out]	path_dir	directory path
 *	@param[out]	path_file	file path
 */
#define REG_START	"^("
#define REG_END		")$"
static void
_build_skip_rules(char *skiplist, STRBUF *unit_dir, STRBUF *unit_file,
	STRHASH *path_dir, STRHASH *path_file)
{
	STRBUF *sb = strbuf_open(0);
	STRBUF *sb2 = strbuf_open(0);
	int type;

	strbuf_puts(unit_dir, REG_START);
	strbuf_puts(unit_file, REG_START);
	/*
	 * Convert a skip list to a regular expression.
	 */
	_open_skiplist(skiplist);
	while ((type = _next_skiplist(sb)) != -1) {
		switch (type) {
		case 0: /* file glob */
			strbuf_puts(unit_file, _glob2regex(strbuf_value(sb), sb2));
			strbuf_putc(unit_file, '|');
			break;
		case 1: /* directory glob */
			strbuf_puts(unit_dir, _glob2regex(strbuf_value(sb), sb2));
			strbuf_putc(unit_dir, '|');
			break;
		case 2:
			/* file path */
			(void)strhash_assign(path_file, strbuf_value(sb), 1); 
			break;
		case 3:
			/* directory path */
			(void)strhash_assign(path_dir, strbuf_value(sb), 1); 
			break;
		}
		/* fprintf(stdout, "%d: %s\n", type, strbuf_value(sb)); */
	}
	/* Remove the last character ('|') from the list. */
	strbuf_unputc(unit_dir, '|');
	strbuf_unputc(unit_file, '|');
	/* Reset if empty. */
	if (!strcmp(strbuf_value(unit_dir), REG_START))
		strbuf_reset(unit_dir);
	else
		strbuf_puts(unit_dir,  REG_END);
	if (!strcmp(strbuf_value(unit_file), REG_START))
		strbuf_reset(unit_file);
	else
		strbuf_puts(unit_file,  REG_END);

	strbuf_close(sb);
	strbuf_close(sb2);
}
static const char *const _tagslist[] = {
	"GPATH", "GTAGS", "GRTAGS", "GSYMS", NULL
};
static int
_is_tagfile(const char *name)
{
	int i = 0;

	for (i = 0; _tagslist[i] != NULL && strcmp(_tagslist[i], name) != 0; i++)
		;
	return _tagslist[i] ? 1 : 0;
}
/*
 * Reason for skipping
 */
#define	BECAUSE_SKIPLIST	1
#define	BECAUSE_SKIPPATH	2
#define BECAUSE_DOT		3
#define BECAUSE_TAGFILE		4
#define BECAUSE_LOOP		5
#define BECAUSE_SYMLINK		6
static void
_print_explain(int because, char type, const char *path)
{
	const char *type_s = "f";
	if (type == 'd')
		type_s = "Directory";
	else if (type == 'f')
		type_s = "File";
	else if (type == 's')
		type_s = "Symbolic Link";
	path = trimpath(path);

	switch (because) {
	case BECAUSE_SKIPLIST:
		fprintf(stderr, " - %s '%s' is skipped by the skip list (unit).\n",
			type_s, path);
		break;
	case BECAUSE_SKIPPATH:
		fprintf(stderr, " - %s '%s' is skipped by the skip list (path).\n",
			type_s, path);
		break;
	case BECAUSE_DOT:
		fprintf(stderr, " - %s '%s' is skipped because the name begins with a dot.\n",
			type_s, path);
		break;
	case BECAUSE_TAGFILE:
		fprintf(stderr, " - %s '%s' is skipped because it is a tag file.\n",
			type_s, path);
		break;
	case BECAUSE_LOOP:
		fprintf(stderr, " - %s '%s' is skipped because symbolic link loop detected.\n",
			type_s, path);
		break;
	case BECAUSE_SYMLINK:
		fprintf(stderr, " - %s '%s' is skipped because --skip-symlink option.\n",
			type_s, path);
		break;
	}
}
/*--------------------------------------------------------------*/
/* langmap							*/
/*--------------------------------------------------------------*/
#include <ctype.h>
#include <regex.h>

static char *_langmap;
static regex_t issource;
/**
 * langmap2regex: initialize the language map
 */
static void
_langmap2regex(char *map, STRBUF *sb) {
	char *p = map;

	strbuf_puts(sb, "^(");
	while (*p) {
		/* skip language name */
		for (; *p && *p != ':'; p++)
			;
		if (*p == 0)
			die_with_code(2, "syntax error in langmap '%s'.", map);
		p++;
		/*
		 * Take a file extension or glob pattern and convert it into
		 * a regular expression.
		 * File extensions begin with '.'. glob patterns begin with '('.
		 */
		while (*p == '.' || *p == '(') {
			if (*p == '.') {	/* suffix */
				strbuf_puts(sb, ".+\\.");
				for (p++; *p && *p != '.' && *p != '(' && *p != ','; p++) {
					if (!isalnum(*p))
						strbuf_putc(sb, '\\');
					strbuf_putc(sb, *p);
				}
			} else if (*p == '(') {	/* glob pattern */
				for (p++; *p && *p != ')'; p++) {
					if (*p == '.')
						strbuf_puts(sb, "\\.");
					else if (*p == '*')
						strbuf_puts(sb, ".*");
					else if (*p == '?')
						strbuf_puts(sb, ".");
					else if (*p == '[') {
						strbuf_putc(sb, '[');
						if (*++p == '!') {
							strbuf_putc(sb, '^');
							p++;
						}
						for (; *p && *p != ']'; p++)
							strbuf_putc(sb, *p);
						if (*p == 0)
							die_with_code(2, "syntax error in langmap '%s'.", _langmap);
						strbuf_putc(sb, ']');
					} else
						strbuf_putc(sb, *p);
				}
				if (*p == 0)
					die_with_code(2, "syntax error in langmap '%s'.", _langmap);
				p++;
			}
			strbuf_putc(sb, '|');
		}
		if (*p == ',')
			p++;
	}
	strbuf_unputc(sb, '|');
	strbuf_puts(sb, ")$");
}
int
issourcefile(char *file)
{
	int result = regexec(&issource, file, 0, 0, 0) == 0 ? 1 : 0;
	/* fprintf(stdout, "issourcefile('%s') => %d\n", file, result); */
	return result;
}
/*--------------------------------------------------------------*/
/* find body							*/
/*--------------------------------------------------------------*/
static FILE *ip;
static FILE *temp;
static char *rootdir;
static char cwddir[MAXPATHLEN];
/*
 * open mode
 */
#define FTS_OPEN	1
#define FILELIST_OPEN	2
static int find_mode;
static int find_eof;
/*
 * options
 */
extern int qflag;
extern int debug;
static int verbose;
static const int check_looplink = 1;
static int accept_dotfiles = 0;
static int find_explain = 0;
static int skip_symlink = 0;

/**             
 * set_accept_dotfiles: make find to accept dot files and dot directries.
 */             
void
set_accept_dotfiles(void)
{
	accept_dotfiles = 1;
}
/**
 * set_skip_symlink: set rules for symbolic links.
 *
 *	mode	0: accept symbolic link
 *		SKIP_SYMLINK_FOR_DIR:  skip symbolic links for a dir
 *		SKIP_SYMLINK_FOR_FILE: skip symbolic links for a file
 *		SKIP_SYMLINK_FOR_ALL:  skip symbolic links
 */
void
set_skip_symlink(int mode)
{
	skip_symlink = mode;
}
static STRHASH *path_dir, *path_file;
static regex_t skip_dir, *skip_dirp;
static regex_t skip_file, *skip_filep;
int
skipthisfile(const char *path)
{
	if (find_init_done == 0)
		find_init();
	int skip = 0;
	if (strncmp(path, "./.", 3) == 0 && !accept_dotfiles)
	{
		skip = 1;
		if (find_explain)
			_print_explain(BECAUSE_DOT, 'f', trimpath(path));
	} else if (_is_tagfile(trimpath(path))) {
		skip = 1;
		if (find_explain)
			_print_explain(BECAUSE_TAGFILE, 'f', trimpath(path));
	} else if (skip_filep && regexec(skip_filep, path, 0, 0, 0) == 0) {
		skip = 1;
		if (find_explain)
			_print_explain(BECAUSE_SKIPLIST, 'f', trimpath(path));
	} else if (path_file && strhash_assign(path_file, path, 0)) {
		skip = 1;
		if (find_explain)
			_print_explain(BECAUSE_SKIPPATH, 'f', trimpath(path));
	}
	return skip;
}
/*
 * find_init: Initialize find
 */
static void
find_init()
{
	if (find_init_done)
		return;
	STRBUF *sb = strbuf_open(0);
	STRBUF *sb2 = strbuf_open(0);
	STRBUF *unit_dir, *unit_file;
	int flags = REG_EXTENDED|REG_NEWLINE;

	/*
	 * (1) skip procedure
	 */
	unit_dir = strbuf_open(0);
	unit_file = strbuf_open(0);
	path_dir = strhash_open(10);
	path_file = strhash_open(10);

	if (getconfb("icase_path"))
		flags |= REG_ICASE;
	if (!getconfs("skip", sb))
		die("cannot get skip variable.");
	/*
	 * Create a regular expression for skip processing.
	 */
	_build_skip_rules(strbuf_value(sb), unit_dir, unit_file, path_dir, path_file);
	if (debug) {
		fprintf(stderr, "path_dir:\n");
		strhash_dump(path_dir);
		fprintf(stderr, "path_file:\n");
		strhash_dump(path_file);
		fprintf(stderr, "unit_dir: %s\n", strbuf_value(unit_dir));
		fprintf(stderr, "unit_file: %s\n", strbuf_value(unit_file));
	}
	/*
	 * Compile the regular expressions.
	 */
	skip_dirp = skip_filep = NULL;
	if (strbuf_getlen(unit_dir) > 0) {
		if (regcomp(&skip_dir, strbuf_value(unit_dir), flags) != 0)
			die("cannot compile regular expression.");
		skip_dirp = &skip_dir;
	}
	if (strbuf_getlen(unit_file) > 0) {
		if (regcomp(&skip_file, strbuf_value(unit_file), flags) != 0)
			die("cannot compile regular expression.");
		skip_filep = &skip_file;
	}
	strbuf_close(unit_dir);
	strbuf_close(unit_file);
	/*
	 * (2) langmap
	 */
	strbuf_reset(sb);
	if (!getconfs("langmap", sb))
		die("cannot get langmap variable.");
	/*
	 * Create a regular expression to identify source files based on langmap.
	 */
	_langmap2regex(strbuf_value(sb), sb2);
	if (regcomp(&issource, strbuf_value(sb2), flags) != 0)
		die("cannot compile regular expression.");
	strbuf_close(sb);
	strbuf_close(sb2);
	find_init_done = 1;
}
static void
find_terminate()
{
	strhash_close(path_dir);
	strhash_close(path_file);
	path_dir = path_file = NULL;
}
/**
 * find_open_filelist: find_open like interface for handling output of find(1).
 *
 *	@param[in]	filename	file including list of file names.
 *				When "-" is specified, read from standard input.
 *	@param[in]	root		root directory of source tree
 *	@param[in]	explain	print verbose message
 */
void
find_open_filelist(const char *filename, const char *root, int explain)
{
	size_t rootdir_len;
	assert(find_mode == 0);
	find_mode = FILELIST_OPEN;
	find_explain = explain;

	find_init();
	if (!strcmp(filename, "-")) {
		/*
		 * If the filename is '-', copy standard input onto
		 * temporary file to be able to read repeatedly.
		 */
		if (temp == NULL) {
			char buf[MAXPATHLEN];

			temp = tmpfile();
			while (fgets(buf, sizeof(buf), stdin) != NULL)
				fputs(buf, temp);
		}
		rewind(temp);
		ip = temp;
	} else {
		ip = fopen(filename, "r");
		if (ip == NULL)
			die("cannot open '%s'.", trimpath(filename));
	}
	/*
	 * rootdir always ends with '/'.
	 *
	 * rootdir_len is two characters longer than root to be able
	 * to append a "/" if needed and the string terminator
	 * ofcourse.
	 */
	rootdir_len = strlen(root) + 2;
	rootdir = malloc(rootdir_len);
	if (!rootdir)
		die("short of memory.");

	snprintf(rootdir, rootdir_len, "%s%s", root,
		 strcmp(root, "/") ? "/" : "");

	strlimcpy(cwddir, root, sizeof(cwddir));
}
/**
 * find_read_filelist: read path from file
 *
 *	@return		path
 */
static char *
find_read_filelist(void)
{
	STATIC_STRBUF(ib);
	static char buf[MAXPATHLEN + 1];
	static char *path;
	static char *file;

	strbuf_clear(ib);
	for (;;) {
		path = strbuf_fgets(ib, ip, STRBUF_NOCRLF);
		if (path == NULL) {
			/* EOF */
			find_eof = 1;
			return NULL;
		}
		if (*path == '\0') {
			/* skip empty line.  */
			continue;
		}
		/*
		 * Lines which start with ". " are considered to be comments.
		 */
		if (*path == '.' && *(path + 1) == ' ')
			continue;
		for (file = path + strbuf_getlen(ib) - 1; *file != '/'; file--)
			; 
		file++;
		int skip = 0;
		/*
		 * Skip the following:
		 * o directory
		 * o file which does not exist
		 * o dead symbolic link
		 */
		if (!test("f", path)) {
			if (test("d", path))
				warning("'%s' is a directory. ignored.", trimpath(path));
			else
				warning("'%s' not found. ignored.", trimpath(path));
			continue;
		}
		/*
		 * normalize path name.
		 *
		 *	rootdir  /a/b/
		 *	buf      /a/b/c/d.c -> c/d.c -> ./c/d.c
		 */
		if (normalize(path, rootdir, cwddir, buf, sizeof(buf)) == NULL) {
			warning("'%s' is out of source tree. ignored.", trimpath(path));
			continue;
		}
		path = buf;
		if (*file == '.' && !accept_dotfiles) {
			skip = 1;
			if (find_explain)
				_print_explain(BECAUSE_DOT, 'f', path);
		} else if (*file == 'G' && _is_tagfile(file)) {
			skip = 1;
			if (find_explain)
				_print_explain(BECAUSE_TAGFILE, 'f', path);
		} else if (skip_filep && regexec(skip_filep, file, 0, 0, 0) == 0) {
			skip = 1;
			if (find_explain)
				_print_explain(BECAUSE_SKIPLIST, 'f', path);
		} else if (strhash_assign(path_file, path, 0)) {
			skip = 1;
			if (find_explain)
				_print_explain(BECAUSE_SKIPPATH, 'f', path);
		}
		if (skip)
			continue;
		/*
		 * A blank at the head of path means
		 * other than source file.
		 */
		if (!issourcefile(path))
			*--path = ' ';
		return path;
	}
}

#include <sys/types.h>
#include <sys/stat.h>
#include <fts.h>

static FTS *ftsp;
/**
 * find_open: start iterator without GPATH.
 *
 *	@param[in]	start	start directory,
 *			If NULL, assumed "." (current) directory.
 *	@param[in]	explain	print verbose message
 */
void
find_open(const char *start, int explain)
{
	char *paths[2];
	int ftsoptions= FTS_NOCHDIR | (skip_symlink ? FTS_PHYSICAL : FTS_LOGICAL);

	assert(find_mode == 0);
	find_mode = FTS_OPEN;
	find_explain = explain;

	find_init();
	if (!start)
		start = ".";
        if ((rootdir = realpath(start, NULL)) == NULL)
                die("cannot get real path of '%s'.", trimpath(start));
	paths[0] = (char *)start;
	paths[1] = NULL;
	ftsp = fts_open(paths, ftsoptions, NULL);
	if (ftsp == NULL)
		die("ftsopen failed.");
}
/**
 * find_read_fts: read path without GPATH.
 *
 *	@return		path
 */
static char *
find_read_fts(void)
{
	static char val[MAXPATHLEN];
	char *result;
	char path[MAXPATHLEN];
	int errno, skip;
	FTSENT *entry;

	while (errno = 0, (entry = fts_read(ftsp)) != NULL) {
                if (entry->fts_level == 0)
                        continue;
                switch (entry->fts_info) {
		/* Items not defined in FTS_XXX (socket) */
		case FTS_DEFAULT:
			break;
		case FTS_SL:
			if (find_explain)
				_print_explain(BECAUSE_SYMLINK, 's', trimpath(entry->fts_path));
			break;
                case FTS_D:
			skip = 0;
			if (entry->fts_name[0] == '.' && !accept_dotfiles)
			{
				skip = 1;
				if (find_explain)
					_print_explain(BECAUSE_DOT, 'd', trimpath(entry->fts_path));
			} else if (skip_dirp && regexec(skip_dirp, entry->fts_name, 0, 0, 0) == 0)
			{
				skip = 1;
				if (find_explain)
					_print_explain(BECAUSE_SKIPLIST, 'd', trimpath(entry->fts_path));
			} else if (strhash_assign(path_dir, entry->fts_path, 0)) {
				skip = 1;
				if (find_explain)
					_print_explain(BECAUSE_SKIPPATH, 'd', trimpath(entry->fts_path));
			}
			if (skip)
				fts_set(ftsp, entry, FTS_SKIP);
			break;
                case FTS_DP:
			/* post order directory */
			break;
		case FTS_DC:
			/* Directory that generates a cycle */
			warning("symbolic link loop detected. '%s' is ignored.", trimpath(trimpath(entry->fts_path)));
			if (find_explain)
				_print_explain(BECAUSE_LOOP, 'd', trimpath(entry->fts_path));
			fts_set(ftsp, entry, FTS_SKIP);
			break;
		case FTS_SLNONE:
			/* Broken symbolic link */
			warning("symbolic link without target. '%s' is ignored.", trimpath(entry->fts_path));
			break;
		case FTS_NS:
			/* Failed to stat() */
			if (!_is_tagfile(entry->fts_name))
				warning("cannot stat '%s'. ignored.\n", trimpath(entry->fts_path));
                        break;
                case FTS_F:
			skip = 0;
			if (entry->fts_name[0] == '.' && !accept_dotfiles)
			{
				skip = 1;
				if (find_explain)
					_print_explain(BECAUSE_DOT, 'f', trimpath(entry->fts_path));
			} else if (entry->fts_name[0] == 'G' && _is_tagfile(entry->fts_name)) {
				skip = 1;
				if (find_explain)
					_print_explain(BECAUSE_TAGFILE, 'f', trimpath(entry->fts_path));
			} else if (skip_filep && regexec(skip_filep, entry->fts_name, 0, 0, 0) == 0) {
				skip = 1;
				if (find_explain)
					_print_explain(BECAUSE_SKIPLIST, 'f', trimpath(entry->fts_path));
			} else if (strhash_assign(path_file, entry->fts_path, 0)) {
				skip = 1;
				if (find_explain)
					_print_explain(BECAUSE_SKIPPATH, 'f', trimpath(entry->fts_path));
			} else if (access(entry->fts_path, R_OK) < 0) {
				skip = 1;
				warning("cannot read '%s'. ignored.", trimpath(entry->fts_path));
			}
			if (skip)
				continue;
			if (issourcefile(entry->fts_name)) {
				result = entry->fts_path;
			} else {
				val[0] = ' ';
				strlimcpy(&val[1], entry->fts_path, sizeof(val));
				result = val;
			}
			return result;
                        break;
                default:
			warning("Unknown type: %d (%s)", entry->fts_info, trimpath(entry->fts_path));
                        break;
                }
	}
	return NULL;
}
/**
 * find_read: read path without GPATH.
 *
 *	@return		path
 */
char *
find_read(void)
{
	static char *path;

	assert(find_mode != 0);
	if (find_eof)
		path = NULL;
	else if (find_mode == FILELIST_OPEN)
		path = find_read_filelist();
	else if (find_mode == FTS_OPEN)
		path = find_read_fts();
	else
		die("find_read: internal error.");
	return path;
}
/**
 * find_close: close iterator.
 */
void
find_close(void)
{
	assert(find_mode != 0);
	if (find_mode == FTS_OPEN) {
		fts_close(ftsp);
	} else if (find_mode == FILELIST_OPEN) {
		/*
		 * The --file=- option is specified, we don't close file
		 * to read it repeatedly.
		 */
		if (ip != temp)
			fclose(ip);
	} else {
		die("find_close2: internal error.");
	}
	find_terminate();
	if (rootdir)
		free(rootdir);
	find_eof = find_mode = 0;
}
