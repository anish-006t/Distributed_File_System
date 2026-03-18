#include "strutils.h"

#include <ctype.h>
#include <string.h>

void str_trim(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    size_t start = 0, end = len;
    while (start < len && isspace((unsigned char)s[start])) start++;
    while (end > start && isspace((unsigned char)s[end-1])) end--;
    if (start > 0) memmove(s, s + start, end - start);
    s[end - start] = '\0';
}

void str_tolower(char *s) {
    for (; s && *s; ++s) *s = (char)tolower((unsigned char)*s);
}

bool str_startswith(const char *s, const char *prefix) {
    size_t i = 0;
    while (prefix[i]) {
        if (s[i] != prefix[i]) return false;
        i++;
    }
    return true;
}

bool str_equals_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (char)tolower((unsigned char)*a);
        char cb = (char)tolower((unsigned char)*b);
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == *b;
}

int str_split_ws(char *s, char **tokens, int max_tokens) {
    int count = 0;
    char *p = s;
    while (*p && count < max_tokens) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        tokens[count++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) { *p = '\0'; p++; }
    }
    return count;
}

char *str_join_space(char *out, size_t outsz, char **tokens, int count) {
    size_t pos = 0;
    for (int i = 0; i < count; ++i) {
        size_t len = strlen(tokens[i]);
        if (pos + len + (i ? 1 : 0) + 1 > outsz) return NULL;
        if (i) out[pos++] = ' ';
        memcpy(out + pos, tokens[i], len);
        pos += len;
    }
    out[pos] = '\0';
    return out;
}
