// Prints argv[0] so tests can verify bionilux preserves it.
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc;
    printf("argv0=%s\n", argv[0]);
    return 0;
}