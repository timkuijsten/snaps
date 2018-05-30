#include "syncer.h"

/*
 * Execs rsync and creates a new backup for the given snapshot interval.
 *
 * Does not return on success or returns on error.
 */
void
execrsync(const struct endpoint *ep, const char *destdir, const char *linkdest)
{
	int i;
	const char *rsyncbin;
	char **rsyncargv;
	char *fmt, *tmp;

	rsyncargv = NULL;
	fmt = tmp = NULL;

	/* Init rsync binary location. */

	if (ep->rsyncbin == NULL)
		rsyncbin = RSYNCBIN;
	else
		rsyncbin = ep->rsyncbin;

	rsyncargv = addstr(rsyncargv, basename(rsyncbin));
	rsyncargv = addstr(rsyncargv, "-az");
	rsyncargv = addstr(rsyncargv, "--delete");
	/* prevent pledges for getpw, unix and dpath */
	rsyncargv = addstr(rsyncargv, "--numeric-ids");
	rsyncargv = addstr(rsyncargv, "--no-specials");
	rsyncargv = addstr(rsyncargv, "--no-devices");
	rsyncargv = addstr(rsyncargv, "--chroot");
	rsyncargv = addstr(rsyncargv, ep->path);

	rsyncargv = addstr(rsyncargv, "--dropsuper");
	if (asprintf(&tmp, "%d", ep->uid) <= 0)
		err(1, "%s: asprintf", __func__);
	rsyncargv = addstr(rsyncargv, tmp);
	free(tmp);
	tmp = NULL;

	/* setup link-dest arg if there is a snapshot on disk */
	if (linkdest != NULL) {
		if (asprintf(&tmp, "--link-dest=%s", linkdest) <= 0)
			err(1, "%s: asprintf", __func__);
		rsyncargv = addstr(rsyncargv, tmp);
		free(tmp);
		tmp = NULL;
	}

	if (verbose < 0) /* quiet */
		rsyncargv = addstr(rsyncargv, "-q");
	else if (verbose > 1)
		for (i = 1; i < verbose; i++)
			rsyncargv = addstr(rsyncargv, "-v");

	/* Append user configured arguments. */
	if (ep->rsyncargv != NULL)
		for (i = 0; ep->rsyncargv[i] != NULL; i++)
			rsyncargv = addstr(rsyncargv, ep->rsyncargv[i]);

	/* setup remote arg: USER@HOST:SRC */

	/*
	 * Ensure SRC ends with a "/" because we already setup each path
	 * ourselves.
	 */

	fmt = "%s@%s:%s";
	i = strlen(ep->rpath);
	if (i > 0)
		if (ep->rpath[i - 1] != '/')
			fmt = "%s@%s:%s/";

	if (asprintf(&tmp, fmt, ep->ruser, ep->hostname, ep->rpath) <= 0)
		err(1, "%s: asprintf", __func__);
	rsyncargv = addstr(rsyncargv, tmp);
	free(tmp);
	tmp = NULL;

	/* setup destination path: DEST */
	rsyncargv = addstr(rsyncargv, destdir);

	if (verbose > 0)
		printstrv(rsyncargv);

	if (execvpe(rsyncbin, rsyncargv, NULL) == -1)
		err(1, "syncer[%d]: %s rsyncbin: %s", getpid(), getepid(ep),
			rsyncbin == NULL ? "empty" : rsyncbin);
}

/* Exec rsync for the only endpoint in memory. */
void
syncer(struct endpoint *ep)
{
	struct snapshot s;
	int cmd;
	char *cp, *cp2, *linkdest;

	if (pledge("stdio id rpath proc exec", NULL) == -1)
		err(1, "%s: pledge", __func__);

	/* expect stdout, stderr, pathfd and the communication channel only */
	if (isopenfd(STDOUT_FILENO) != 1)
		errx(1, "expected stdout to be open");
	if (isopenfd(STDERR_FILENO) != 1)
		errx(1, "expected stderr to be open");
	if (isopenfd(ep->pathfd) != 1)
		errx(1, "expected pathfd to be open");
	if (isopenfd(ep->synfd) != 1)
		errx(1, "expected communication channel to be open");
	if (getdtablecount() != 4)
		errx(1, "fd leak: %d", getdtablecount());

	/* Wait until we're ready to start. */
	if (readcmd(ep->synfd, &cmd) == -1)
		err(1, "%s: %s read error", __func__, getepid(ep));

	if (cmd != CMDSTART && cmd != CMDSTOP)
		errx(1, "%s: %s unexpected command: %d", __func__, getepid(ep),
			cmd);

	if (cmd == CMDSTOP)
		exit(0);

	/*
	 * Trust on hardened rsync and hope the configured user isn't being used
	 * for tasks in different trust domains.
	 */
	if (ep->uid == 0 || ep->gid == 0)
		errx(1, "syncer[%d]: %s configure a different user than the "
			"superuser", getpid(), getepid(ep));

	/*
	 * Change working dir to syncdir so that --link-dest works. Expect
	 * hrsync to change dir to the syncdir after chrooting as well.
	 */
	cp = getsyncdir();
	if (asprintf(&cp2, "%s/%s", ep->path, cp) <= 0)
		err(1, "%s: asprintf", __func__);
	free(cp);
	cp = NULL;

	if (chdir(cp2) == -1)
		err(1, "%s: chdir", __func__);
	free(cp2);
	cp2 = NULL;

	/*
	 * Determine the youngest snapshot interval on disk and setup linkdest
	 * if one is found, relative to the destination dir.
	 */
	linkdest = NULL;
	cp = NULL;

	if (newestsnapshot(ep, &s))
		cp = snapshotname(&s);

	if (cp != NULL) {
		if (asprintf(&linkdest, "../%s", cp) <= 0)
			err(1, "%s: asprintf", __func__);
		free(cp);
		cp = NULL;
	}

	if (verbose > 2)
		fprintf(stdout, "syncer[%d]: running as %d\n",
			getpid(),
			ep->uid);

	/*
	 * Use a relative path for destination dir so search permissions higher
	 * up the hierarchy are not needed.
	 */
	execrsync(ep, ".", linkdest); /* exec, so no need to free cp */

	errx(1, "%s: execrsync returned %s", __func__, getepid(ep));
}
