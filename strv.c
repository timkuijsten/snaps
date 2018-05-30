#include "strv.h"

/*
 * Make a copy of a string and append it to a string vector (or create a new
 * vector if strv is NULL).
 *
 * Return a pointer to a possibly relocated strv, or exit on error.
 */
char **
addstr(char **strv, const char *str)
{
	int i;

	if (str == NULL)
		return strv;

	/* find first empty slot */
	for (i = 0; strv != NULL && strv[i] != NULL; i++)
		;

	/* ensure enough space */
	strv = reallocarray(strv, i + 2, sizeof(str));
	if (strv == NULL)
		err(1, "%s: reallocarray", __func__);

	strv[i] = strdup(str);
	if (strv[i] == NULL)
		err(1, "%s: strdup", __func__);

	/* terminate vector */
	strv[i+1] = NULL;

	return strv;
}

/* Clear and free a complete string vector. */
void
clrstrv(char ***strv)
{
	int i;

	if (*strv == NULL)
		return;

	/* free up individual strings first. */
	for (i = 0; (*strv)[i] != NULL; i++) {
		free((*strv)[i]);
		(*strv)[i] = NULL;
	}

	free(*strv);
	*strv = NULL;
}

/*
 * Duplicate a string vector.
 *
 * Return the newly allocated vector on success, NULL if strv is NULL and exit
 * on error.
 */
char **
dupstrv(char **strv)
{
	int i;
	char **r = NULL;

	if (strv == NULL)
		return NULL;

	/* copy up till the first null value */
	for (i = 0; strv[i] != NULL; i++)
		r = addstr(r, strv[i]);

	return r;
}

/* Print a null terminated string vector on the designated stream. */
void
fprintstrv(FILE *fp, char **strv)
{
	while (strv && *strv)
		fprintf(fp, " %s", *strv++);
	fprintf(fp, "\n");
}

/* Print a null terminated string vector. */
void
printstrv(char **strv)
{
	fprintstrv(stdout, strv);
}
