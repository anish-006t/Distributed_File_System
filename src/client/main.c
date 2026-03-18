#include <stdio.h>
#include <stdlib.h>
#include "client_ui.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <nm_host> <nm_port>\n", argv[0]);
        return 1;
    }
    const char *host = argv[1];
    unsigned short port = (unsigned short)atoi(argv[2]);
    return run_client(host, port);
}
