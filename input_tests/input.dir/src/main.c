// TODO: replace with a program that reads and prints input events
#include <stdio.h>
#include <unistd.h>

int main(void) {
    puts("input test: running, QEMU window should be up");
    for (int i = 0; i < 600; i++) sleep(1);
    puts("input test: done");
    return 0;
}
