#include "util.h"

extern int verbose;

const int CMDCLOSED	= 0x0000,
	CMDSTART	= 0x0001,
	CMDSTOP	= 0x0002,
	CMDREADY	= 0x0004,
	CMDROTCLEANUP	= 0x0008,
	CMDROTINCLUDE	= 0x000c,
	CMDCUST		= 0x0010;	/* Followed by a custom integer. */

static struct snapinterval *snapshotinterval(const struct snapshot *);

struct snapinterval *
snaps_alloc_snapinterval(char *name, int count, time_t lifetime)
{
	struct snapinterval *si;

	si = malloc(sizeof(struct snapinterval));
	if (si == NULL)
		err(1, "%s: malloc", __func__);

	si->name = strdup(name);
	if (si->name == NULL)
		err(1, "%s: strdup", __func__);

	si->count = count;
	si->lifetime = lifetime;

	return si;
}

void
snaps_free_snapinterval(struct snapinterval **si)
{
	if (*si == NULL)
		return;

	free((*si)->name);
	(*si)->name = NULL;

	(*si)->count = 0;
	(*si)->lifetime = 0;

	free(*si);
	*si = NULL;
}

/* Add a new snapinterval while ensuring order by lifetime. */
struct snapinterval **
snaps_add_snapinterval(struct snapinterval **siv, struct snapinterval *si)
{
	int i;

	/* find first empty slot */
	for (i = 0; siv != NULL && siv[i] != NULL; i++)
		;

	/* ensure enough space */
	siv = reallocarray(siv, i + 2, sizeof(si));
	if (siv == NULL)
		err(1, "%s: reallocarray", __func__);

	/* terminate new vector */
	siv[i+1] = NULL;

	/* don't just append, ensure order from short to long lifetime */
	for ( ; i > 0 && si->lifetime < siv[i-1]->lifetime; i--)
		siv[i] = siv[i-1];
	siv[i] = si;

	return siv;
}

/* Free all intervals in siv and siv itself. */
void
snaps_clear_snapintervalv(struct snapinterval ***siv)
{
	int n;

	if (*siv == NULL)
		return;

	for (n = 0; (*siv)[n] != NULL; n++)
		snaps_free_snapinterval(&(*siv)[n]);

	free(*siv);
	*siv = NULL;
}

struct endpoint *
snaps_alloc_endpoint(char *ruser, char *hostname, char *rpath, char *root,
	int createroot, gid_t shared, uid_t uid, gid_t gid,
	struct snapinterval **snapshots)
{
	char *pathcomp;
	struct endpoint *ep;

	ep = malloc(sizeof(struct endpoint));
	if (ep == NULL)
		err(1, "%s: malloc", __func__);

	ep->ruser = strdup(ruser);
	if (ep->ruser == NULL)
		err(1, "%s: strdup", __func__);
	ep->hostname = strdup(hostname);
	if (ep->hostname == NULL)
		err(1, "%s: strdup", __func__);
	ep->rpath = strdup(rpath);
	if (ep->rpath == NULL)
		err(1, "%s: strdup", __func__);
	ep->root = strdup(root);
	if (ep->root == NULL)
		err(1, "%s: strdup", __func__);

	ep->createroot = createroot;
	ep->shared = shared;
	ep->uid = uid;
	ep->gid = gid;

	/* Base local path on the root, hostname and remote path. */

	if (asprintf(&pathcomp, "%s/%s", ep->hostname, ep->rpath) <= 0)
		err(1, "%s: asprintf", __func__);

	if (normalize_pathcomp(pathcomp) != 0)
		errx(1, "%s could not normalize path component: %s", __func__,
			pathcomp);

	/* set path by prepending the root */
	if (asprintf(&ep->path, "%s/%s", ep->root, pathcomp) <= 0)
		err(1, "%s: asprintf", __func__);

	free(pathcomp);
	pathcomp = NULL;

	ep->pathfd = -1;

	ep->snapshots = snapshots;

	ep->rsyncbin = NULL;
	ep->rsyncargv = NULL;
	ep->rsyncexit = NULL;
	ep->postexec = NULL;

	ep->rotfd = -1;
	ep->rotpid = -1;
	ep->synfd = -1;
	ep->synpid = -1;
	ep->poxfd = -1;
	ep->poxpid = -1;

	return ep;
}

void
snaps_free_endpoint(struct endpoint **ep)
{
	if (*ep == NULL)
		return;

	free((*ep)->ruser);
	(*ep)->ruser = NULL;
	free((*ep)->hostname);
	(*ep)->hostname = NULL;
	free((*ep)->rpath);
	(*ep)->rpath = NULL;
	free((*ep)->root);
	(*ep)->root = NULL;
	free((*ep)->path);
	(*ep)->path = NULL;

	(*ep)->createroot = 0;
	(*ep)->shared = -1;
	(*ep)->uid = -1;
	(*ep)->gid = -1;

	free((*ep)->rsyncbin);
	(*ep)->rsyncbin = NULL;

	clrstrv(&(*ep)->rsyncargv); /* rsyncargv is set to null */
	clrintv(&(*ep)->rsyncexit); /* rsyncexit is set to null */

	free((*ep)->postexec);
	(*ep)->postexec = NULL;

	/* close fd to the path */
	if (close((*ep)->pathfd) == -1 && errno != EBADF)
		err(1, "close pathfd");
	(*ep)->pathfd = -1;

	/* close fd to the rotator */
	if (close((*ep)->rotfd) == -1 && errno != EBADF)
		err(1, "close rotfd");
	(*ep)->rotfd = -1;

	/* close fd to the syncer */
	if (close((*ep)->synfd) == -1 && errno != EBADF)
		err(1, "close synfd");
	(*ep)->synfd = -1;

	/* close fd to postexec */
	if (close((*ep)->poxfd) == -1 && errno != EBADF)
		err(1, "close poxfd");
	(*ep)->poxfd = -1;

	(*ep)->rotpid = -1;
	(*ep)->synpid = -1;
	(*ep)->poxpid = -1;

	snaps_clear_snapintervalv(&(*ep)->snapshots);

	free(*ep);
	*ep = NULL;
}

