%{
#include "scfg.h"

int yydebug = 0;

typedef struct {
	union {
		struct scfgentry *scfge;
		char *str;
		char **strv;
	};
	int lineno;
	int colno;
} YYSTYPE;

extern int verbose;

/* Should be assigned externally. */
int yyd = -1;

/* Use a stream internally for easy look-ahead using ungetc. */
static FILE *yyfp = NULL;
static int curcol;
static int curline;

/*
 * Global config vector that holds entries.
 */
static struct scfgentry *cfgroot = NULL;

static int allocated = 0;

static void scfg_freeblock(struct scfgentry ***);
static void scfg_freeentry(struct scfgentry **);
static void scfg_settermv(struct scfgentry *, char **);
static struct scfgentry *scfg_allocentry(void);
static struct scfgentry **scfg_addentry(struct scfgentry **, struct scfgentry *);

static char *getstr(FILE *);
static char *appendc(char *, char, size_t *);

void
yyerror(char *msg)
{
        warnx("%s at line %d, column %d", msg, curline, curcol);
}

%}

%start grammar

%token <str> STRING
%token OBRACE CBRACE EOS

%type <strv> termv
%type <scfge> grammar block

%%

grammar:	/* new */ {
		if (verbose > 2)
			warnx("GRAMMAR:");

		if (cfgroot == NULL) {
			cfgroot = scfg_allocentry();
			$$ = cfgroot;
		} else {
			$$ = scfg_allocentry();
		}
	}
	| grammar termv block EOS {
		if (verbose > 2)
			warnx("grammar: grammar termv block EOS %p %p %p", $1, $2, $3);

		struct scfgentry *scfge;

		scfge = scfg_allocentry();

		if ($2 != NULL)
			scfg_settermv(scfge, $2);

		if ($3 != NULL)
			scfge->block = scfg_addentry(scfge->block, $3);

		/* Reset to null if it was an empty line. */
		if ($2 == NULL && $3 == NULL)
			scfg_freeentry(&scfge);
		else /* add to existing grammar block */
			$1->block = scfg_addentry($1->block, scfge);

		/* Keep $1 */
	}
	;
termv:	/* optional */ {
		if (verbose > 2)
			warnx("termv:");

		$$ = NULL;
	}
	| termv STRING {
		if (verbose > 2)
			warnx("termv: termv STRING %p %s", $1, $2);

		$$ = addstr($1, $2);
	}
	;
block:	/* optional */ {
		if (verbose > 2)
			warnx("block:");

		$$ = NULL;
	}
	| OBRACE grammar CBRACE {
		if (verbose > 2)
			warnx("BLOCK: OBRACE grammar CBRACE { %p }", $2);

		if ($2->block == NULL && $2->termv == NULL)
			scfg_freeentry(&$2);

		/* return nested entry or null */
		$$ = $2;
	}
	;

%%
int
yylex(void)
{
	extern int error;
	static int prevtoken = 0, nexttoken = 0;

	if (yyd == -1) {
		yyerror("no descriptor open");
		return -1;
	}

	/* Init on first call. */
	if (yyfp == NULL) {
		curcol = 0;
		curline = 1;
		if ((yyfp = fdopen(yyd, "r")) == NULL)
				err(1, "%s: fdopen", __func__);
	}

	/*
	 * Maybe the next token was cached because of an implicit
	 * end-of-statement.
	 */

	if (nexttoken > 0) {
		prevtoken = nexttoken;
		nexttoken = 0;
		return prevtoken;
	}

	/* Expect yylval be either NULL or initialized in a previous call. */

	free(yylval.str);

	while ((yylval.str = getstr(yyfp)) != NULL) {
		yylval.lineno = curline;
		yylval.colno = curcol;

		if (strcmp(yylval.str, "\n") == 0) {
			prevtoken = EOS;
			return EOS;
		} else if (strcmp(yylval.str, ";") == 0) {
			prevtoken = EOS;
			return EOS;
		} else if (strcmp(yylval.str, "{") == 0) {
			prevtoken = OBRACE;
			return OBRACE;
		} else if (strcmp(yylval.str, "}") == 0) {
			/*
			 * Return end-of-statement if not set explicitly after
			 * the last entry in a block.
			 */
			if (prevtoken != EOS) {
				nexttoken = CBRACE;
				prevtoken = EOS;
				return EOS;
			}
			prevtoken = CBRACE;
			return CBRACE;
		} else {
			prevtoken = STRING;
			return STRING;
		}
	}

	if (ferror(yyfp) != 0)
		err(1, "yyfp error");

	if (feof(yyfp) == 0)
		errx(1, "yyfp still open");

	/* Cleanup and signal the end. */
	yyfp = NULL;
	return 0;
}

