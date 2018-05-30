#include <sys/stat.h>
#include <sys/time.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fts.h>
#include <limits.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rotator.h"

static void movein(struct snapshot *, struct snapinterval *, time_t, int);
static void spreadout(struct endpoint *, time_t);

/*
 * Grant access for the syncer process to a snapshot on disk.
 *
 * Return 0 on success, or -1 on error with errno set.
 */
static int
allowsyncer(struct snapshot *s)
{
	/* 0755 */
	return setsnapshotmode(s, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH |
		S_IXOTH);
}

/*
 * Block access for the syncer process to a snapshot on disk.
 *
 * Return 0 on success, or -1 on error with errno set.
 */
static int
blocksyncer(struct snapshot *s)
{
	/* 0705 */
	return setsnapshotmode(s, S_IRWXU | S_IROTH | S_IXOTH);
}

/*
 * Create a new sync dir for the given root, make sure it is writable by the
 * endpoint group. It is considered an error if the syncdir already exists.
 *
 * Return 0 on success, or -1 on error and set errno.
 */
static int
newsyncdir(const struct endpoint *ep)
{
	int r;
	char *path;

	path = getsyncdir();

	r = 0;

	/*
	 * Ensure it's fully accessible for the owner and group only.
	 */

	if ((r = mkdirat(ep->pathfd, path, S_IRWXU | S_IRWXG)) == -1)
		goto end;

	if ((r = fchmodat(ep->pathfd, path, S_IRWXU | S_IRWXG, 0)) == -1)
		goto end;

	if ((r = fchownat(ep->pathfd, path, -1, ep->gid, 0)) == -1)
		goto end;

end:
	free(path);
	path = NULL;
	return r;
}

/*
 * Remove the given path recursively.
 *
 * Slimmed down copy of /bin/rm/rm.c.
 *
 *	$OpenBSD: rm.c,v 1.42 2017/06/27 21:49:47 tedu Exp $
 *	$NetBSD: rm.c,v 1.19 1995/09/07 06:48:50 jtc Exp $
 *
 * Copyright (c) 1990, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
void
rm_tree(char **argv)
{
	FTS *fts;
	FTSENT *p;
	int flags;

	flags = FTS_PHYSICAL | FTS_NOSTAT;

	if ((fts = fts_open(argv, flags, NULL)) == NULL)
		err(1, "fts_open");

	while ((p = fts_read(fts)) != NULL) {
		switch (p->fts_info) {
		case FTS_D:
			/* first pass */
			continue;

		case FTS_ERR:
			errc(1, p->fts_errno, "%s", p->fts_path);
			break;

		case FTS_DNR:
			/*
			 * If we can't read or search the directory, may still be
			 * able to remove it. Don't print out the un{read,search}able
			 * message unless the remove fails.
			 */
			errc(1, p->fts_errno, "%s", p->fts_path);
			if (rmdir(p->fts_accpath) == -1)
				warn("rmdir");
			break;

		case FTS_DP:
			if (rmdir(p->fts_accpath) == -1)
				err(1, "rmdir");
			break;

		default:
			if (unlink(p->fts_accpath) == -1)
				warn("%s", p->fts_path);
		}
	}

	if (errno != 0)
		err(1, "fts_read");
	if (fts_close(fts) == -1)
		err(1, "fts_close");
}

/*
 * Find the backup with the highest number in the given interval.
 *
 * Return the number of the oldest backup found or 0 if none is found. Return -1
 * on error and set errno.
 */
int
maxbackup(const char *ivalname)
{
	char *p;
	struct stat st;
	int n;

	p = NULL;
	n = 0;
	while (n < INT_MAX) {
		free(p);
		p = NULL;
		n++;

		if (asprintf(&p, "%s.%d", ivalname, n) <= 0)
			err(1, "%s: asprintf", __func__);

		if (stat(p, &st) == -1) {
			if (errno != ENOENT)
				err(1, "%s: stat", __func__);

			/* encountered the first non-existing dir */
			n--;
			break;
		}

		if ((st.st_mode & S_IFMT) != S_IFDIR) {
			errno = ENOTDIR;
			n = -1;
			break;
		}
	}
	if (n == INT_MAX) {
		errno = ERANGE;
		n = -1;
	}

	free(p);
	p = NULL;
	return n;
}

