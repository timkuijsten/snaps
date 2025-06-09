#include <ctype.h>
#include <err.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "util.h"
#include "scfg.h"

struct endpoint **epv = NULL;

extern int forceopt;

/* default settings */
struct tmpkv defset[] = {
	{ "root", NULL, NULL },
	{ "createroot", "yes", NULL },
	{ "user", NULL, NULL },
	{ "ruser", "root", NULL },
	{ "hourly", "0", NULL },
	{ "daily", "0", NULL },
	{ "weekly", "0", NULL },
	{ "monthly", "0", NULL },
};

/* global settings */
struct tmpkv gset[] = {
	{ "root", NULL, NULL },
	{ "createroot", NULL, NULL },
	{ "user", NULL, NULL },
	{ "group", NULL, NULL },
	{ "rsyncbin", NULL, NULL },
	{ "rsyncargs", NULL, NULL },
	{ "rsyncexit", NULL, NULL },
	{ "hourly", NULL, NULL },
	{ "daily", NULL, NULL },
	{ "weekly", NULL, NULL },
	{ "monthly", NULL, NULL },
	{ "ruser", NULL, NULL },
	{ "hostname", NULL, NULL },
	{ "rpath", NULL, NULL },
	{ "exec", NULL, NULL },
};

/* per-endpoint setting */
struct tmpkv tmpepset[] = {
	{ "root", NULL, NULL },
	{ "createroot", NULL, NULL },
	{ "user", NULL, NULL },
	{ "group", NULL, NULL },
	{ "rsyncbin", NULL, NULL },
	{ "rsyncargs", NULL, NULL },
	{ "rsyncexit", NULL, NULL },
	{ "hourly", NULL, NULL },
	{ "daily", NULL, NULL },
	{ "weekly", NULL, NULL },
	{ "monthly", NULL, NULL },
	{ "ruser", NULL, NULL },
	{ "hostname", NULL, NULL },
	{ "rpath", NULL, NULL },
	{ "exec", NULL, NULL },
	{ "backup", NULL, NULL },
};

static const size_t tmpepsetsize = sizeof(tmpepset) / sizeof(tmpepset[0]);
static const size_t gsetsize = sizeof(gset) / sizeof(gset[0]);

void gsetint(char **, int *);
char *getsetting(char *);
char **getmsetting(char *);
int **getmnsetting(char *);
int getbsetting(char *, int *);
int getnsetting(char *, int *);
int haskey(struct tmpkv *, size_t, const char *);
char *getkey(struct tmpkv *, size_t, const char *);
int parsehoststr(const char *, char **, char **, char **);
char *getbackupid(void);
struct snapinterval **parseintervals(void);
int saveset(struct tmpkv *, size_t, const char *, char *, char **);
void clrtmpkv(struct tmpkv *, size_t);

extern int verbose;
extern time_t starttime;

/* set global setting */
int
setgset(struct scfgentry *ce)
{
	const char *key;

	key = scfg_getkey(ce);

	if (key == NULL)
		return 1;

	/* skip backup keys */
	if (strcmp(key, "backup") == 0)
		return 1;

	if (haskey(gset, gsetsize, key) == 0) {
		warnx("\"%s\" is not a global key", key);
		return 0;
	}

	if (getkey(gset, gsetsize, key) != NULL) {
		warnx("\"%s\" should be set only once", key);
		return 0;
	}

	if (!saveset(gset, gsetsize, key, scfg_getval(ce), scfg_getmval(ce))) {
		warnx("\"%s\" is an unknown global keyword", key);
		return 0;
	}

	return 1;
}

/* set endpoint specific setting */
int
setepset(struct scfgentry *ce)
{
	const char *key;

	key = scfg_getkey(ce);

	/* skip backup keys */
	if (strcmp(key, "backup") == 0)
		return 1;

	if (ce->block != NULL) {
		warnx("endpoint specific settings can not contain nested "
			"blocks: \"%s\"", key);
		return 0;
	}

	if (!saveset(tmpepset, tmpepsetsize, key, scfg_getval(ce),
	    scfg_getmval(ce))) {
		warnx("unknown endpoint keyword: \"%s\"", key);
		return 0;
	}

	return 1;
}

/*
 * Create an endpoint to backup based on gset and tmpepset. Resolve user and
 * group ids and set local paths.
 *
 * Add each succesfully processed endpoint to epv.
 */
