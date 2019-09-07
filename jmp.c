/*
 * Based off the very useful
 * https://gist.github.com/s-macke/6dd78c78be46214d418454abb667a1ba
 * by @s-macke
 *
 * compile with
set -e
~/Dev/sdk/upstream/bin/clang -O --target=wasm32 -c jmp.c -o jmp.o -Wno-incomplete-setjmp-declaration -g
~/Dev/sdk/upstream/bin/wasm-ld --no-entry --export-all --allow-undefined jmp.o -o jmp.wasm
~/Dev/binaryen/bin/wasm-opt jmp.wasm --asyncify -O -o jmp_async.wasm -g --pass-arg=asyncify-ignore-imports
~/Dev/binaryen/bin/wasm2js jmp_async.wasm -O -o a.js -g


~/Dev/emscripten/emcc jmp.c -w -s ERROR_ON_UNDEFINED_SYMBOLS=0 --profiling
~/Dev/binaryen/bin/wasm-opt a.out.wasm --asyncify -o b.out.wasm -g --pass-arg=asyncify-ignore-imports
 *
 * run with
 *
 *    https://github.com/wasmerio/wasmer
 *        wasmer run jmp.wasm
 */

#define NULL 0

// ----------------------------------------------------
// Short version of
// https://github.com/CraneStation/wasi-libc/blob/master/libc-bottom-half/headers/public/wasi/core.h
// to support the WASI fd_write function

typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

typedef long size_t;
typedef uint16_t __wasi_errno_t;
typedef uint32_t __wasi_fd_t;

typedef struct __wasi_ciovec_t {
    const void *buf;
    size_t buf_len;
} __wasi_ciovec_t;

#define WASM_IMPORT(module, base) \
    __attribute__((__import_module__(#module), __import_name__(#base)))

__wasi_errno_t __wasi_fd_write(
    __wasi_fd_t fd,
    const __wasi_ciovec_t *iovs,
    size_t iovs_len,
    size_t *nwritten
) WASM_IMPORT(wasi_unstable, fd_write) __attribute__((__warn_unused_result__));

// ----------------------------------------------------
// returns the length of the string
__attribute__((noinline))
size_t slen(const char *str) {
    const char *s;
    for (s = str; *s; ++s);
    return (s - str);
}

// sends string to stdout
__attribute__((noinline))
void print(const char *str) {
    const __wasi_fd_t stdout = 1;
    size_t nwritten;
    __wasi_errno_t error;
    __wasi_ciovec_t iovec;
    iovec.buf = str;
    iovec.buf_len = slen(str);
    error =__wasi_fd_write(stdout, &iovec, 1, &nwritten);
}

// ----------------------------------------------------

// The "upper runtime": setjmp etc.

void asyncify_start_unwind(void* buf) WASM_IMPORT(asyncify, start_unwind);
void asyncify_stop_unwind() WASM_IMPORT(asyncify, stop_unwind);
void asyncify_start_rewind(void* buf) WASM_IMPORT(asyncify, start_rewind);
void asyncify_stop_rewind() WASM_IMPORT(asyncify, stop_rewind);

struct async_buf {
    void* top;
    void* end;
    char buffer[1000];
};

__attribute__((noinline))
void rewind_buf(struct async_buf* buf) {
    buf->top = &buf->buffer[0];
}

__attribute__((noinline))
void init_buf(struct async_buf* buf) {
    rewind_buf(buf);
    buf->end = &buf->buffer[1000];
}

struct async_buf buf1, buf2;

__attribute__((noinline))
int setjmp(char* buf) {
    static int counter = 0;
    int ret = 0;
    if (counter == 0) {
        print("setjmp1\n");
        asyncify_start_unwind(&buf1);
    } else if (counter == 1) {
        asyncify_stop_rewind();
        print("setjmp back from initial resume\n");
    } else if (counter == 3) {
        print("setjmp back from longjmp\n");
        asyncify_stop_rewind();
        ret = 1;
    } else {
        print("wat\n");
    }
    counter++;
    return ret;
}

__attribute__((noinline))
void longjmp(char* buf, int value) {
    print("longjmp\n");
    asyncify_start_unwind(&buf2);
}

__attribute__((noinline))
void second() {
    print("second\n");
    longjmp(NULL, 1);
}

__attribute__((noinline))
void first() {
    print("first\n");
    second();
}

__attribute__((noinline))
void program() {
    print("start\n");
    if (!setjmp(NULL)) {
        first();
    } else {
        print("back\n");
    }
    print("end\n");
}

// The "lower runtime".
#if __EMSCRIPTEN__
int main() {
#else
void _start() {
#endif
    init_buf(&buf1);
    init_buf(&buf2);
    print("a\n");
    // Call main the first time.
    program();
    // Then it unwinds to here.
    print("b\n");
    asyncify_stop_unwind();
    print("c\n");
    asyncify_start_rewind(&buf1);
    // Call it a second time to return to setjmp().
    print("d\n");
    program();
    print("e\n");
    // Longjmp has unwinded to here.
    asyncify_stop_unwind();
    // Now rewind to a different place than we unwinded! We unwinded from longjmp, but rewind to the setjmp.
    rewind_buf(&buf1);
    asyncify_start_rewind(&buf1);
    print("f\n");
    program();
    print("g\n");
}