void
snaps_endpoint_setopts(struct endpoint *ep, char *rsyncbin, char **rsyncargv,
	int **rsyncexit, char *postexec)
{
	if (ep == NULL)
		return;

	free(ep->rsyncbin);
	ep->rsyncbin = NULL;

	clrstrv(&ep->rsyncargv); /* rsyncargv is set to null */
	clrintv(&ep->rsyncexit); /* rsyncexit is set to null */

	free(ep->postexec);
	ep->postexec = NULL;

	if (rsyncbin != NULL) {
		ep->rsyncbin = strdup(rsyncbin);
		if (ep->rsyncbin == NULL)
			err(1, "%s: strdup", __func__);
	}

	/* NULL is a valid value to the dup functions. exits on error */
	ep->rsyncargv = dupstrv(rsyncargv);
	ep->rsyncexit = dupintv(rsyncexit);

	if (postexec != NULL) {
		ep->postexec = strdup(postexec);
		if (ep->postexec == NULL)
			err(1, "%s: strdup", __func__);
	}
}

struct endpoint **
snaps_add_endpoint(struct endpoint **epv, struct endpoint *ep)
{
	int i;

	if (ep == NULL)
		return epv;

	/* find first empty slot */
	for (i = 0; epv != NULL && epv[i] != NULL; i++)
		;

	/* ensure enough space */
	epv = reallocarray(epv, i + 2, sizeof(ep));
	if (epv == NULL)
		err(1, "%s: reallocarray", __func__);

	epv[i] = ep;

	/* terminate vector */
	epv[i+1] = NULL;

	return epv;
}

/*
 * Ensure ep is freed and no longer part of epv.
 */
struct endpoint **
snaps_rm_endpoint(struct endpoint **epv, struct endpoint *ep)
{
	int n;

	if (epv == NULL)
		return epv;

	/* find ep */
	for (n = 0; epv[n] != NULL && epv[n] != ep; n++)
		;

	if (epv[n] == NULL)
		return epv;

	/* free and compact */
	snaps_free_endpoint(&epv[n]);
	do {
		epv[n] = epv[n + 1];
		n++;
	} while (epv[n] != NULL);

	/* resize */
	epv = reallocarray(epv, n, sizeof(ep));
	if (epv == NULL)
		err(1, "%s: reallocarray", __func__);

	return epv;
}

/* Open endpoint root dir for relative path reference. */
void
snaps_endpoint_openrootfd(struct endpoint *ep)
{
	/* (re)open fd to new path */
	if (close(ep->pathfd) == -1 && errno != EBADF)
		err(1, "%s: close pathfd", __func__);
	ep->pathfd = open(ep->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC |
		O_NOFOLLOW);
        if (ep->pathfd == -1)
		err(1, "%s: open pathfd %s", __func__, ep->path);
}

/*
 * Set a new path for the endpoint and (re)open a file descriptor to it.
 */
void
snaps_endpoint_chpath(struct endpoint *ep, const char *npath)
{
	if (ep == NULL)
		return;

	free(ep->path);
	ep->path = strdup(npath);
	if (ep->path == NULL)
		err(1, "%s: strdup", __func__);
	snaps_endpoint_openrootfd(ep);
}

/*
 * Keep one endpoint and remove all the others.
 *
 * Returns a new pointer to a possibly relocated epv, exits if memory allocation
 * fails.
 */
struct endpoint **
snaps_keep_one_endpoint(struct endpoint **epv, struct endpoint *ep)
{
	int n;

	if (epv == NULL)
		return epv;

	/* free everything except ep */
	for (n = 0; epv[n] != NULL; n++)
		if (epv[n] != ep)
			snaps_free_endpoint(&epv[n]);

	/* ensure ep is the first item */
	epv[0] = ep;
	epv[1] = NULL;

	/* resize */
	epv = reallocarray(epv, 2, sizeof(ep));
	if (epv == NULL)
		err(1, "%s: reallocarray", __func__);

	return epv;
}

/*
 * Make sure a directory exists. Creates directories recursively if needed but
 * only if all ancestors are owned by the superuser and none of them is writable
 * by either the group or others.
 *
 * If updmod is not nul, it will be updated to contain whether or not the mode
 * or owner are updated.
 *
 * If the path already exists but is not a directory return -1 and set errno to
 * ENOTDIR.
 *
 * Returns the number of created directories on success, or -1 on failure with
 * errno set to indicate the error.
 */
