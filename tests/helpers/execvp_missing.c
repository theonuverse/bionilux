// execvp on a name that does not exist. The caller must observe ENOENT,
// not a silent 127 from a launcher that could not find the target.
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char *argv[] = { "bionilux-no-such-binary-xyz", NULL };
    execvp(argv[0], argv);
    if (errno == ENOENT) {
        puts("enoent-ok");
        return 0;
    }
    printf("wrong-errno=%d (%s)\n", errno, strerror(errno));
    return 1;
}