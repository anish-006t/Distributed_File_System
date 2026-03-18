#include "protocol.h"
#include "socket_utils.h"

#include <stdio.h>
#include <string.h>

int proto_send_ok(int fd) {
    return su_send_line(fd, "OK");
}

int proto_send_err(int fd, enum ErrorCode code, const char *msg) {
    char buf[PROTO_MAX_LINE];
    snprintf(buf, sizeof(buf), "ERR %d %s", code, msg ? msg : "");
    return su_send_line(fd, buf);
}

int proto_send_text_block(int fd, const char *text) {
    if (proto_send_ok(fd) < 0) return -1;
    const char *p = text ? text : "";
    size_t len = strlen(p);
    size_t pos = 0;
    const size_t MAX = PROTO_MAX_LINE - 1; // reserve for null terminator
    // Emit lines split on '\n'; if a logical line exceeds buffer, chunk it
    while (pos < len) {
        // Find end of logical line (next \n or end)
        size_t start = pos;
        while (pos < len && p[pos] != '\n') pos++;
        size_t logical_len = pos - start;
        // Emit in chunks of at most MAX
        size_t emitted = 0;
        while (emitted < logical_len) {
            size_t chunk = logical_len - emitted;
            if (chunk > MAX) chunk = MAX;
            char line[PROTO_MAX_LINE];
            memcpy(line, p + start + emitted, chunk);
            line[chunk] = '\0';
            if (su_send_line(fd, line) < 0) return -1;
            emitted += chunk;
        }
        // If logical line was empty (e.g., consecutive newlines), send empty line
        if (logical_len == 0) {
            if (su_send_line(fd, "") < 0) return -1;
        }
        // Skip the '\n'
        if (pos < len && p[pos] == '\n') pos++;
    }
    // Handle case of empty text: send a single empty line
    if (len == 0) {
        if (su_send_line(fd, "") < 0) return -1;
    }
    return su_send_line(fd, ".");
}

int proto_send_kv(int fd, const char *k, const char *v) {
    char buf[PROTO_MAX_LINE];
    snprintf(buf, sizeof(buf), "%s: %s", k, v);
    return su_send_line(fd, buf);
}
