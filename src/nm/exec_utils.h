#ifndef EXEC_UTILS_H
#define EXEC_UTILS_H

// Execute the provided script content via /bin/bash and capture stdout+stderr.
// Returns a newly malloc'd buffer with output (caller frees), or NULL on error.
char *exec_capture_bash(const char *script, int *exit_code);

#endif // EXEC_UTILS_H
