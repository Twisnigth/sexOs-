#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    printf("=== Programme musl-libc statique execute par sexOs ===\n");
    printf("argc=%d argv[0]=%s\n", argc, argv[0]);
    char *buf = malloc(64);          // exerce brk/mmap
    strcpy(buf, "malloc + printf fonctionnent en ring 3 !");
    printf("%s\n", buf);
    for (int i = 1; i <= 3; i++) printf("  ligne %d\n", i);
    free(buf);
    return 7;
}
