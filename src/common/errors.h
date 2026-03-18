#ifndef ERRORS_H
#define ERRORS_H

enum ErrorCode {
    ERR_OK = 0,
    ERR_UNKNOWN = 1,
    ERR_BAD_REQUEST = 2,
    ERR_UNAUTHORIZED = 3,
    ERR_FORBIDDEN = 4,
    ERR_NOT_FOUND = 5,
    ERR_CONFLICT = 6,
    ERR_LOCKED = 7,
    ERR_RANGE = 8,
    ERR_INTERNAL = 9,
    ERR_CONSISTENCY = 10
};

#endif // ERRORS_H