/* Queue deletion of a snapshot. */
void
qdel(const char *src)
{
	int i;
	char *dst;

	/*
	 * Determine the number of snapshots in DELIVAL.
	 */

	if ((i = maxbackup(DELIVAL)) == -1)
		err(1, "maxbackup %s", DELIVAL);

	/* maxbackup guarantees i < INT_MAX */
	if (i == (INT_MAX - 1))
		err(1, "max number of snapshots reached: %d in %s", i, DELIVAL);

	dst = snapdirstr(DELIVAL, i + 1);

	if (verbose > 0)
		fprintf(stdout, "rotator[%d]: %s -> %s\n", getpid(), src, dst);
	if (rename(src, dst) == -1)
		err(1, "%s: rename %s", __func__, src);

	free(dst);
	dst = NULL;
}

/*
 * Rotate backups for a given endpoint. Delete everything that falls out.
 *
 * On startup, check for orphaned sync dirs and if found, cleanup.
 */
void
rotator(struct endpoint *ep, time_t starttime, int force)
{
	struct snapshot s, firstoffirst, newestondisk;
	struct flock fl;
	struct stat st;
	int n, fd, cmd;
	time_t age, ttl;
	char *src[2], *tmp, *pathinfo;

	/* Sandbox */

	if (starttime < 1513295520)
		errx(1, "starttime error, check your clock");

	if ((pathinfo = strdup(ep->path)) == NULL)
		err(1, "%s: strdup", __func__);
	if (chroot(ep->path) == -1 || chdir("/") == -1)
		err(1, "%s: chroot %s", __func__, pathinfo);
	snaps_endpoint_chpath(ep, "/");

	if (pledge("stdio flock wpath rpath cpath fattr chown", NULL) == -1)
		err(1, "%s: pledge", __func__);

	/* expect stdout, stderr, pathfd and the communication channel only */
	if (isopenfd(STDOUT_FILENO) != 1)
		errx(1, "expected stdout to be open");
	if (isopenfd(STDERR_FILENO) != 1)
		errx(1, "expected stderr to be open");
	if (isopenfd(ep->pathfd) != 1)
		errx(1, "expected pathfd to be open");
	if (isopenfd(ep->rotfd) != 1)
		errx(1, "expected communication channel to be open");
	if (getdtablecount() != 4)
		errx(1, "fd leak: %d", getdtablecount());

	/* Wait until we're ready to start. */
	if (readcmd(ep->rotfd, &cmd) == -1)
		err(1, "%s: %s read error", __func__, getepid(ep));

	if (cmd != CMDSTART && cmd != CMDSTOP)
		errx(1, "%s: %s unexpected command: %d", __func__, getepid(ep),
			cmd);

	if (cmd == CMDSTOP)
		exit(0);

	/* Make sure there is an interval. */
	if (ep->snapshots == NULL || *ep->snapshots == NULL)
		errx(1, "%s: no snapshot interval configured", getepid(ep));

	/*
	 * We're done if the first snapshot of the first interval has not
	 * expired yet and force is false.
	 */

	if (setsnapshot(ep, ep->snapshots[0]->name, 1, &firstoffirst) == -1)
		err(1, "%s: setsnapshot", __func__);

	if ((ttl = snapshotttl(&firstoffirst, starttime, &age)) == -1)
		err(1, "%s: snapshotttl", __func__);

	if (!force && (ttl - TIMEPAD) > 0) {
		if (verbose > 0) {
			tmp = strdup(humanduration(age));
			fprintf(stdout, "  %s %s left (%s old)\n",
				getepid(ep), humanduration(ttl), tmp);
			free(tmp);
			tmp = NULL;
		}

		/*
		 * All done. Let exit close the communication channel and signal
		 * a cmdclosed to the parent.
		 */
		exit(0);
	}

	if (verbose > -1) {
		fprintf(stdout, "%s -> %s as %d:%d (", getepid(ep), pathinfo,
			ep->uid, ep->gid);

		if (ttl == 0 && age == 0)
			fprintf(stdout, "first %s backup)\n", firstoffirst.name);
		else
			fprintf(stdout, "%s old)\n", humanduration(age));
	}

	free(pathinfo);
	pathinfo = NULL;

	/*
	 * Obtain a lock or exit. Check if a syncdir exists and if so, cleanup.
	 * Always finalize by creating a fresh new sync dir and release the lock
	 * only on exit to signal duplicate copies of the rotator.
	 */

	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 0;

	if ((fd = open(LOCKFILE, O_WRONLY | O_CREAT | O_CLOEXEC, 0600)) == -1)
		err(1, "rotator[%d]: open %s", getpid(), LOCKFILE);

	if (fcntl(fd, F_SETLK, &fl) == -1) {
		if (errno != EAGAIN)
			err(1, "rotator[%d]: could not obtain a lock", getpid());

		/* get id of the process holding the lock */
		if (fcntl(fd, F_GETLK, &fl) == -1)
			err(1, "rotator[%d]: could not obtain lock info",
				getpid());
		if (fl.l_type == F_UNLCK)
			errx(1, "rotator[%d]: could not obtain lock at first, "
				"but now it's free, retry next time", getpid());

		errx(1, "rotator[%d]: %s process %d holds a lock", getpid(),
			getepid(ep), fl.l_pid);
	}

	/* Cleanup any left-behind sync dir. */

	if ((src[0] = getsyncdir()) == NULL)
		err(1, "rotator[%d]: getsyncdir", getpid());

	src[1] = NULL;

	if (fstatat(ep->pathfd, src[0], &st, AT_SYMLINK_NOFOLLOW) == -1) {
		if (errno != ENOENT)
			err(1, "rotator[%d]: could not stat sync dir",
				getpid());
	} else {
		if ((st.st_mode & S_IFMT) != S_IFDIR) {
			errno = ENOTDIR;
			err(1, "rotator[%d]: sync dir exists but is not"
				" a directory", getpid());
		}

		/*
		 * Schedule a delete of the orphaned dir so that our start won't
		 * be delayed.
		 */

		if (verbose > 0)
			fprintf(stdout, "rotator[%d]: scheduled delete of "
				"orphaned sync dir...\n", getpid());

		qdel(src[0]);
	}

	free(src[0]);
	src[0] = NULL;

	/*
	 * Setup a new sync dir for the syncer.
	 */

	if (newsyncdir(ep) == -1)
		err(1, "rotator[%d]: newsyncdir", getpid());

	/* Pledge drop flock, chown and wpath. */
	if (pledge("stdio rpath cpath fattr", NULL) == -1)
		err(1, "%s: pledge", __func__);

	/* Grant access to the newest snapshot for rsync link-dest optimization. */
	if (newestsnapshot(ep, &newestondisk))
		if (allowsyncer(&newestondisk) == -1)
			err(1, "rotator[%d]: allowsyncer", getpid());

	/* Signal that we're ready and wait until we may proceed. */
	if (writecmd(ep->rotfd, CMDREADY) == -1)
		err(1, "rotator[%d]: %s write ready error", getpid(),
			getepid(ep));
	if (readcmd(ep->rotfd, &cmd) == -1)
		err(1, "rotator[%d]: %s read error", getpid(), getepid(ep));

	/*
	 * Assume the parent waited for the syncer to exit so we're free to do
	 * whatever we want with the sync dir, without risking a compromised
	 * syncer playing tricks with us. If a previous syncer still runs a
	 * previous rotator should still run as well and we would never be able
	 * to obtain a lock.
	 */

	/*
	 * Revoke access from the syncer to the new snapshot and to the snapshot
	 * used as link-dest (if any).
	 *
	 * Note: the snapshot used as link-dest is still the newest as long as
	 * the new snapshot is not moved in.
	 */

	if (setsnapshot(ep, SYNCDIR, 1, &s) == -1)
		err(1, "%s: setsnapshot", __func__);

	if (blocksyncer(&s) == -1)
		err(1, "rotator[%d]: blocksyncer new snapshot", getpid());

	if (newestsnapshot(ep, &newestondisk))
		if (blocksyncer(&newestondisk) == -1)
			err(1, "rotator[%d]: blocksyncer previous snapshot",
				getpid());

	/* Reset the snapshot time for future reference. */
	setsnapshottime(&s, starttime);

	/* Pledge drop fattr. */
	if (pledge("stdio rpath cpath", NULL) == -1)
		err(1, "%s: pledge", __func__);

	if (cmd == CMDROTCLEANUP) {
		if (verbose > 0)
			fprintf(stdout, "rotator[%d]: remove %s\n", getpid(),
				SYNCDIR);

		tmp = getsyncdir();
		qdel(tmp);
		free(tmp);
		tmp = NULL;
	} else if (cmd == CMDROTINCLUDE) {
		/* Move the new snapshot into the first interval. */

		movein(&s, s.ep->snapshots[0], starttime, force);
		spreadout(ep, starttime);
	} else {
		errx(1, "rotator[%d]: %s unexpected command: %d", getpid(),
			getepid(ep), cmd);
	}

	/* Delete all snapshots in DELIVAL. */

	if ((n = maxbackup(DELIVAL)) == -1)
		err(1, "rotator[%d]: maxbackup %s", getpid(), DELIVAL);

	while (n) {
		src[0] = snapdirstr(DELIVAL, n);
		src[1] = NULL;

		if (verbose > 1)
			fprintf(stdout, "rotator[%d]: removing %s\n", getpid(),
				src[0]);

		rm_tree(src);
		free(src[0]);
		src[0] = NULL;

		n--;
	}

	/* We're done. */

	if (unlink(LOCKFILE) == -1)
		err(1, "unlink");

	exit(0);
}

