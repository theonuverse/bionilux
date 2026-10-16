// Calls system() and checks the exit status. On Termux, glibc's system()
// needs /bin/sh which does not exist. If it works, Termux's glibc has
// _PATH_BSHELL rewritten; if not, the test fails cleanly.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int rc = system("echo system-ok");
    if (rc == 0) return 0;
    fprintf(stderr, "system() returned %d\n", rc);
    return 1;
}