/*
 * Parse input for strings.
 *
 * A string is a concatenation of non-null and non-control characters. Strings
 * are expected to be separated by blanks, newlines or ';'. A string may
 * only contain spaces if it is enclosed in double quotes or if every space is
 * escaped using a '\' character.
 *
 * The following special characters outside a string are returned as null-
 * terminated character strings:
 * 	'{'
 * 	'}'
 * 	'\n'
 * 	';'
 *
 * A '#' outside of a string ignores all characters up to the first newline.
 *
 * Any control characters or '\0' outside of a string, except for '\t' and '\n'
 * are considered illegal.
 *
 * Return a pointer to a new string on success or NULL on end-of-file or error.
 */
static char *
getstr(FILE *stream)
{
	enum states { S, STR, QSTR, ESCQ, ESCS, COMMENT };
	size_t len;
	int c, state;
	char *r;

	state = S;
	r = NULL;
	len = 0;
	while ((c = fgetc(stream)) != EOF) {

		/* Track position in file for debugging. */

		if (c == '\n') {
			curline++;
			curcol = 0;
		}

		curcol++;

		switch (state) {
		case S:
			if (isblank(c)) {
				/* Swallow any preceding blanks. */

			} else if (c == ';' || c == '\n' || c == '{'
					|| c == '}') {
				/* These characters are strings by themselves. */

				r = appendc(r, c, &len);
				goto end;

			} else if (c == '\\') {
				state = ESCS;

			} else if (c == '"') {
				state = QSTR;

			} else if (c == '#') {
				state = COMMENT;

			} else if (c != '\0' && !iscntrl(c)) {
				r = appendc(r, c, &len);
				state = STR;

			} else {
				warnx("unexpected char %d at %d,%d", c, curline, curcol);
				goto err;

			}
			break;
		case STR:
			if (isblank(c)) {
				/* End of string. */
				goto end;

			} else if (c == ';' || c == '\n' || c == '{'
					|| c == '}') {
				/*
				 * End of string. Finish this string and leave
				 * the newline or curly brace for the next run
				 * because these are special strings by
				 * themselves.
				 */

				ungetc(c, stream);

				/* don't count the characater twice */

				if (c == '\n')
					curline--;
				else
					curcol--;

				goto end;

			} else if (c == '\\') {
				state = ESCS;

			} else if (c != '\0' && !iscntrl(c)) {
				r = appendc(r, c, &len);

			} else {
				warnx("unexpected char %d at %d,%d", c, curline, curcol);
				goto err;

			}
			break;
		case QSTR:
			if (c == '\\') {
				state = ESCQ;

			} else if (c == '"') {
				goto end;

			} else if (c != '\0' && !iscntrl(c)) {
				r = appendc(r, c, &len);

			} else {
				warnx("unexpected char %d at %d,%d", c, curline, curcol);
				goto err;

			}
			break;
		case ESCS:
			if (c == '\0' || iscntrl(c)) {
				warnx("unexpected char %d at %d,%d", c, curline, curcol);
				goto err;
			}

			r = appendc(r, c, &len);
			state = STR;
			break;
		case ESCQ:
			if (c == '\0' || iscntrl(c)) {
				warnx("unexpected char %d at %d,%d", c, curline, curcol);
				goto err;
			}

			r = appendc(r, c, &len);
			state = QSTR;
			break;
		case COMMENT:
			if (c == '\t') {
				/* swallow the tab control-character */
			} else if (c == '\n') {
				/*
				 * End of comment, a newline is a string by
				 * itself.
				 */

				r = appendc(r, c, &len);
				goto end;
			} else if (c == '\0' || iscntrl(c)) {
				warnx("unexpected char %d at %d,%d", c, curline, curcol);
				goto err;
			}

			break;
		}
	}

err:
	free(r);
	r = NULL;
end:
	if (verbose > 3)
		warnx("returning %lu \"%s\"", r == NULL ? 0 : strlen(r), r == NULL ? "NULL" : r);
	return r;
}