/*
 * Move a new snapshot into an interval if there are expired snapshots or force
 * is true.
 */
static void
movein(struct snapshot *s, struct snapinterval *si, time_t starttime, int force)
{
	int i;
	char *src, *dst;
	struct snapshot s2;
	time_t age, ttl;

	/*
	 * Find the first non-expired snapshot in this interval (or non-
	 * existing).
	 */
	for(i = 1; ; i++) {
		if (setsnapshot(s->ep, si->name, i, &s2) == -1)
			err(1, "%s: setsnapshot", __func__);
		if ((ttl = snapshotttl(&s2, starttime, &age)) == -1)
			err(1, "%s: snapshotttl", __func__);

		if (ttl == 0 && age == 0)
			break; /* snapshot does not exist */

		if ((ttl - TIMEPAD) > 0)
			break; /* snapshot not expired */
	}

	if (verbose > 1)
		fprintf(stdout, "rotator[%d]: oldest non-expired %s: %d, ttl: "
			"%lld, age: %lld\n", getpid(), si->name, i, ttl, age);

	/* Point i to the oldest expired snapshot or 0. */
	i--;

	/*
	 * If a non-expired snapshot exists, delete the oldest expired.
	 */
	if ((ttl || age) && i > 0) {
		dst = snapdirstr(si->name, i);
		qdel(dst);
		free(dst);
		dst = NULL;
		i--;
	}

	/* Move existing expired snapshots up. */
	while (i > 0) {
		src = snapdirstr(si->name, i);
		dst = snapdirstr(si->name, i + 1);
		if (rename(src, dst) == -1)
			err(1, "rotator[%d]: %s: rename %s to %s",
				getpid(), __func__, src, dst);
		free(src);
		src = NULL;
		free(dst);
		dst = NULL;
		i--;
	}

	/* Determine ttl and age of first snapshot in interval. */
	if (setsnapshot(s->ep, si->name, 1, &s2) == -1)
		err(1, "%s: setsnapshot", __func__);
	if ((ttl = snapshotttl(&s2, starttime, &age)) == -1)
		err(1, "%s: snapshotttl", __func__);

	src = snapshotname(s);
	dst = snapshotname(&s2);

	/* If force is true, ensure the first position is free. */
	if ((ttl || age) && force) {
		qdel(dst);
		ttl = 0;
		age = 0;
	}

	/*
	 * If the first position is free, move in the new snapshot else delete
	 * it.
	 */

	if (ttl || age)
		qdel(src);
	else {
		if (verbose > 1)
			fprintf(stdout, "rotator[%d]: %s -> %s\n",
				getpid(), src, dst);
		if (rename(src, dst) == -1)
			err(1, "rotator[%d]: %s: rename %s", getpid(), __func__,
				src);
	}

	free(src);
	src = NULL;
	free(dst);
	dst = NULL;
}

