#include <ctype.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "strv.h"

/*
 * Simple configuration entry
 *
 * Each entry consists of zero or more terms and zero or more blocks. Each
 * block by itself can consist of zero or more config entries.
 */

struct scfgentry {
	char **termv;	/* NULL terminated string vector */
	struct scfgentry **block;	/* NULL terminated list of string vectors */
};

/* iterator options */
struct scfgiteropts {
	int mindepth;	/* filter starting from a minimum depth */
	int maxdepth;	/* filter up to a maximum depth (including) */
	char *key;	/* keyword to filter on first term */
	struct scfgentry *root;	/* alternative root item to start at */
};

char *scfg_termn(struct scfgentry *, int);
char *scfg_getkey(struct scfgentry *);
char *scfg_getval(struct scfgentry *);
char **scfg_getmval(struct scfgentry *);
struct scfgentry *scfg_getbykey(const char *);
size_t scfg_count(void);
void scfg_printr(void);
int scfg_clear(void);
int scfg_foreach(const struct scfgiteropts *, int (*)(struct scfgentry *));
