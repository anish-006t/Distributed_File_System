#ifndef STRUTILS_H
#define STRUTILS_H

#include <stddef.h>
#include <stdbool.h>

void str_trim(char *s);
void str_tolower(char *s);
bool str_startswith(const char *s, const char *prefix);
bool str_equals_ci(const char *a, const char *b);

// Split by whitespace into tokens; modifies input by inserting NULs; returns count.
int str_split_ws(char *s, char **tokens, int max_tokens);

// Join strings with space into out (size outsz). Returns out on success or NULL.
char *str_join_space(char *out, size_t outsz, char **tokens, int count);

#endif // STRUTILS_H
