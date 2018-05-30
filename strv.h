#ifndef STRV_H
#define STRV_H

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char **addstr(char **, const char *);
char **dupstrv(char **);
void clrstrv(char ***);
void fprintstrv(FILE *, char **);
void printstrv(char **);

#endif
