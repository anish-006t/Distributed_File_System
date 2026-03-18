#include "exec_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <ctype.h>
#include "../common/log.h"

// Helper function to split text by newlines (proper shell command separation)
// Spec requires executing file content as shell commands, not sentence-based
static int split_into_commands(const char *text, char ***out_commands) {
    int capacity = 16;
    int count = 0;
    char **commands = (char**)malloc(capacity * sizeof(char*));
    if (!commands) return 0;
    
    const char *start = text;
    const char *p = text;
    
    while (*p) {
        // Only treat newlines as command boundaries (not punctuation)
        if (*p == '\n') {
            // Extract line (without the newline)
            size_t len = p - start;
            if (len > 0) {
                // Trim leading/trailing whitespace
                while (len > 0 && isspace((unsigned char)start[0])) { start++; len--; }
                while (len > 0 && isspace((unsigned char)start[len-1])) { len--; }
                
                if (len > 0) {
                    char *cmd = (char*)malloc(len + 1);
                    if (cmd) {
                        memcpy(cmd, start, len);
                        cmd[len] = '\0';
                        
                        if (count >= capacity) {
                            capacity *= 2;
                            commands = (char**)realloc(commands, capacity * sizeof(char*));
                        }
                        commands[count++] = cmd;
                    }
                }
            }
            
            // Move past newline
            p++;
            start = p;
        } else {
            p++;
        }
    }
    
    // Handle trailing text without newline
    size_t len = p - start;
    while (len > 0 && isspace((unsigned char)start[0])) { start++; len--; }
    while (len > 0 && isspace((unsigned char)start[len-1])) { len--; }
    if (len > 0) {
        char *cmd = (char*)malloc(len + 1);
        if (cmd) {
            memcpy(cmd, start, len);
            cmd[len] = '\0';
            if (count >= capacity) {
                capacity *= 2;
                commands = (char**)realloc(commands, capacity * sizeof(char*));
            }
            commands[count++] = cmd;
        }
    }
    
    *out_commands = commands;
    return count;
}

char *exec_capture_bash(const char *script, int *exit_code) {
    if (!script) {
        if (exit_code) *exit_code = 0;
        return strdup("");
    }
    // Split script into individual commands by sentence delimiters
    char **commands = NULL;
    int num_commands = split_into_commands(script, &commands);
    
    if (num_commands == 0) {
        if (exit_code) *exit_code = 0;
        return strdup("");
    }
    
    // Create temporary script file (mkstemps requires a template buffer we can modify)
    char tmpl[] = "/tmp/nm_exec_XXXXXX.sh";
    int fd = mkstemps(tmpl, 3); // .sh suffix
    if (fd < 0) {
        for (int i = 0; i < num_commands; i++) free(commands[i]);
        free(commands);
        return NULL;
    }
    
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        for (int i = 0; i < num_commands; i++) free(commands[i]);
        free(commands);
        return NULL;
    }
    
    // Write each command on a separate line with set -e to capture errors but continue collecting output manually
    fprintf(f, "#!/bin/bash\n");
    fprintf(f, "set +o pipefail\n");
    for (int i = 0; i < num_commands; i++) {
        if (!commands[i]) continue;
        fprintf(f, "%s\n", commands[i]);
        free(commands[i]);
    }
    free(commands);
    fclose(f);
    // Make script executable
    chmod(tmpl, 0700);

    // Execute the script
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "/bin/bash '%s' 2>&1", tmpl);
    FILE *p = popen(cmd, "r");
    if (!p) { unlink(tmpl); return NULL; }
    
    size_t cap = 8192, len = 0; // start with 8KB
    char *buf = (char*)malloc(cap);
    if (!buf) { pclose(p); unlink(tmpl); return NULL; }
    
    int c;
    const size_t MAX_OUTPUT = 1<<20; // 1MB cap to avoid runaway memory
    while ((c = fgetc(p)) != EOF) {
        if (len + 1 >= cap) {
            size_t newcap = cap * 2;
            if (newcap > MAX_OUTPUT) newcap = MAX_OUTPUT;
            if (newcap == cap) { // reached cap
                // truncate remaining output
                while (fgetc(p) != EOF) { /* discard */ }
                break;
            }
            char *newbuf = (char*)realloc(buf, newcap);
            if (!newbuf) {
                log_error("exec_capture_bash: realloc failed at %zu bytes", newcap);
                break; // keep partial output
            }
            buf = newbuf;
            cap = newcap;
        }
        buf[len++] = (char)c;
    }
    
    int rc = pclose(p);
    unlink(tmpl);
    
    buf[len] = '\0';
    if (exit_code) *exit_code = WEXITSTATUS(rc);
    return buf;
}