/*
 * Append a character to a dynamically allocated string.
 *
 * Return a pointer to the possibly relocated string on success, or exit on
 * error.
 */
static char *
appendc(char *str, char c, size_t *len)
{
		/* account for terminating null byte */
		str = realloc(str, *len + 2);
		if (str == NULL)
			err(1, "%s: realloc", __func__);

		str[*len] = c;
		str[*len + 1] = 0;
		(*len)++;

		return str;
}

/*
 * Recursive count off all configuration entries, inclusive. Each non-empty
 * block counts for an extra entry.
 */
static size_t
count(struct scfgentry *scfge)
{
	struct scfgentry **blockp;
	size_t i;

	i = 0;

	if (scfge == NULL)
		return i;

	i++;

	if (scfge->block == NULL)
		return i;

	for (blockp = scfge->block; blockp && *blockp; blockp++)
		i += count(*blockp);

	return i;
}

/*
 * Recursive count off all configuration entries, inclusive. Each non-empty
 * block counts for an extra entry including one for the root node.
 */
size_t
scfg_count(void)
{
	if (cfgroot == NULL)
		return 0;

	return count(cfgroot);
}

/*
 * Recursively print all terms of all configuration entries.
 */
static void
printr(struct scfgentry *scfge, int depth)
{
	struct scfgentry **blockp;

	if (scfge == NULL)
		return;

	fprintf(stdout, "%d: ", depth);
	printstrv(scfge->termv);

	if (scfge->block == NULL)
		return;

	for (blockp = scfge->block; blockp && *blockp; blockp++)
		printr(*blockp, depth + 1);
}

/*
 * Recursively print all terms of all configuration entries.
 */
void
scfg_printr(void)
{
	if (cfgroot == NULL)
		return;

	printr(cfgroot, 0);
}

/*
 * Find a scfgentry by the first term.
 *
 * Returns scfgentry if found, NULL if not.
 */
static struct scfgentry *
scfg_getbyfirstterm(struct scfgentry *scfge, const char *key)
{
	struct scfgentry **blockp, *found;

	if (scfge == NULL)
		return NULL;

	if (key == NULL)
		return NULL;

	if (scfge->termv && scfge->termv[0] &&
	    (strcmp(key, scfge->termv[0]) == 0))
		return scfge;

	for (blockp = scfge->block; blockp && *blockp; blockp++) {
		found = scfg_getbyfirstterm(*blockp, key);
		if (found)
			return found;
	}

	return NULL;
}

/*
 * Get a term by index, starting at zero.
 *
 * Returns the term on index i if it exists or NULL if not.
 */
char *
scfg_termn(struct scfgentry *scfge, int i)
{
	int j;

	if (scfge == NULL || scfge->termv == NULL)
		return NULL;

	for (j = 0; j < i && scfge->termv[j]; j++)
		;

	return scfge->termv[j];
}

/*
 * Get the first term of a term vector.
 *
 * Returns the term on index 0 if it exists or NULL if not.
 */
char *
scfg_getkey(struct scfgentry *scfge)
{
	return scfg_termn(scfge, 0);
}

/*
 * Get the second term of a term vector.
 *
 * Returns the term on index 1 if it exists or NULL if not.
 */
char *
scfg_getval(struct scfgentry *scfge)
{
	return scfg_termn(scfge, 1);
}

/*
 * Get everything after the first term of a term vector.
 *
 * Returns a vector starting at the second if it exists or NULL if not.
 */
char **
scfg_getmval(struct scfgentry *scfge)
{
	if (scfge && scfge->termv && scfge->termv[1])
		return &scfge->termv[1];

	return NULL;
}

/*
 * Find a scfgentry by the first term.
 *
 * Returns scfgentry if found, NULL if not.
 */
