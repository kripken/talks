#include <setjmp.h>
#include <stdio.h>

jmp_buf buf;

__attribute__((noinline))
void inner() {
    puts("call longjmp");
    longjmp(buf, 1);
}

int main() {
    puts("start");
    if (!setjmp(buf)) {
        puts("call inner");
        inner();
    } else {
        puts("back from longjmp");
    }
    puts("end");
}