int
createendpoint(struct scfgentry *ce)
{
	struct passwd *pwd;
	struct group *grp;
	struct endpoint **epp, *ep;
	struct snapinterval **siv;
	struct scfgiteropts iteropts;
	int e, issubdir, createroot;
	uid_t uid;
	gid_t gid, shared;
	char **root, *hoststr, *ruser, *hostname, *rpath, *tmp, *backupid;
	const char *key;
	int **rsyncexit;

	key = scfg_getkey(ce);
	if (key == NULL || strcmp(key, "backup") != 0) {
		warnx("only backup keys are supported: \"%s\"", key);
		return -1;
	}

	/* Each backup key should have a host definition. */

	/*
	 * Set endpoint specific settings.
	 */

	if (ce->block != NULL) {
		memset(&iteropts, '\0', sizeof(iteropts));

		iteropts.root = ce;

		if (scfg_foreach(&iteropts, setepset) != 1) {
			warnx("not all endpoint settings could be set");
			return -1;
		}
	}

	/* Check if all required settings are there. Do no allocations yet. */

	e = 0;

	hoststr = scfg_getval(ce);
	if (hoststr == NULL) {
		warnx("backup key without host definition");
		e = 1;
	}

	/* parse backup host definition */
	if (parsehoststr(hoststr, &ruser, &hostname, &rpath) == -1) {
		warnx("invalid backup value: \"%s\"", hoststr);
		e = 1;
	}

	/* Make sure the block did not already have any of those settings. */

	if (ruser) {
		if (getkey(tmpepset, tmpepsetsize, "ruser") == NULL) {
			if (!saveset(tmpepset, tmpepsetsize, "ruser", ruser,
			    NULL)) {
				warnx("unknown keyword: \"%s\"", "ruser");
				e = 1;
			}
		} else {
			warnx("ruser already set to \"%s\" for: \"%s\"",
			    getkey(tmpepset, tmpepsetsize, "ruser"), hoststr);
			e = 1;
		}
	}

	if (hostname) {
		if (getkey(tmpepset, tmpepsetsize, "hostname") == NULL) {
			if (!saveset(tmpepset, tmpepsetsize, "hostname", hostname,
			    NULL)) {
				warnx("unknown keyword: \"%s\"", "hostname");
				e = 1;
			}
		} else {
			warnx("hostname already set to \"%s\" for: \"%s\"",
			    getkey(tmpepset, tmpepsetsize, "hostname"), hoststr);
			e = 1;
		}
	}

	if (rpath) {
		if (getkey(tmpepset, tmpepsetsize, "rpath") == NULL) {
			if (!saveset(tmpepset, tmpepsetsize, "rpath", rpath,
			    NULL)) {
				warnx("unknown keyword: \"%s\"", "rpath");
				e = 1;
			}
		} else {
			warnx("rpath already set to \"%s\" for: \"%s\"",
			    getkey(tmpepset, tmpepsetsize, "rpath"), hoststr);
			e = 1;
		}
	}

	backupid = getbackupid();

	if (getsetting("hourly") == NULL && getsetting("daily") == NULL &&
		getsetting("weekly") == NULL && getsetting("monthly") == NULL) {
		warnx("specify at least one interval for: \"%s\"", backupid);
		e = 1;
	}

	root = getmsetting("root");

	ruser = getsetting("ruser");
	hostname = getsetting("hostname");
	rpath = getsetting("rpath");

	if (ruser == NULL || strlen(ruser) == 0) {
		warnx("missing required parameter ruser for \"%s\"", backupid);
		e = 1;
	}

	if (hostname == NULL || strlen(hostname) == 0) {
		warnx("missing required parameter hostname for \"%s\"", backupid);
		e = 1;
	}

	if (rpath == NULL || strlen(rpath) == 0) {
		warnx("missing required parameter rpath for \"%s\"", backupid);
		e = 1;
	}

	siv = parseintervals();
	if (siv == NULL) {
		warnx("could not determine the number of copies to retain and "
			"on which interval for \"%s\"", backupid);
		e = 1;
	}

	/*
	 * Make sure the root path is absolute.
	 */

	if (root == NULL || root[0] == NULL) {
		warnx("root must be set for \"%s\"", backupid);
		e = 1;
	} else if (!isabsolutepath(root[0])) {
		warnx("root must be set to an absolute path: %s",
			root[0]);
		e = 1;
	}

	if (getbsetting("createroot", &createroot) == -1) {
		warnx("createroot is not set to either \"yes\" or \"no\"");
		e = 1;
	}

	/*
	 * Resolve shared group id (precedence of names over ids is
	 * based on chown(1) and POSIX).
	 */

	shared = UNSHARED;
	if (root && root[1] != NULL) {
		grp = getgrnam(root[1]);
		if (grp != NULL) {
			shared = grp->gr_gid;
		} else {
			/* Maybe it's a gid. */
			if (getnsetting("shared", &shared) == -1) {
				warnx("could not determine shared group id of"
					" \"%s\"", root[1]);
				e = 1;
			}
		}
	}

	/*
	 * Resolve user and group ids (precedence of names over ids is
	 * based on chown(1) and POSIX).
	 */

	uid = 0;
	tmp = getsetting("user");

	if (tmp == NULL) {
		warnx("configure an unprivileged user as which rsync must be "
			"run");
		e = 1;
	} else {
		pwd = getpwnam(tmp);

		if (pwd == NULL) {

			/* Maybe it's a uid. */

			if (getnsetting("user", &uid) == -1) {
				warnx("could not determine user id of user "
					"\"%s\"", tmp);
				e = 1;
			} else {
				pwd = getpwuid(uid);
				if (pwd == NULL) {
					/*
					 * uid is not set in passwd, but thats
					 * ok. Use the same id for the group.
					 */
					gid = uid;
				} else {
					/* Use the configured primary group. */
					uid = pwd->pw_uid;
					gid = pwd->pw_gid;
				}
			}
		} else {
			uid = pwd->pw_uid;
			gid = pwd->pw_gid;
		}

		if (e == 0 && uid == 0) {
			warnx("it is unsafe and not supported to run rsync as "
				"the superuser");
			e = 1;
		}
	}

	/*
	 * If a group is specified in the config file, do the same for group.
	 * Otherwise keep the group id of any previously resolved user.
	 */

	tmp = getsetting("group");
	if (tmp != NULL) {
		grp = getgrnam(tmp);
		if (grp == NULL) {

			/* Maybe it's a gid. */

			if (getnsetting("group", &gid) == -1) {
				warnx("could not determine group id of "
					"\"%s\"", tmp);
				e = 1;
			}
		} else {
			gid = grp->gr_gid;
		}
	}

	rsyncexit = NULL;
	if (getmsetting("rsyncexit") != NULL) {
		if ((rsyncexit = getmnsetting("rsyncexit")) == NULL) {
			warnx("rsyncexit contains invalid exit codes");
			e = 1;
		}
	}

	if (e)
		goto out;

	/*
	 * Proceed to setup a new endpoint.
	 */

	/*
	 * Aid the admin in preventing a configuration mistake and make sure
	 * this root is not a subdir of a previously added root.
	 */
	for (epp = epv; epp && *epp; epp++) {
		if (inroot((*epp)->root, root[0], &issubdir) && issubdir) {
			warnx("%s is a subdir of %s, skipping", root[0], (*epp)->root);
			goto out;
		}

		if (inroot(root[0], (*epp)->root, &issubdir) && issubdir) {
			warnx("%s is a subdir of %s, skipping", (*epp)->root, root[0]);
			goto out;
		}
	}

	/* Create the new endpoint. */
	ep = snaps_alloc_endpoint(ruser, hostname, rpath, root[0], createroot,
		shared, uid, gid, siv);

	/*
	 * Make sure an endpoint with the same id does not already exist.
	 */
	if (snaps_find_endpoint(epv, snaps_endpoint_id(ep))) {
		warnx("another endpoint with the same id already exists: "
			"\"%s\"", snaps_endpoint_id(ep));
		snaps_free_endpoint(&ep);
		goto out;
	}

	snaps_endpoint_setopts(ep, getsetting("rsyncbin"),
		getmsetting("rsyncargs"), rsyncexit, getsetting("exec"));
	clrintv(&rsyncexit);

	/* Finally, add the new endpoint. */
	epv = snaps_add_endpoint(epv, ep);

out:
	clrtmpkv(tmpepset, tmpepsetsize);
	return 1;
}

