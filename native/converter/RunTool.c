#include <errno.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    if (setpgid(0, 0) != 0) { perror("setpgid"); return 1; }
    execv(argv[1], argv + 1);
    perror(argv[1]);
    return errno == ENOENT ? 127 : 126;
}
