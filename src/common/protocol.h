#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include "errors.h"

// Protocol: Line-based. Client->NM sends commands as single lines.
// NM responds with either:
//   OK
//   DATA <n>    (optional, followed by n bytes in a separate "blob" line mode)
//   .           (terminator for multi-line textual responses)
// or
//   ERR <code> <message>
// For textual multi-line instead of DATA, NM may use:
//   OK
//   <lines...>
//   .

#define PROTO_MAX_LINE 4096

// Helpers to compose responses
int proto_send_ok(int fd);
int proto_send_err(int fd, enum ErrorCode code, const char *msg);
int proto_send_text_block(int fd, const char *text);
int proto_send_kv(int fd, const char *k, const char *v);

#endif // PROTOCOL_H