/*
 * Spread extraneous snapshots between intervals. The newest extraneous snapshot
 * is put into the next interval if there is a next interval and if the first
 * snapshot in that interval has expired or does not exist.
 *
 * All remaining extranous snapshots in each interval are queued for deletion.
 */
static void
spreadout(struct endpoint *ep, time_t starttime)
{
	struct snapinterval **siv, *nsi;
	struct snapshot s;
	char *tmp;
	int n;

	/*
	 * For each interval, start with the oldest snapshot and work our way to
	 * the newest so that interruptions don't break the line.
	 */

	for (siv = ep->snapshots; *siv; siv++) {
		/* next snapinterval */
		nsi = *(siv + 1);

		if ((n = maxbackup((*siv)->name)) == -1)
			err(1, "rotator[%d]: %s: maxbackup %s", getpid(),
				__func__, (*siv)->name);

		/*
		 * Delete all extraneous snapshots except for the newest.
		 */

		while (n - 1 > (*siv)->count) {
			tmp = snapdirstr((*siv)->name, n);
			qdel(tmp);
			free(tmp);
			tmp = NULL;
			n--;
		}

		/*
		 * Try to move the newest extraneous snapshot into the next
		 * interval without force, or delete it.
		 */

		if (n > (*siv)->count) {
			if (nsi == NULL) {
				tmp = snapdirstr((*siv)->name, n);
				qdel(tmp);
				free(tmp);
				tmp = NULL;
			} else {
				if (setsnapshot(ep, (*siv)->name, n, &s) == -1)
					err(1, "%s: setsnapshot", __func__);

				movein(&s, nsi, starttime, 0);
			}
			n--;
		}
	}
}
