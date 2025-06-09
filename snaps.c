#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "parseconfig.h"
#include "rotator.h"
#include "syncer.h"

#define VERSION "1.0.0-beta1"

#define CONFIGFILE "/etc/snaps.conf"
#define EMPTYDIR "/var/empty"

int verbose = 0;
int helpopt = 0;
int versopt = 0;
int forceopt = 0;	/* Force a backup, no matter how long ago the last was
			 * taken.
			 */
int cfgcheckonly = 0;	/* Do a configuration check only. */

/*
 * Use the time the program is started to determine the interval with a
 * previously created backup.
 */
time_t starttime;

void print_usage(FILE *);

/*
 * Communication with the other processes is as follows:
 *
 * main (trusted):
 *	if postexec
 *		fork postexec
 *	fork rotator
 *	fork syncer
 *	wait for rotator ready or done
 *	if rotator ready
 *		signal syncer to start
 *		wait for syncer exit
 *		if postexec
 *			signal postexec syncer exit status
 *			wait for postexec exit
 *			if postexec was succesful
 *				signal rotator to rollin
 *			else
 *				signal rotator to cleanup
 *		else if postexec not configured
 *			if syncer was succesful
 *				signal rotator to rollin
 *			else
 *				signal rotator to cleanup
 *	else if rotator done
 *		signal syncer to stop
 *		if postexec
 *			signal postexec to stop
 *	wait for rotator exit
 *
 * rotator (trusted):
 *	ensure root
 *	obtain a lock (might be locked by an older rotator)
 *	cleanup any left-behind sync dir
 *	create new sync dir
 *	signal parent ready
 *	wait for signal from parent, either rollin or cleanup
 *	then move or remove the sync dir, depending on signal
 *	exit
 *
 * syncer (untrusted, after rsync started):
 *	wait for start signal from parent
 *	close communication channel with parent
 *	fork/exec rsync
 */