/*
 * Save a certain setting by referencing it.
 *
 * Return 1 if saved, or 0 if keyword not found.
 */
int
saveset(struct tmpkv *kv, size_t size, const char *key, char *val, char **mval)
{
	int i;

	for (i = 0; i < size; i++) {
		if (strcmp(kv->key, key) == 0) {
			kv->val = val;
			kv->mval = mval;
			return 1;
		}
		kv++;
	}

	return 0;
}

/*
 * Find a value by key in the key-value set.
 *
 * Returns the value if kv contains the key, NULL if not. Beware that the value
 * can be NULL.
 */
char *
getkey(struct tmpkv *kv, size_t size, const char *key)
{
	int i;

	for (i = 0; i < size; i++)
		if (strcmp(key, kv[i].key) == 0)
			return kv[i].val;

	return NULL;
}

/*
 * Find if a key is in the key-value set.
 *
 * Returns 1 if kv contains the key, 0 if not.
 */
int
haskey(struct tmpkv *kv, size_t size, const char *key)
{
	int i;

	if (key == NULL)
		return 0;

	for (i = 0; i < size; i++)
		if (strcmp(key, kv[i].key) == 0)
			return 1;

	return 0;
}

/*
 * Get a tmpkv structure by key. Endpoint settings overrule global settings and
 * global settings overrule default settings.
 *
 * Returns a pointer to the setting if found or NULL if not found. Do not free
 * the result because it might point to a statically defined default value.
 */