int
secureensuredir(const char *p, mode_t mode, gid_t gid, int *updmod)
{
	struct stat st;
	int created = 0, done, trusted, i;
	char path[PATH_MAX], *slash;
	mode_t cmode;

	/* The file access permission bits. */
	const mode_t pbits = S_ISUID | S_ISGID | S_ISVTX | S_IRWXU | S_IRWXG |
		S_IRWXO;

	/* Only permission bits should be passed. */
	if ((mode & ~pbits) != 0) {
		errno = EINVAL;
		return -1;
	}

	/* Requests for a writable "group" or "other" won't be serviced. */
	if ((mode & (S_IWGRP | S_IWOTH)) != 0) {
		errno = EINVAL;
		return -1;
	}

	/*
	 * First check if the existing part can be trusted.
	 */

	if (trustedpath(p, S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, gid, &trusted,
	    NULL) != 0)
		err(1, "%s: trustedpath", __func__);

	if (!trusted) {
		errno = EPERM;
		return -1;
	}

	/* Ensure an absolute path to work with. */
	path[0] = '\0';
	if (p[0] != '/') {
		if (getcwd(path, sizeof(path)) == NULL)
			err(1, "%s: getcwd", __func__);

		if (path[0] != '/') /* See getcwd(2) on Linux. */
			errx(1, "are you in a chroot and forgot to chdir(2)?");

		/* ensure termination with a '/' */
		i = strrchr(path, '\0') - path;
		if (path[i - 1] != '/') {
			if (i + 1 >= sizeof(path)) {
				errno = ENAMETOOLONG;
				return -1;
			}
			path[i++] = '/';
			path[i] = '\0';
		}
	}
	if (strlcat(path, p, sizeof(path)) >= sizeof(path)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	slash = path;
	for (;;) {
		slash += strspn(slash, "/");
		slash += strcspn(slash, "/");

		done = (*slash == '\0');
		*slash = '\0';

		if (stat(path, &st) == -1) {
			if (errno == ENOENT) {
				if (mkdir(path, mode) == -1)
					err(1, "mkdir: %s", path);
				created++;
			} else {
				/* errno is set by stat */
				return -1;
			}
		} else {
			/*
			 * The path exists. Check type and trust trustedpath to
			 * have checked ownership and permissions.
			 */

			if (!S_ISDIR(st.st_mode)) {
				errno = ENOTDIR;
				return -1;
			}
		}

		if (done)
			break;

		*slash = '/';
	}

	if (updmod != NULL)
		*updmod = 0;

	/*
	 * Ensure the requested permissions.
	 */

	if (stat(path, &st) == -1)
		err(1, "%s: stat", __func__);

	cmode = st.st_mode & pbits;

	if (cmode != mode) {
		if (chmod(path, mode) == -1)
			return -1;
		if (updmod != NULL)
			*updmod = 1;
	}

	/*
	 * Ensure the requested group owner.
	 */

	if (gid != -1) {
		if (st.st_gid != gid) {
			if (chown(path, -1, gid) == -1)
				return -1;
			if (updmod != NULL)
				*updmod = 1;
		}
	}

	return created;
}

/*
 * Find out if all existing components of a path are owned by the superuser and
 * are not writable by the group or others. The final component, if it exists,
 * must have a mode that is a subset of the given relax mode. Furthermore, if a
 * group id is provided, than the final component must be owned by this group.
 *
 * Supported relax bits: S_IRGRP, S_IXGRP, S_IROTH, S_IXOTH.
 *
 * Return 0 on success or -1 on failure with errno set. If the path can be
 * trusted than "trusted" is set to 1, otherwise to 0. If the path is trusted
 * and "exists" is not null, then it will be set to 1 if all components exist in
 * the file-system or 0 if some part does not exist.
 */
int
trustedpath(const char *p, mode_t relax, gid_t gid, int *trusted, int *ex)
{
	struct stat st;
	int eop, symlinks, exists;
	size_t i;
	ssize_t slen;
	mode_t mode;
	char symlink[PATH_MAX], path[PATH_MAX], *slash;

	if (p == NULL) {
		errno = EINVAL;
		return -1;
	}

	/* Check if only supported relax bits are passed. */

	if ((relax & (S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)) != relax) {
		errno = EINVAL;
		return -1;
	}

	if (trusted == NULL) {
		errno = EINVAL;
		return -1;
	}

	if (p[0] == '\0') {
		errno = ENOENT;
		return -1;
	}

	/* Special check the root. */

	if (lstat("/", &st) == -1)
		err(1, "%s: lstat", __func__);

	if (!S_ISDIR(st.st_mode)) {
		/* XXX can this ever happen? */
		errno = ENOTDIR;
		return -1;
	}

	*trusted = 0;

	if (st.st_uid != 0)
		return 0;
	if (st.st_mode & (S_IWGRP | S_IWOTH))
		return 0;

	exists = 1;

	/* Ensure an absolute path to work with. */
	path[0] = '\0';
	if (p[0] != '/') {
		if (getcwd(path, sizeof(path)) == NULL)
			err(1, "%s: getcwd", __func__);

		if (path[0] != '/') /* See getcwd(2) on Linux. */
			errx(1, "are you in a chroot and forgot to chdir(2)?");

		/* ensure termination with a '/' */
		i = strrchr(path, '\0') - path;
		if (path[i - 1] != '/') {
			if (i + 1 >= sizeof(path)) {
				errno = ENAMETOOLONG;
				return -1;
			}
			path[i++] = '/';
			path[i] = '\0';
		}
	}
	if (strlcat(path, p, sizeof(path)) >= sizeof(path)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	symlinks = 0;

	slash = path;

	for (;;) {
		/* Move to end of the next component. */
		slash += strspn(slash, "/");
		slash += strcspn(slash, "/");

		eop = (*slash == '\0');
		*slash = '\0';

		if (lstat(path, &st) == -1) {
			if (errno == ENOENT) {
				/*
				 * We made it to a non-existing component, we're
				 * good.
				 */
				exists = 0;
				break;
			} else {
				err(1, "%s: lstat \"%s\" of \"%s\"",
					__func__, path, p);
			}
		}

		if (st.st_uid != 0)
			return 0;
		if (st.st_mode & (S_IWGRP | S_IWOTH))
			return 0;

		if (S_ISLNK(st.st_mode)) {
			if (++symlinks > SYMLOOP_MAX) {
				errno = ELOOP;
				return -1;
			}

			slen = readlink(path, symlink, sizeof(symlink));
			if (slen == -1) {
				return -1;
			} else if (slen == 0) {
				errno = EINVAL;
				return -1;
			} else if (slen == sizeof(symlink)) {
				errno = ENAMETOOLONG;
				return -1;
			}

			symlink[slen] = '\0';

			/* If there is anything left, append it to symlink. */
			if (!eop) {
				if (symlink[slen - 1] != '/') {
					if (slen + 1 >= sizeof(symlink)) {
						errno = ENAMETOOLONG;
						return -1;
					}
					symlink[slen++] = '/';
					symlink[slen] = '\0';
				}
				if (strlcat(symlink, slash + 1, sizeof(symlink))
				    >= sizeof(symlink)) {
					errno = ENAMETOOLONG;
					return -1;
				}
			}

			/*
			 * If the symlink target is absolute, replace all
			 * preceding components with the target, else replace
			 * only the last component. Make sure slash keeps
			 * pointing to the last processed slash.
			 */

			if (symlink[0] == '/') {
				/* point to new symlink root */
				path[0] = '\0';
				slash = path;
			} else {
				/* strip the component after the last '/' */
				slash = strrchr(path, '/');
				*(slash + 1) = '\0';
			}

			if (strlcat(path, symlink, sizeof(path))
			    >= sizeof(path)) {
				errno = ENAMETOOLONG;
				return -1;
			}
			eop = 0;
		}

		/* If this was the last part, we're good. */
		if (eop)
			break;

		*slash = '/';
	}

	/*
	 * Do some stricter checks on the final component if it exists.
	 *
	 * Check if the permission bits of the final component are a subset of
	 * the given relax mode. If a group id is given, make sure these match
	 * as well.
	 */

	if (exists) {
		/*
		 * Get all file access permission bits except the owner bits and
		 * the saved-text bit. It is already verified that the owner is
		 * the superuser and the presence of the saved-text bit would
		 * only impose extra restrictions so it's safe to ignore.
		 */
		mode = st.st_mode & (S_ISUID | S_ISGID | S_IRWXG | S_IRWXO);

		if ((mode & ~relax) != 0)
			return 0;

		if (gid != -1)
			if (st.st_gid != gid)
				return 0;
	}

	*trusted = 1;
	if (ex != NULL)
		*ex = exists;

	return 0;
}

/*
 * Normalize path.
 *
 * Remove extraneous slashes, "." and ".." components, turn a relative path into
 * an absolute path and ensure termination with a "/". Note that apart from
 * calling getcwd on a relative path no checks are done within the file-system
 * if any of the components exist or are of a certain type.
 *
 * Return the normalized path on success, NULL on error and set errno. The
 * returned variable should be free(3)'d if not passed by the caller.
 *
 * Based on realpath(2) that ships with OpenBSD.
 */

/*	$OpenBSD: realpath.c,v 1.22 2017/12/24 01:50:50 millert Exp $ */
/*
 * Copyright (c) 2003 Constantin S. Svintsoff <kostik@iclub.nsu.ru>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The names of the authors may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Returns (resolved) on success, or (NULL) on failure, in which case the path
 * which caused trouble is left in (resolved).
 */
char *
normalize_path(const char *path, char *resolved, int withslash)
{
	const char *p;
	char *q;
	size_t left_len, resolved_len, next_token_len;
	int mem_allocated;
	char left[PATH_MAX], next_token[PATH_MAX];

	if (path == NULL) {
		errno = EINVAL;
		return (NULL);
	}

	if (path[0] == '\0') {
		errno = ENOENT;
		return (NULL);
	}

	if (resolved == NULL) {
		resolved = malloc(PATH_MAX);
		if (resolved == NULL)
			return (NULL);
		mem_allocated = 1;
	} else
		mem_allocated = 0;

	if (path[0] == '/') {
		resolved[0] = '/';
		resolved[1] = '\0';
		if (path[1] == '\0')
			return (resolved);
		resolved_len = 1;
		left_len = strlcpy(left, path + 1, sizeof(left));
	} else {
		if (getcwd(resolved, PATH_MAX) == NULL) {
			if (mem_allocated)
				free(resolved);
			else
				strlcpy(resolved, ".", PATH_MAX);
			return (NULL);
		}
		resolved_len = strlen(resolved);
		left_len = strlcpy(left, path, sizeof(left));
	}
	if (left_len >= sizeof(left)) {
		errno = ENAMETOOLONG;
		goto err;
	}

	/*
	 * Iterate over path components in `left'.
	 */
	while (left_len != 0) {
		/*
		 * Extract the next path component and adjust `left'
		 * and its length.
		 */
		p = strchr(left, '/');

		next_token_len = p ? (size_t) (p - left) : left_len;
		memcpy(next_token, left, next_token_len);
		next_token[next_token_len] = '\0';

		if (p != NULL) {
			left_len -= next_token_len + 1;
			memmove(left, p + 1, left_len + 1);
		} else {
			left[0] = '\0';
			left_len = 0;
		}

		if (resolved[resolved_len - 1] != '/') {
			if (resolved_len + 1 >= PATH_MAX) {
				errno = ENAMETOOLONG;
				goto err;
			}
			resolved[resolved_len++] = '/';
			resolved[resolved_len] = '\0';
		}
		if (next_token[0] == '\0')
			continue;
		else if (strcmp(next_token, ".") == 0)
			continue;
		else if (strcmp(next_token, "..") == 0) {
			/*
			 * Strip the last path component except when we have
			 * single "/"
			 */
			if (resolved_len > 1) {
				resolved[resolved_len - 1] = '\0';
				q = strrchr(resolved, '/') + 1;
				*q = '\0';
				resolved_len = q - resolved;
			}
			continue;
		}

		/*
		 * Append the next path component.
		 */
		resolved_len = strlcat(resolved, next_token, PATH_MAX);
		if (resolved_len >= PATH_MAX) {
			errno = ENAMETOOLONG;
			goto err;
		}
	}

	if (withslash) {
		/* Ensure termination with '/'. */
		if (resolved[resolved_len - 1] != '/') {
			if (resolved_len + 1 >= PATH_MAX) {
				errno = ENAMETOOLONG;
				goto err;
			}
			resolved[resolved_len++] = '/';
			resolved[resolved_len] = '\0';
		}
	} else {
		/*
		 * Remove trailing slash except when the resolved pathname
		 * is a single "/".
		 */
		if (resolved_len > 1 && resolved[resolved_len - 1] == '/')
			resolved[resolved_len - 1] = '\0';
	}

	return (resolved);

err:
	if (mem_allocated)
		free(resolved);
	return (NULL);
}

/*
 * Check if an endpoint with the given identifier exists in the endpoint vector.
 *
 * Return a pointer to the endpoint if found, NULL otherwise.
 */
struct endpoint *
snaps_find_endpoint(struct endpoint **epv, const char *id)
{
	while (epv && *epv) {
		if (strcmp(snaps_endpoint_id(*epv), id) == 0)
			return *epv;
		epv++;
	}

	return NULL;
}

/*
 * Return a unique identifier for this endpoint.
 */
char *
snaps_endpoint_id(struct endpoint *ep)
{
	return ep->path;
}

/*
 * Check if path is within root.
 *
 * If 'issubdir' is not nul, set it to 1 if path is a subdir of root, 0
 * otherwise.
 *
 * Return 1 if path equals or is within root, 0 otherwise.
 */
int
inroot(const char *root, const char *path, int *issubdir)
{
	char p1[PATH_MAX], p2[PATH_MAX];

	/* init */
	if (issubdir != NULL)
		*issubdir = 0;

	if (root == NULL || path == NULL)
		return 0;

	if (root[0] == '\0' || path[0] == '\0')
		return 0;

	if (normalize_path(root, p1, 1) == NULL)
		err(1, "%s: normalize_path: %s", __func__, root);

	if (normalize_path(path, p2, 1) == NULL)
		err(1, "%s: normalize_path: %s", __func__, path);

	if (strncmp(p1, p2, strlen(p1)) == 0) {
		if (issubdir != NULL && strlen(p2) > strlen(p1))
			*issubdir = 1;
		return 1;
	}

	return 0;
}

/*
 * Turns the string path into one pathname component by replacing each slash
 * with an underscore.
 *
 * Further, it collapses multiple slashes into one and removes a trailing slash, if any.
 *
 * 1. make sure the length is at least 1
 * 2. make sure the length does not exceed NAME_MAX
 * 3. make sure it's not ".." or ".", it's ok to start with one of those
 * 4. collapse multiple slashes into one.
 * 5. remove trailing slash if any, unless only one slash is there
 * 6. replace any slash with an underscore
 *
 * Return 0 on success, -1 on error.
 */
int
normalize_pathcomp(char *path)
{
	size_t len, i, j;

	if (path == NULL || path[0] == '\0')
		return -1;

	len = strlen(path);
	if (len > NAME_MAX)
		return -1;

	if (strcmp(path, ".") == 0)
		return -1;

	if (strcmp(path, "..") == 0)
		return -1;

	/* replace multiple slashes with one underscore */
	for (i = 0, j = 0; i < len; i++, j++) {
		if (path[i] == '/') {
			i += strspn(&path[i + 1], "/");

			/* i now points to the last slash */

			/*
			 * If this is the last character, and the result is not
			 * only a single slash, skip it.
			 */
			if (j > 0 && i == (len - 1)) {
				path[i] = '\0';
			} else {
				/* replace it with an underscore */
				path[i] = '_';
			}
		}

		path[j] = path[i];
	}

	path[j] = '\0';

	return 0;
}

/*
 * Check whether a path is absolute or not.
 *
 * Ignore the fact that POSIX dictates that a path starting with exactly two
 * slashes is not a valid path.
 *
 * Return 1 if the path is absolute, or 0 if it is not.
 */
int
isabsolutepath(const char *path)
{
	if (path != NULL && path[0] == '/')
		return 1;

	return 0;
}

/*
 * Format a duration in seconds into a human readable string, using seconds,
 * minutes, hours, days.
 *
 * Returns a static buffer.
 */
char *
humanduration(time_t t)
{
	static char result[20] = "";

	if (t == 1) {
		snprintf(result, sizeof(result), "%lld second",	t);
	} else if (t < 120) {
		snprintf(result, sizeof(result), "%lld seconds",	t);
	} else if (t < (3600 * 2)) {
		snprintf(result, sizeof(result), "%lld minutes",	t / 60);
	} else if (t < (86400 * 2)) {
		snprintf(result, sizeof(result), "%lld hours",	t / 3600);
	} else if (t < (86400 * 7 * 2)) {
		snprintf(result, sizeof(result), "%lld days",	t / 86400);
	} else {
		snprintf(result, sizeof(result), "%lld weeks",	t / (86400 * 7));
	}

	return result;
}

/*
 * Return a directory name, given an interval name and number.
 *
 * Return a string that should be free(3)d on success, or NULL on error with
 * errno set.
 */
char *
snapdirstr(const char *ivalname, int number)
{
	char *p;

	if (asprintf(&p, "%s.%d", ivalname, number) <= 0)
		err(1, "%s", __func__);

	return p;
}

/*
 * Get the sync dir for a given endpoint.
 *
 * Return the directory name on success, or NULL on error with errno set.
 */
char *
getsyncdir(void)
{
	return snapdirstr(SYNCDIR, 1);
}

/*
 * Wait for a specific process.
 *
 * Return the exit status if exited, return 128 if child exited because of a
 * signal or return -1 if interupted or an error occurred.
 */
int
reapproc(pid_t pid)
{
	int status;

	if (pid <= 0)
		return -1;

again:
	if (waitpid(pid, &status, 0) == -1) {
		if (errno == EINTR)
			goto again;
		return -1;
	}

	if (!WIFEXITED(status)) {
		if (WIFSIGNALED(status)) {
			warnx("%d terminated by signal \"%s\"",
				pid, strsignal(WTERMSIG(status)));
			return 128;
		}

		errx(1, "unexpected status");
	}

	return WEXITSTATUS(status);
}

/*
 * Return an informal identification string for the given endpoint.
 */
char *
getepid(const struct endpoint *ep)
{
	static char hostid[HOST_NAME_MAX + PATH_MAX];

	if (snprintf(hostid, sizeof(hostid), "%s:%s", ep->hostname,
	    ep->rpath) <= 0) {
		warnx("can't determine hostid");
		hostid[0] = '\0';
	}

	return hostid;
}

/*
 * Check if the given file descriptor is open.
 *
 * Return 1 if fd is open, 0 if fd is closed, -1 on failure with errno set.
 */
int
isopenfd(int fd)
{
	if (fcntl(fd, F_GETFL) == -1) {
		if (errno == EBADF)
			return 0;
		else
			return -1;
	}

	return 1;
}

/*
 * Write a command.
 *
 * Return 0 on success, -1 on failure with errno set.
 */
int
writecmd(int commfd, int cmd)
{
	int i;

	if ((i = write(commfd, &cmd, sizeof(cmd))) == -1)
		return -1; /* errno set by write(2) */

	if (i != sizeof(cmd))
		errx(1, "%s: %d bytes written instead of %lu",
			__func__, i, sizeof(cmd));

	return 0;
}

/*
 * Read a command.
 *
 * Returns 0 on success or -1 on failure with errno set. On success result will
 * be set to the received command or to CMDCLOSED if an EOF is received. On
 * error result will be undefined.
 */
int
readcmd(int commfd, int *result)
{
	int i, cmd;

	if ((i = read(commfd, &cmd, sizeof(cmd))) == -1)
		return -1; /* errno set by read(2) */

	/* EOF */
	if (i == 0) {
		*result = CMDCLOSED;
		return 0;
	}

	if (i != sizeof(cmd)) {
		warnx("%s: %d bytes read instead of %lu", __func__, i,
			sizeof(cmd));
		errno = EINVAL;
		return -1;
	}

	*result = cmd;

	return 0;
}

/*
 * Set directory permissions of a snapshot on disk.
 *
 * Return 0 on success, or -1 on error with errno set.
 */
int
setsnapshotmode(struct snapshot *s, mode_t mode)
{
	int fd, r;

	r = 0;

	if ((fd = opensnapshot(s)) == -1)
		return -1;

	r = fchmod(fd, mode);
	if (close(fd) == -1)
		return -1;

	return r;
}

/*
 * Set the creation time of a snapshot.
 */
void
setsnapshottime(struct snapshot *s, time_t t)
{
	char *dst;
	struct timeval times[2];

	dst = snapshotname(s);

	times[0].tv_sec = t;
	times[0].tv_usec = 0;
	times[1].tv_sec = t;
	times[1].tv_usec = 0;
	if (utimes(dst, times) == -1)
		err(1, "%s: utimes", __func__);
	free(dst);
	dst = NULL;
}

/*
 * Return the time a snapshot was made.
 *
 * Looks at the modification time of the directory which is expected to be reset
 * after a successful sync.
 *
 * A positive time_t >0 on success or -1 on error with errno set.
 */
time_t
snapshottime(struct snapshot *s)
{
	struct stat st;
	int fd;

	if ((fd = opensnapshot(s)) == -1) /* errno is set */
		return -1;

	/* mod-time is set after ripplein */
	if (fstat(fd, &st) == -1)
		err(1, "%s: fstat", __func__);

	if (close(fd) == -1)
		err(1, "%s: close", __func__);

	return st.st_mtim.tv_sec;
}

/*
 * Convert a snapshot structure to a directory name. The caller should free(3)
 * the result.
 *
 * Return a string on success, or NULL on error with errno set.
 */
char *
snapshotname(struct snapshot *s)
{
	if (s == NULL) {
		errno = EINVAL;
		return NULL;
	}

	return snapdirstr(s->name, s->number);
}

/*
 * Open a file-descriptor to a snapshot.
 *
 * Return the open descriptor on success, or -1 on error with errno set.
 */
int
opensnapshot(const struct snapshot *s)
{
	char *dir;
	int fd;

	if (s == NULL || s->ep == NULL) {
		errno = EINVAL;
		return -1;
	}

	dir = snapdirstr(s->name, s->number);
	fd = openat(s->ep->pathfd, dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC |
		O_NOFOLLOW);
	free(dir);
	dir = NULL;

	return fd;
}

/*
 * Find the newest snapshot on disk within an interval.
 *
 * Update and return s if found or return NULL if none was found and leave s
 * untouched.
 */
static struct snapshot *
newestsnapshotininterval(struct endpoint *ep, struct snapinterval *si, struct snapshot *s)
{
	struct snapshot sn;
	time_t age;

	for (int i = 1; i <= si->count; i++) {
		if (setsnapshot(ep, si->name, i, &sn) == -1)
			err(1, "%s: setsnapshot", __func__);

		if (snapshotttl(&sn, 0, &age) || age) {
			s->ep = sn.ep;
			s->name = sn.name;
			s->number = sn.number;

			return s;
		}
	}

	return NULL;
}

/*
 * Find the newest snapshot that exists on disk.
 *
 * Update and return s if found or return NULL if none was found and leave s
 * untouched.
 */
struct snapshot *
newestsnapshot(struct endpoint *ep, struct snapshot *s)
{
	struct snapinterval **sv;

	sv = ep->snapshots;
	if (sv == NULL)
		return NULL;

	while (*sv) {
		if (newestsnapshotininterval(ep, *sv, s) != NULL)
			return s;
		sv++;
	}

	return NULL;
}

/*
 * Set a snapshot.
 *
 * Return 0 on success, -1 on error with errno set.
 */
int
setsnapshot(struct endpoint *ep, char *name, int number, struct snapshot *s)
{
	if (s == NULL) {
		errno = EINVAL;
		return -1;
	}

	if (number < 1 || number == INT_MAX) {
		errno = EINVAL;
		return -1;
	}

	memset(s, '\0', sizeof(*s));
	s->ep = ep;
	s->name = name;
	s->number = number;

	return 0;
}

/*
 * Return the snapinterval this snapshot belongs to.
 *
 * Return the snapinterval on success or NULL on error with errno set.
 */
static struct snapinterval *
snapshotinterval(const struct snapshot *s)
{
	struct snapinterval **siv;

	if (s == NULL || s->ep == NULL || s->ep->snapshots == NULL) {
		errno = EINVAL;
		return NULL;
	}

	for (siv = s->ep->snapshots; *siv; siv++)
		if (strcmp((*siv)->name, s->name) == 0)
			break;

	if (*siv == NULL)
		errno = EINVAL;

	return *siv;
}

/*
 * Check if a snapshot exists and how many seconds remain before it is expired
 * according to the position (number) and the lifetime of the interval it is in.
 *
 * If age is not null, then the current age of the snapshot is set. If 0 is
 * returned and age is 0, then the snapshot does not exist (or now and the
 * snapshot creation time collide).
 *
 * Return the number of seconds before expiry on success or -1 on error with
 * errno set.
 */
time_t
snapshotttl(struct snapshot *s, time_t now, time_t *age)
{
	struct snapinterval *si;
	time_t born, a;
	int i;

	if ((si = snapshotinterval(s)) == NULL)
		err(1, "%s: snapshotinterval", __func__);

	born = snapshottime(s);
	if (born == -1 && errno == ENOENT) {
		if (age)
			*age = 0;
		return 0;
	}

	if (born == -1) /* errno is set */
		return -1;

	a = now - born;

	if (age)
		*age = a;

	/* Relative age to position in interval. */
	for (i = s->number - 1; i > 0; i--)
		a -= si->lifetime;

	if (a < si->lifetime)
		return si->lifetime - a;

	return 0;
}

/*
 * Prepare an execution environment: pledge, change user/group etc.
 *
 * Return 0 on success, -1 on failure with errno set.
 */
int
privdrop(uid_t uid, gid_t gid)
{
	/*
	 * We need to be the superuser in order to clear supplementary group ids
	 * and all three user and group ids.
	 */

	if (geteuid() != 0) {
		errno = EPERM;
		warnx("must run as the superuser");
		return -1;
	}

	if (uid == 0 || gid == 0) {
		errno = EINVAL;
		warnx("dropping privileges to the superuser is not meaningful");
		return -1;
	}

	/* set supplementary groups to the primary group only */
	if (setgroups(1, &gid) == -1) {
		warn("%s: setgroups", __func__);
		return -1;
	}

	/* change real and effective group-id */
	if (setgid(gid) == -1) {
		warn("%s: setgid", __func__);
		return -1;
	}

	/* change real and effective user-id */
	if (setuid(uid) == -1) {
		warn("%s: setuid", __func__);
		return -1;
	}

	if (pledge("stdio exec", NULL) == -1) {
		warn("%s: pledge", __func__);
		return -1;
	}

	return 0;
}

/*
 * Execute a custom binary after the syncer has terminated.
 */
void
postexec(const struct endpoint *ep)
{
	struct passwd *pwd;
	char *cp, *cp2;
	char **binargv, **binenvp;
	int cmd;

	if (pledge("stdio getpw id rpath proc exec", NULL) == -1)
		err(1, "%s: pledge", __func__);

	/* expect stdout, stderr, pathfd and the communication channel only */
	if (isopenfd(STDOUT_FILENO) != 1)
		errx(1, "expected stdout to be open");
	if (isopenfd(STDERR_FILENO) != 1)
		errx(1, "expected stderr to be open");
	if (isopenfd(ep->pathfd) != 1)
		errx(1, "expected pathfd to be open");
	if (isopenfd(ep->poxfd) != 1)
		errx(1, "expected communication channel to be open");
	if (getdtablecount() != 4)
		errx(1, "fd leak: %d", getdtablecount());

	/* Wait until we're ready to start. */
	if (readcmd(ep->poxfd, &cmd) == -1)
		err(1, "%s: %s read error", __func__, getepid(ep));

	if (cmd != CMDCUST && cmd != CMDSTOP)
		errx(1, "%s: %s unexpected command: %d", __func__, getepid(ep),
			cmd);

	if (cmd == CMDSTOP)
		exit(0);

	/* Check if an executable is configured. */
	if (ep->postexec == NULL)
		errx(1, "%s: no postexec configured", getepid(ep));

	/* change working dir to syncdir */
	cp = getsyncdir();
	if (asprintf(&cp2, "%s/%s", ep->path, cp) <= 0)
		errx(1, "%s: asprintf", __func__);
	free(cp);
	cp = NULL;

	if (chdir(cp2) == -1)
		err(1, "%s: chdir %s", __func__, cp2);
	free(cp2);
	cp2 = NULL;

	/* Try to get some user info for building the environment later on. */
	pwd = getpwuid(ep->uid);

	/* drop privileges */

	if (privdrop(ep->uid, ep->gid) == -1)
		errx(1, "%s: privdrop", __func__);
	if (verbose > 2)
		fprintf(stdout, "postexec[%d]: running as %d:%d\n",
			getpid(),
			getuid(),
			getgid());

	/* Expect the exit status of the syncer as the next command. */
	if (readcmd(ep->poxfd, &cmd) == -1)
		err(1, "%s: %s read error", __func__, getepid(ep));

	/* Close communication channel. */
	if (close(ep->poxfd) == -1)
		err(1, "%s: closing communication channel", getepid(ep));

	/*
	 * Build the argument vector.
	 */

	binargv = NULL;
	binargv = addstr(binargv, basename(ep->postexec));

	if (asprintf(&cp, "%d", cmd) <= 0)
		err(1, "%s: asprintf", __func__);
	binargv = addstr(binargv, cp);
	free(cp);
	cp = NULL;

	/*
	 * Build a minimal environment vector.
	 */

	binenvp = NULL;
	binenvp = addstr(binenvp, "PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/X11R6/bin:/usr/local/bin:/usr/local/sbin");

	if (pwd) {
		if (pwd->pw_name != NULL) {
			if (asprintf(&cp, "LOGNAME=%s", pwd->pw_name) <= 0)
				err(1, "%s: asprintf", __func__);
			binenvp = addstr(binenvp, cp);
			free(cp);
			cp = NULL;

			if (asprintf(&cp, "USER=%s", pwd->pw_name) <= 0)
				err(1, "%s: asprintf", __func__);
			binenvp = addstr(binenvp, cp);
			free(cp);
			cp = NULL;
		}

		if (pwd->pw_dir != NULL) {
			if (asprintf(&cp, "HOME=%s", pwd->pw_dir) <= 0)
				err(1, "%s: asprintf", __func__);
			binenvp = addstr(binenvp, cp);
			free(cp);
			cp = NULL;
		}

		if (pwd->pw_shell != NULL) {
			if (asprintf(&cp, "SHELL=%s", pwd->pw_shell) <= 0)
				err(1, "%s: asprintf", __func__);
			binenvp = addstr(binenvp, cp);
			free(cp);
			cp = NULL;
		}
	}

	if (verbose > 1)
		printstrv(binargv);

	if (execvpe(ep->postexec, binargv, binenvp) == -1)
		err(1, "%s: execvpe %s", __func__, getepid(ep));
}
