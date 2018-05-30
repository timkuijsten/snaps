#ifndef SYNCER_H
#define SYNCER_H

#include <fcntl.h>

#include "util.h"

#define RSYNCBIN "/usr/local/sbin/prsync"

extern int verbose;

void syncer(struct endpoint *);

#endif
