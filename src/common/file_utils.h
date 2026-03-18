#ifndef FILE_UTILS_H
#define FILE_UTILS_H

#include <stddef.h>
#include <stdbool.h>

int fu_mkdirs(const char *path);
bool fu_exists(const char *path);
char *fu_read_all(const char *path, size_t *out_len);
int fu_write_all(const char *path, const char *data, size_t len);
int fu_copy_file(const char *src, const char *dst);

#endif // FILE_UTILS_H
