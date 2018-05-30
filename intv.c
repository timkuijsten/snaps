#include "intv.h"

/*
 * Add an integer to an integer vector (or create a new vector if intv is NULL).
 *
 * Return a pointer to a possibly relocated intv, or exit on error.
 */
int **
addint(int **intv, int num)
{
	int i;

	/* find first empty slot */
	for (i = 0; intv != NULL && intv[i] != NULL; i++)
		;

	/* ensure enough space */
	intv = reallocarray(intv, i + 2, sizeof(int *));
	if (intv == NULL)
		err(1, "%s: reallocarray", __func__);

	intv[i] = malloc(sizeof(num));
	if (intv[i] == NULL)
		err(1, "%s: malloc", __func__);

	*intv[i] = num;
	intv[i+1] = NULL;

	return intv;
}

/* Clear and free a number vector. */
void
clrintv(int ***intv)
{
	int i;

	if (*intv == NULL)
		return;

	/* free up individual numbers first. */
	for (i = 0; (*intv)[i] != NULL; i++) {
		free((*intv)[i]);
		(*intv)[i] = NULL;
	}

	free(*intv);
	*intv = NULL;
}

/*
 * Duplicate a number vector.
 *
 * Return the newly allocated vector on success, NULL if intv is NULL and exit
 * on error.
 */
int **
dupintv(int **intv)
{
	int i;
	int **r = NULL;

	if (intv == NULL)
		return NULL;

	for (i = 0; intv[i]; i++)
		r = addint(r, *intv[i]);

	return r;
}

/* Print a null terminated number vector on the designated stream. */
void
fprintintv(FILE *fp, int **intv)
{
	while (intv && *intv)
		fprintf(fp, " %d", **intv++);
	fprintf(fp, "\n");
}

/* Print a null terminated number vector. */
void
printintv(int **intv)
{
	fprintintv(stdout, intv);
}