int
main(int argc, char *argv[])
{
	struct endpoint **epv;
	int cmd, c, n, commfd[2], i, **rsyncexit, trusted, exists, updated;
	char *cfgfile, *hostid, **filters, **cpp;
	mode_t relax, mode;
	extern int opterr;

	if ((starttime = time(NULL)) == -1)
		err(1, "could not determine current time");

	cfgfile = NULL;
	filters = NULL;

	opterr = 0;
	while ((c = getopt(argc, argv, "c:fhnqs:vV")) != -1)
		switch(c) {
		case 'c':
			if ((cfgfile = strdup(optarg)) == NULL)
				err(1, "strdup");
			break;
		case 'f':
			forceopt = 1;
			break;
		case 'h':
			helpopt = 1;
			break;
		case 'n':
			cfgcheckonly = 1;
			break;
		case 'q':
			verbose--;
			break;
		case 's':
			if (strlen(optarg) == 0)
				errx(1, "empty host filter specified");
			filters = addstr(filters, optarg);
			break;
		case 'v':
			verbose++;
			break;
		case 'V':
			versopt = 1;
			break;
		case '?':
		case ':':
			print_usage(stderr);
			exit(1);
		}

	argc -= optind;
	argv += optind;

	if (argc > 0) {
		print_usage(stderr);
		exit(1);
	}

	if (helpopt)
		print_usage(stdout);

	if (versopt)
		fprintf(stdout, "%s\n", VERSION);

	if (helpopt || versopt)
		exit(0);

	/* Ensure stdout is line buffered, even when run non-interactively. */
	if (setvbuf(stdout, NULL, _IOLBF, 0) != 0)
		errx(1, "setvbuf");

	if (cfgfile == NULL)
		if ((cfgfile = strdup(CONFIGFILE)) == NULL)
			err(1, "strdup");

	/*
	 * Check owner and permissions of the config file. It should be owned by
	 * the superuser and wheel. It's ok if it's readable by wheel.
	 */

	if (trustedpath(cfgfile, S_IRGRP, 0, &trusted, &exists) == -1)
		err(1, "%s: trustedpath", __func__);

	if (trusted && !exists) {
		errno = ENOENT;
		err(1, "%s", cfgfile);
	}

	if (!trusted) {
		errx(1, "%s untrusted.\nAcceptable ownership and permissions: "
			"root:wheel rw-r-----.\n"
			"Furthermore all path components leading up to the file"
			" must be owned by the\nsuperuser and none should be "
			"writable by the group or others.", cfgfile);
	}

	/* Open and parse config file. */

	if ((n = open(cfgfile, O_RDONLY | O_CLOEXEC)) == -1)
		err(1, "%s", cfgfile);
	if ((epv = parseconfig(n)) == NULL)
		errx(0, "no hosts to backup");
	if (close(n) == -1)
		err(1, "%s: close", __func__);

	if (cfgcheckonly) {
		if (verbose > -1)
			fprintf(stdout, "%s OK\n", cfgfile);
		free(cfgfile);
		cfgfile = NULL;
		exit(0);
	}

	free(cfgfile);
	cfgfile = NULL;

	if (close(STDIN_FILENO) == -1)
		err(1, "%s: close", __func__);

	/* expect stdout and stderr only */
	if (isopenfd(STDOUT_FILENO) != 1)
		errx(1, "expected stdout to be open");
	if (isopenfd(STDERR_FILENO) != 1)
		errx(1, "expected stderr to be open");
	if (getdtablecount() != 2)
		errx(1, "fd leak: %d", getdtablecount());

	if (chdir("/") == -1)
		err(1, "can't change directory to /");

	if (geteuid() != 0)
		errx(1, "must run as the superuser");

	/* Defaut to confidentiality and integrity. */
	umask(077);

	/*
	 * If one or more host filters are used, filter out any hosts that don't
	 * match any filter.
	 */
	if (filters) {
		for (n = 0; epv[n] != NULL; n++) {
			hostid = getepid(epv[n]);
			if (strlen(hostid) == 0)
				errx(1, "could not determine host id");

			/* Find first matching filter. */
			for (cpp = filters; *cpp; cpp++)
				if (strstr(hostid, *cpp))
					break;

			if (*cpp == NULL) {
				/* host does not match any filter */
				epv = snaps_rm_endpoint(epv, epv[n]);
				n--;
			}
		}

		clrstrv(&filters);
	}

	/*
	 * Make sure the root dir and endpoint path ownership and permissions
	 * are ok and create directories or fix permissions where possible.
	 * Remove endpoints where anything is unrecoverably wrong.
	 *
	 * All existing components must be owned by the superuser and none
	 * should be writable by the group or by others.
	 *
	 * If the endpoint is shared, the mode of the root dir will be set to
	 * 0750 and the mode of the endpoint path to 0751. If the endpoint is
	 * not shared the mode will be set to 0700 and 0711, respectively. Use
	 * "dir dropping" to let unprivileged processes access contents in the
	 * root dir.
	 */
	n = 0;
	while (epv[n] != NULL) {

		/*
		 * Make sure the following conditions hold for the root dir:
		 *   - it's absolute
		 *   - it's either 700 or 750 if shared
		 *   - if the path does not exist yet, createroot must be set
		 */

		if (!isabsolutepath(epv[n]->root)) {
			warnx("%s: root must be set to an absolute path: "
				"\"%s\"", getepid(epv[n]), epv[n]->root);
			epv = snaps_rm_endpoint(epv, epv[n]);
			continue;
		}

		relax = 0;
		if (epv[n]->shared != UNSHARED)
			relax = S_IRGRP | S_IXGRP;

		if (trustedpath(epv[n]->root, relax, epv[n]->shared, &trusted,
		    &exists) == -1)
			err(1, "%s: trustedpath", __func__);

		if (!trusted) {
			warnx("%s: %s is untrusted.\nMake sure all components "
				"are owned by the superuser and none are "
				"writable by the\ngroup or others. The last "
				"component must have mode %s.",
				getepid(epv[n]),
				epv[n]->root,
				epv[n]->shared == UNSHARED ? "0700" : "0750");
			epv = snaps_rm_endpoint(epv, epv[n]);
			continue;
		}

		if (!exists && !epv[n]->createroot) {
			warnx("%s: make sure the root \"%s\" exists or set "
				"createroot to \"yes\"",
				getepid(epv[n]), epv[n]->root);
			epv = snaps_rm_endpoint(epv, epv[n]);
			continue;
		}

		mode = S_IRWXU;
		if (epv[n]->shared != UNSHARED)
			mode |= S_IRGRP | S_IXGRP;

		if (secureensuredir(epv[n]->root, mode, epv[n]->shared,
		    &updated) == -1)
			return -1;

		if (updated)
			warnx("%s: updated ownership and permissions of \"%s\"",
				getepid(epv[n]), epv[n]->root);

		/*
		 * Same thing for the endpoint path. Mode should be either 711
		 * or 751 depending on whether it's shared or not.
		 */

		relax = S_IXGRP | S_IXOTH;
		if (epv[n]->shared != UNSHARED)
			relax |= S_IRGRP;

		if (trustedpath(epv[n]->path, relax, epv[n]->shared, &trusted,
		    &exists) == -1)
			err(1, "%s: trustedpath", __func__);

		if (!trusted) {
			if (epv[n]->shared == UNSHARED) {
				warnx("insecure mode: path must be "
					"owned by wheel and must not be"
					" readable or writable by the "
					"group or others: %s",
					epv[n]->path);
			} else {
				warnx("insecure mode: path must be "
					"owned by group id %d and must "
					"not be writable by the group "
					"or readable or writable by "
					"others: %s", epv[n]->shared,
					epv[n]->path);
			}
			epv = snaps_rm_endpoint(epv, epv[n]);
			continue;
		}

		mode = S_IRWXU | S_IXGRP | S_IXOTH;
		if (epv[n]->shared != UNSHARED)
			mode |= S_IRGRP;

		if (secureensuredir(epv[n]->path, mode, epv[n]->shared,
		    &updated) == -1)
			return -1;

		if (updated)
			warnx("%s: updated ownership and permissions of \"%s\"",
				getepid(epv[n]), epv[n]->path);

		n++;
	}

	/*
	 * Pre-fork rotators and syncers.
	 */

	for (n = 0; epv[n] != NULL; n++) {

		/*
		 * Fork and start postexec if configured. Remove other endpoints
		 * from the new address space.
		 */

		if (epv[n]->postexec != NULL) {
			/* setup a communication channel to postexec  */
			if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC,
			    AF_UNSPEC, commfd) == -1)
				err(1, "could not setup a communication channel");

			if ((epv[n]->poxpid = fork()) == -1)
				err(1, "could not fork postexec");

			if (epv[n]->poxpid == 0) {
				if (verbose > 1)
					fprintf(stdout, "postexec[%d]: %s forked\n",
						getpid(), getepid(epv[n]));

				if (close(commfd[0]) == -1)
					err(1, "closing peer side");
				epv[n]->poxfd = commfd[1];

				/*
				 * Open a file descriptor to the root dir to
				 * work with.
				 */
				snaps_endpoint_openrootfd(epv[n]);

				/*
				 * Remove other endpoints.
				 */

				epv = snaps_keep_one_endpoint(epv, epv[n]);

				setproctitle("postexec %s", getepid(epv[0]));

				postexec(epv[0]);

				errx(1, "unexpected return of postexec");
			} else {
				if (close(commfd[1]) == -1)
					err(1, "closing peer side");
				epv[n]->poxfd = commfd[0];
			}
		}

		/*
		 * Fork and start a rotator. Remove other endpoints from the new
		 * address space.
		 */

		/* setup a communication channel to the rotator */
		if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, AF_UNSPEC,
		    commfd) == -1)
			err(1, "could not setup a communication channel");

		if ((epv[n]->rotpid = fork()) == -1)
			err(1, "could not fork rotator");

		if (epv[n]->rotpid == 0) {
			if (verbose > 1)
				fprintf(stdout, "rotator[%d]: %s forked\n",
					getpid(), getepid(epv[n]));

			/*
			 * Close the communication channel with postexec if
			 * there was any.
			 */

			if (epv[n]->postexec != NULL) {
				if (close(epv[n]->poxfd) == -1)
					err(1, "close communication channel to "
						"postexec in the rotator");
				epv[n]->poxfd = -1;
			}

			if (close(commfd[0]) == -1)
				err(1, "closing peer side");
			epv[n]->rotfd = commfd[1];

			/*
			 * Open a file descriptor to the root dir to
			 * work with.
			 */
			snaps_endpoint_openrootfd(epv[n]);

			/*
			 * Remove other endpoints.
			 */

			epv = snaps_keep_one_endpoint(epv, epv[n]);

			setproctitle("rotator %s", getepid(epv[0]));

			rotator(epv[0], starttime, forceopt);

			errx(1, "unexpected return of rotator");
		} else {
			if (close(commfd[1]) == -1)
				err(1, "closing peer side");
			epv[n]->rotfd = commfd[0];
		}

		/*
		 * Fork and start a syncer process. Remove other endpoints from
		 * the new address space.
		 */

		/* setup a communication channel to the syncer process */
		if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, AF_UNSPEC,
		    commfd) == -1)
			err(1, "could not setup a communication channel");

		if ((epv[n]->synpid = fork()) == -1)
			err(1, "could not fork syncer");

		if (epv[n]->synpid == 0) {
			if (verbose > 1)
				fprintf(stdout, "syncer[%d]: %s forked\n",
					getpid(), getepid(epv[n]));

			/*
			 * Close the communication channel with postexec if
			 * there was any.
			 */

			if (epv[n]->postexec != NULL) {
				if (close(epv[n]->poxfd) == -1)
					err(1, "close communication channel to "
						"postexec in the syncer");
				epv[n]->poxfd = -1;
			}

			/*
			 * Open a file descriptor to the root dir to
			 * work with.
			 */
			snaps_endpoint_openrootfd(epv[n]);

			/*
			 * Close the communication channel with the rotator.
			 */

			if (close(epv[n]->rotfd) == -1)
				err(1, "close communication channel to the "
					"rotator in the syncer");
			epv[n]->rotfd = -1;

			/*
			 * Setup communication channel.
			 */

			if (close(commfd[0]) == -1)
				err(1, "closing peer side");
			epv[n]->synfd = commfd[1];

			/*
			 * Remove other endpoints.
			 */

			epv = snaps_keep_one_endpoint(epv, epv[n]);

			setproctitle("syncer %s", getepid(epv[0]));

			syncer(epv[0]);

			errx(1, "unexpected return of syncer");
		} else {
			if (close(commfd[1]) == -1)
				err(1, "closing peer side");
			epv[n]->synfd = commfd[0];
		}
	}

	/*
	 * Chroot, pledge and wait for the first rotator to be ready, signal the
	 * accompanying syncer to start, repeat for each backup host, one after
	 * the other.
	 */

	if (chroot(EMPTYDIR) == -1 || chdir("/") == -1)
		err(1, "%s: chroot %s", __func__, EMPTYDIR);
	if (pledge("stdio", NULL) == -1)
		err(1, "%s: pledge", __func__);

	for (n = 0; epv[n] != NULL; n++) {
		/*
		 * Signal the rotator to start and wait for it to signal it's
		 * ready or done.
		 */

		if (writecmd(epv[n]->rotfd, CMDSTART) == -1)
			err(1, "%s: write rotator start signal",
				getepid(epv[n]));
		if (readcmd(epv[n]->rotfd, &cmd) == -1)
			err(1, "%s: rotator read error", getepid(epv[n]));

		if (cmd != CMDCLOSED && cmd != CMDREADY)
			err(1, "%s: unexpected signal from rotator %d",
				getepid(epv[n]), cmd);

		/*
		 * Signal the syncer to either start or stop, same with postexec
		 * if it is running.
		 */

		if (cmd == CMDREADY) {
			if (writecmd(epv[n]->synfd, CMDSTART) == -1)
				err(1, "%s: write syncer start signal",
					getepid(epv[n]));

			if (close(epv[n]->synfd) == -1)
				err(1, "%s: closing communication channel to syncer",
					getepid(epv[n]));
			epv[n]->synfd = -1;

			if ((i = reapproc(epv[n]->synpid)) == -1)
				errx(1, "%s: reap syncer error", getepid(epv[n]));

			if (verbose > 1)
				fprintf(stdout, "%s: syncer[%d] exit %d\n",
					getepid(epv[n]), epv[n]->synpid, i);

			/*
			 * Signal postexec the syncer exit status or -1 if an error
			 * occurred, close the communication channel and wait until it's
			 * done.
			 */

			if (epv[n]->postexec != NULL) {
				if (writecmd(epv[n]->poxfd, CMDCUST) == -1)
					err(1, "%s: write postexec custom signal",
						getepid(epv[n]));
				if (writecmd(epv[n]->poxfd, i) == -1)
					err(1, "%s: write postexec syncer exit status",
						getepid(epv[n]));
				if (close(epv[n]->poxfd) == -1)
					err(1, "%s: closing communication channel to postexec",
						getepid(epv[n]));
				if ((i = reapproc(epv[n]->poxpid)) == -1)
					errx(1, "%s: reap postexec error", getepid(epv[n]));
				if (verbose > 1)
					fprintf(stdout, "%s: postexec[%d] exit %d\n",
						getepid(epv[n]), epv[n]->poxpid, i);
				epv[n]->poxpid = -1;
			}

			/*
			 * Depending on the termination status of the syncer or
			 * postexec if configured, signal the rotator to either
			 * cleanup, or rollin the new snapshot.
			 */

			if (epv[n]->postexec == NULL) {

				/*
				 * i contains the rsync exit status, see if it
				 * indicates success.
				 */

				if (i != 0 && epv[n]->rsyncexit) {
					for (rsyncexit = epv[n]->rsyncexit;
					    *rsyncexit; rsyncexit++) {
						if (i == **rsyncexit) {
							i = 0;
							break;
						}
					}
				}

				if (i != 0)
					warnx("%s: syncer[%d] exit %d",
						getepid(epv[n]), epv[n]->synpid, i);
			}

			epv[n]->synpid = -1;

			if (i == 0) {
				if (writecmd(epv[n]->rotfd, CMDROTINCLUDE) == -1)
					err(1, "%s: write rotator signal",
						getepid(epv[n]));
			} else {
				if (writecmd(epv[n]->rotfd, CMDROTCLEANUP) == -1)
					err(1, "%s: write rotator signal",
						getepid(epv[n]));
			}
		} else {
			/*
			 * Signal the syncer and optionally postexec to stop.
			 * Close communication channels after sending the signal
			 * and reap.
			 */

			/* First the syncer. */
			if (writecmd(epv[n]->synfd, CMDSTOP) == -1)
				err(1, "%s: write syncer stop signal",
					getepid(epv[n]));
			if (close(epv[n]->synfd) == -1)
				err(1, "%s: closing communication channel to syncer",
					getepid(epv[n]));
			epv[n]->synfd = -1;
			if ((i = reapproc(epv[n]->synpid)) == -1)
				errx(1, "%s: reap syncer error", getepid(epv[n]));
			if (verbose > 1)
				fprintf(stdout, "%s: syncer[%d] exit %d\n",
					getepid(epv[n]), epv[n]->synpid, i);
			epv[n]->synpid = -1;

			/* Then postexec, if configured. */
			if (epv[n]->postexec != NULL) {
				if (writecmd(epv[n]->poxfd, CMDSTOP) == -1)
					err(1, "%s: write postexec stop signal",
						getepid(epv[n]));
				if (close(epv[n]->poxfd) == -1)
					err(1, "%s: closing communication channel to postexec",
						getepid(epv[n]));
				if ((i = reapproc(epv[n]->poxpid)) == -1)
					errx(1, "%s: reap postexec error", getepid(epv[n]));
				if (verbose > 1)
					fprintf(stdout, "%s: postexec[%d] exit %d\n",
						getepid(epv[n]), epv[n]->poxpid, i);
				epv[n]->poxpid = -1;
			}
		}

		/*
		 * At last the rotator. It signalled us it was done or we
		 * signalled it to finish up, so no need to send the stop
		 * signal. It should already have exited. Close the
		 * communication-channel with the rotator and reap.
		 */

		if (close(epv[n]->rotfd) == -1)
			warn("%s: closing communication channel to rotator",
				getepid(epv[n]));
		epv[n]->rotfd = -1;

		if ((i = reapproc(epv[n]->rotpid)) == -1)
			errx(1, "%s: rotator[%d] reap rotator error",
				getepid(epv[n]), epv[n]->rotpid);

		if (verbose > 1)
			fprintf(stdout, "%s: rotator[%d] exit %d\n",
				getepid(epv[n]), epv[n]->rotpid, i);

		if (i != 0)
			warnx("%s: rotator[%d] exit %d",
				getepid(epv[n]), epv[n]->rotpid, i);
		epv[n]->rotpid = -1;
	}

	return 0;
}

void
print_usage(FILE *fp)
{
	fprintf(fp, "usage: %s [-fhnqvV] [-c configfile] [-s filter]\n", getprogname());
}