struct scfgentry *
scfg_getbykey(const char *key)
{
	return scfg_getbyfirstterm(cfgroot, key);
}

/* Free block. */
static void
scfg_freeblock(struct scfgentry ***block)
{
	struct scfgentry **scfge;

	if (*block == NULL)
		return;

	for (scfge = *block; *scfge; scfge++)
		scfg_freeentry(scfge);

	free(*block);
	*block = NULL;
}

static void
scfg_freeentry(struct scfgentry **scfge)
{
	if (*scfge == NULL)
		return;

	clrstrv(&(*scfge)->termv);

	scfg_freeblock(&(*scfge)->block);

	allocated--;

	if (verbose > 2 )
		warnx("%s: deallocated %p %d", __func__, *scfge, allocated);

	free(*scfge);
	*scfge = NULL;
}

/*
 * Remove the complete config from memory and check for leaks.
 *
 * Return 0 on success, -1 if an error occurred.
 */
int
scfg_clear(void)
{
	if (verbose > 2 )
		warnx("%s", __func__);

	if (cfgroot == NULL) {
		if (allocated > 0) {
			warnx("no cfgroot while %d entries allocated",
				allocated);
			return -1;
		}
		return 0;
	}

	scfg_freeentry(&cfgroot);

	if (allocated > 0) {
		warnx("memleak: still %d entries allocated", allocated);
		return -1;
	}

	return 0;
}

/* Set a termv. */
static void
scfg_settermv(struct scfgentry *scfge, char **termv)
{
	clrstrv(&scfge->termv);
	scfge->termv = termv;
}

/* Allocate a new scfc entry . */
static struct scfgentry *
scfg_allocentry(void)
{
	struct scfgentry *scfge;

	if ((scfge = malloc(sizeof(*scfge))) == NULL)
		err(1, "%s: malloc", __func__);
	allocated++;
	if (verbose > 2 )
		warnx("%s:  allocated %p %d", __func__, scfge, allocated);

	scfge->termv = NULL;
	scfge->block = NULL;

	return scfge;
}

/* Add a entry to a block. */
static struct scfgentry **
scfg_addentry(struct scfgentry **block, struct scfgentry *scfge)
{
	size_t n;

	/* find first empty slot, if initialized */
	for (n = 0; block && block[n]; n++)
		;

	/* ensure enough space */
	block = reallocarray(block, n + 2, sizeof(*block));
	if (block == NULL)
		err(1, "%s: reallocarray", __func__);

	block[n] = scfge;

	/* terminate vector */
	block[n+1] = NULL;

	return block;
}

/*
 * Iterate recursively over all entries with at least one term, optionally
 * filtered. Return 1 if all entries are processed, 0 if stopped by the callback
 * or -1 on error.
 *
 * If the iterator is not invoked because block is non-null but empty, return 1.
 */
static int
foreach(struct scfgentry *ce, const struct scfgiteropts *opts,
	int depth, int (*cb)(struct scfgentry *))
{
	struct scfgentry **blockp;
	int r;

	if (ce == NULL)
		return 1;

	if (ce->termv != NULL && ce->termv[0] != NULL) {
		if (opts) {
			if (opts->maxdepth)
				if (depth > opts->maxdepth)
					goto done;

			if (opts->mindepth)
				if (depth < opts->mindepth)
					goto descend;

			if (opts->key)
				if (strcmp(ce->termv[0], opts->key) != 0)
					goto descend;
		}

		r = cb(ce);
		if (r != 1)
			return r;
	}

descend:
	if (ce->block != NULL) {
		for (blockp = ce->block; *blockp; blockp++) {
			r = foreach(*blockp, opts, depth + 1, cb);
			if (r != 1)
				return r;
		}
	}

done:
	return 1;
}

/* Iterate over each config entry, optionally filtered. */
int
scfg_foreach(const struct scfgiteropts *opts, int (*cb)(struct scfgentry *))
{
	struct scfgentry *root;

	root = cfgroot;

	if (opts != NULL && opts->root != NULL)
		root = opts->root;

	if (root == NULL)
		return 1;

	return foreach(root, opts, 0, cb);
}