struct tmpkv *
gettmpkv(char *key)
{
	int i;

	if (verbose > 2)
		warnx("looking up \"%s\"", key);

	for (i = 0; i < tmpepsetsize; i++)
		if (strcmp(key, tmpepset[i].key) == 0 && tmpepset[i].val != NULL)
			return &tmpepset[i];
	if (verbose > 2)
		warnx("%s not in tmpepset", key);

	for (i = 0; i < gsetsize; i++)
		if (strcmp(key, gset[i].key) == 0 && gset[i].val != NULL)
			return &gset[i];
	if (verbose > 2)
		warnx("%s not in gset", key);

	for (i = 0; i < sizeof(defset) / sizeof(defset[0]); i++)
		if (strcmp(key, defset[i].key) == 0 && defset[i].val != NULL)
			return &defset[i];
	if (verbose > 2)
		warnx("%s not in defset", key);

	return NULL;
}

/*
 * Get a certain setting. Endpoint settings overrule global settings and global
 * settings overrule default settings.
 *
 * Returns a pointer to the setting if found or NULL if not found. Do not free
 * the result because it might point to a statically defined default value.
 */
char *
getsetting(char *key)
{
	struct tmpkv *setting;

	setting = gettmpkv(key);

	if (setting != NULL)
		return setting->val;

	return NULL;
}

/*
 * Get a certain multiple-value setting. Endpoint settings overrule global
 * settings and global settings overrule default settings.
 *
 * Returns a pointer to the setting if found or NULL if not found. Do not free
 * the result because it might point to a statically defined default value.
 */
char **
getmsetting(char *key)
{
	struct tmpkv *setting;

	setting = gettmpkv(key);

	if (setting != NULL)
		return setting->mval;

	return NULL;
}

/*
 * Get multiple numeric values of a setting.
 *
 * The caller should call clrintv on the result when done.
 *
 * Return a null terminated vector of pointers to integers on success or NULL on
 * error with errno set.
 */
int **
getmnsetting(char *key)
{
	const char *errstr;
	char **cpp;
	int num, **numbers;
	int i;

	if ((cpp = getmsetting(key)) == NULL)
		return NULL;

	numbers = NULL;
	for (i = 0; cpp[i]; i++) {
		num = strtonum(cpp[i], INT_MIN, INT_MAX, &errstr);
		if (errstr != NULL) {
			warnx("number %s: \"%s\"", errstr, cpp[i]);
			goto err;
		}
		numbers = addint(numbers, num);
	}

	return numbers;

err:
	clrintv(&numbers);

	return NULL;
}

/*
 * Get the boolean value of a setting, the setting must be either "yes" or "no".
 *
 * Stores the result in res.
 *
 * Returns 0 on success and -1 on error or if the setting is not set.
 */
