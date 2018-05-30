#ifndef UTIL_H
#define UTIL_H

#include <sys/stat.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "intv.h"
#include "strv.h"

#define SYNCDIR ".sync"
#define LOCKFILE ".lock"
#define TIMEPAD 30	/* Number of seconds to ignore when determining if it's
			 * time to make a new backup.
			 */

extern int verbose;

const int CMDCLOSED, CMDSTART, CMDSTOP, CMDREADY, CMDROTCLEANUP,
	CMDROTINCLUDE, CMDCUST;

/* temp key value store */
struct tmpkv {
	char *key;
	char *val;
	char **mval;
};

struct snapinterval {
	char *name;	/* name of the interval */
	int count;	/* maximum number of intervals */
	time_t lifetime;	/* length of the interval in seconds */
};

struct snapshot {
	struct endpoint *ep;	/* the endpoint this snapshot belongs to */
	char *name;	/* the name of the interval this snapshot belongs to */
	int number;	/* the number of this snapshot within the interval */
};

struct endpoint {
	char *ruser;	/* remote user */
	char *hostname;	/* remote hostname */
	char *rpath;	/* remote path */
	char *root;	/* local root dir */
	int createroot;	/* whether or not to create the root dir if it does not
			 * exist */
	char *path;	/* local path */
	uid_t uid;	/* local uid */
	gid_t gid;	/* local gid */
	gid_t shared;	/* share backup with this group */
	int pathfd;	/* fd to local path for relative path reference */
	int rotfd;	/* rotator communication pipe */
	int synfd;	/* syncer communication pipe */
	int poxfd;	/* postexec communication pipe */
	pid_t rotpid;	/* rotator process id */
	pid_t synpid;	/* syncer process id */
	pid_t poxpid;	/* postexec process id */
	struct snapinterval **snapshots;
	char *rsyncbin;	/* name of rsync binary */
	char **rsyncargv;	/* extra arguments to rsync */
	int **rsyncexit;	/* extra exit codes to accept */
	char *postexec;	/* postexec */
};

struct snapinterval *snaps_alloc_snapinterval(char *, int, time_t);
void snaps_free_snapinterval(struct snapinterval **);
struct snapinterval **snaps_add_snapinterval(struct snapinterval **,
	struct snapinterval *);
void snaps_endpoint_openrootfd(struct endpoint *);
void snaps_endpoint_chpath(struct endpoint *, const char *);
struct endpoint **snaps_keep_one_endpoint(struct endpoint **,
	struct endpoint *);
struct endpoint *snaps_alloc_endpoint(char *,  char *, char *, char *, int,
	gid_t, uid_t, gid_t, struct snapinterval **);
void snaps_free_endpoint(struct endpoint **);
void snaps_endpoint_setopts(struct endpoint *, char *, char **, int **, char *);
struct endpoint **snaps_add_endpoint(struct endpoint **, struct endpoint *);
struct endpoint **snaps_rm_endpoint(struct endpoint **, struct endpoint *);
int trustedpath(const char *, mode_t, gid_t, int *, int *);
char *normalize_path(const char *, char *, int);
struct endpoint *snaps_find_endpoint(struct endpoint **, const char *);
char *snaps_endpoint_id(struct endpoint *);
int inroot(const char *, const char *, int *);
int normalize_pathcomp(char *);
int secureensuredir(const char *, mode_t, gid_t, int *);
int isabsolutepath(const char *);
char *humanduration(time_t);
char *snapdirstr(const char *, int);
char *getsyncdir(void);
int reapproc(pid_t);
char *getepid(const struct endpoint *);
int isopenfd(int);
char **addstr(char **, const char *);
int writecmd(int, int);
int readcmd(int, int *);
int setsnapshotmode(struct snapshot *, mode_t);
void setsnapshottime(struct snapshot *, time_t);
time_t snapshottime(struct snapshot *);
char *snapshotname(struct snapshot *);
int opensnapshot(const struct snapshot *);
struct snapshot *newestsnapshot(struct endpoint *, struct snapshot *);
int setsnapshot(struct endpoint *, char *, int, struct snapshot *);
time_t snapshotttl(struct snapshot *, time_t, time_t *);
int privdrop(uid_t, gid_t);
void postexec(const struct endpoint *);

#endif