int
getbsetting(char *key, int *res)
{
	char *cp;

	if ((cp = getsetting(key)) == NULL)
		return -1;

	if (strcasecmp(cp, "yes") == 0)
		*res = 1;
	else if (strcasecmp(cp, "no") == 0)
		*res = 0;
	else
		return -1;

	return 0;
}

/*
 * Get the numeric value of a setting.
 *
 * Stores the result in res.
 *
 * Returns 0 on success and -1 on error.
 */
int
getnsetting(char *key, int *res)
{
	const char *errstr;
	char *cp;

	if ((cp = getsetting(key)) == NULL)
		return -1;

	*res = strtonum(cp, INT_MIN, INT_MAX, &errstr);
	if (errstr != NULL)
		return -1;

	return 0;
}

/*
 * Determine the number of days in the month the given time lies in.
 *
 * Returns 28, 29, 30 or 31.
 */
int
daysinmonth(time_t t)
{
	const int daytab[2][12] = {
		{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
		{ 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
	};
	int leap;

	struct tm *tm;
	int days;

	tm = localtime(&t);
	if (tm == NULL)
		errx(1, "%s: localtime", __func__);

	leap = (tm->tm_year % 4 == 0 && tm->tm_year % 100 != 0) || tm->tm_year % 400 == 0;

	days = daytab[leap][tm->tm_mon];
	if (days < 28 || days > 31) {
		if (verbose > 1)
			warnx("days determined: %d", days);
		errx(1, "could not determine days in month for time: %lld", t);
	}

	return days;
}

/*
 * Parse special keyword intervals. Currently supports "hourly", "daily",
 * "weekly" and "monthly".
 *
 * Returns a snapinterval vector on success or NULL on failure.
 */
struct snapinterval **
parseintervals(void)
{
	struct snapinterval **siv;
	int retain, interval;

	/* resolve intervals */
	siv = NULL;

	if (getnsetting("hourly", &retain) != -1)
		if (retain > 0) {
			interval = 3600;
			siv = snaps_add_snapinterval(siv, snaps_alloc_snapinterval("hourly", retain, interval));
			if (verbose > 2)
				warnx("retaining %d hourly snapshots, interval: %d", retain, interval);
		}

	if (getnsetting("daily", &retain) != -1)
		if (retain > 0) {
			interval = 3600 * 24;
			siv = snaps_add_snapinterval(siv, snaps_alloc_snapinterval("daily", retain, interval));
			if (verbose > 2)
				warnx("retaining %d daily snapshots, interval: %d", retain, interval);
		}

	if (getnsetting("weekly", &retain) != -1)
		if (retain > 0) {
			interval = 3600 * 24 * 7;
			siv = snaps_add_snapinterval(siv, snaps_alloc_snapinterval("weekly", retain, interval));
			if (verbose > 2)
				warnx("retaining %d weekly snapshots, interval: %d", retain, interval);
		}

	if (getnsetting("monthly", &retain) != -1)
		if (retain > 0) {
			interval = 3600 * 24 * daysinmonth(starttime);
			siv = snaps_add_snapinterval(siv, snaps_alloc_snapinterval("monthly", retain, interval));
			if (verbose > 2)
				warnx("retaining %d monthly snapshots, interval: %d", retain, interval);
		}

	return siv;
}

/*
 * Parse a host string of the form: [user@]host[:path].
 *
 * If rruser, rhostname and rrpath are not NULL, results are set. Each result
 * can be NULL if it is not present in the input.
 *
 * Return 0 on success, -1 on error.
 */
int
parsehoststr(const char *inp, char **rruser, char **rhostname, char **rrpath)
{
	enum states { S, HORR, HOST, PATH, ERR, E };
	size_t vallen;
	int state, i;
	char *ruser, *hostname, *rpath, *cp, *str;

	if ((str = strdup(inp)) == NULL)
		err(1, "%s: strdup", __func__);

	/*
	 * Split input with '\0' characters, but only set values if they were
	 * not set within the block itself.
	 *
	 * According to sethostname(3) about any hostname is valid.
	 */

	state = S;

	cp = str;
	vallen = strlen(cp);

	ruser = hostname = rpath = NULL;

	/*
	 * Include terminating nul so we can finish up within the state
	 * machine.
	 */

	for (i = 0; i <= vallen; i++, cp++) {
		switch (state) {
		case S:
			if (*cp == '\0') {
				/*
				 * Empty host strings are ok as long as the
				 * block itself defines all three components.
				 */
				state = E;

			} else if (*cp == ':') {
				rpath = cp + 1;
				state = PATH;
			} else if (!iscntrl(*cp)) {
				state = HORR;
			} else {
				state = ERR;
			}

			break;
		case HORR: /* host or remote username */
			if (*cp == '@') {
				/* What we've seen up till now has been a username. */

				*cp = '\0';
				ruser = str;
				hostname = cp + 1;
				state = HOST;
			} else if (*cp == ':') {
				/* What we've seen up till now has been a hostname. */

				*cp = '\0';
				hostname = str;
				rpath = cp + 1;
				state = PATH;
			} else if (*cp == '\0') {
				/* What we've seen up till now has been a hostname. */

				/*
				 * Accept \0 because hostnames don't
				 * always need to be followed by a
				 * remote path.
				 */

				hostname = str;
				state = E;
			} else if (!iscntrl(*cp)) {

				/* see how far this goes.. */

			} else {
				state = ERR;
			}

			break;
		case HOST:
			if (*cp == '\0') {
				state = E;
			} else if (*cp == ':') {
				*cp = '\0';
				rpath = cp + 1;
				state = PATH;
			} else if (!iscntrl(*cp)) {

				/* see how far this goes.. */

			} else {
				state = ERR;
			}

			break;
		case PATH:
			if (*cp == '\0') {
				state = E;
			} else if (!iscntrl(*cp)) {

				/* see how far this goes.. */

			} else {
				state = ERR;
			}


			break;
		case ERR:
			break;
		case E:
			break;
		}
	}

	if (state != E) {
		free(str);
		return -1;
	}

	if (rruser != NULL) {
		if (ruser != NULL) {
			if ((*rruser = strdup(ruser)) == NULL)
				err(1, "%s: strdup", __func__);
		} else
			*rruser = NULL;
	}

	if (rhostname != NULL) {
		if (hostname != NULL) {
			if ((*rhostname = strdup(hostname)) == NULL)
				err(1, "%s: strdup", __func__);
		} else
			*rhostname = NULL;
	}

	if (rrpath != NULL) {
		if (rpath != NULL) {
			if ((*rrpath = strdup(rpath)) == NULL)
				err(1, "%s: strdup", __func__);
		} else
			*rrpath = NULL;
	}

	free(str);

	return 0;
}

/*
 * Return a host identification string based on the current settings in gset and
 * tmpepset.
 */
char *
getbackupid(void)
{
	static char hostid[HOST_NAME_MAX + PATH_MAX];

	if (snprintf(hostid, sizeof(hostid), "%s:%s",
		getsetting("hostname") == NULL ? "" : getsetting("hostname"),
		getsetting("rpath") == NULL ? "" : getsetting("rpath")) < 0) {

		warnx("can't determine hostid");
		hostid[0] = '\0';
	}

	return hostid;
}

/*
 * Call yyparse that should create a null terminated key/value vector of the
 * config file.
 * Then, in a first pass determine all global settings and in a second pass
 * create endpoints with endpoint specific settings.
 *
 * Return a pointer to the endpoint vector on success, exit on error.
 */
struct endpoint **
parseconfig(int cfgd)
{
	extern int yyparse(void);
	extern int yyd;
	struct scfgiteropts iteropts;

	yyd = cfgd;

	if (yyparse() != 0)
		errx(1, "%s: yyparse", __func__);

	if (verbose > 2)
		scfg_printr();

	memset(&iteropts, '\0', sizeof(iteropts));

	iteropts.maxdepth = 1;

	/* First pass: ensure global settings are set. */
	if (scfg_foreach(&iteropts, setgset) != 1)
		errx(1, "%s: not all global settings could be set", __func__);

	/* Second pass: create endpoints. */
	iteropts.key = "backup";
	if (scfg_foreach(&iteropts, createendpoint) != 1)
		errx(1, "%s: not all endpoints could be created", __func__);

	clrtmpkv(gset, gsetsize);
	scfg_clear();

	return epv;
}

/* Clear all values in a temporary key/value set. */
void
clrtmpkv(struct tmpkv *tmpkv, size_t size)
{
	while (size--) {
		tmpkv[size].val = NULL;
		tmpkv[size].mval = NULL;
	}
}
