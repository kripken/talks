/*
 * Hackish way to implement setjmp/longjmp in pure wasm using Binaryen's
 * Asyncify pass.
 *
 * compile with
 *
 *   clang -O --target=wasm32 -c jmp.c -o jmp.o -Wno-incomplete-setjmp-declaration -Wno-incompatible-library-redeclaration -g
 *   wasm-ld --no-entry --export-all --allow-undefined jmp.o -o jmp.wasm
 *   wasm-opt jmp.wasm --asyncify -O -o jmp_async.wasm -g --pass-arg=asyncify-ignore-imports
 *
 * run with one of:
 *
 *    wasmer run jmp.wasm
 *    wasmtime jmp_async.wasm
 *    etc.
 *
 * Code based off the very useful
 *
 *    https://gist.github.com/s-macke/6dd78c78be46214d418454abb667a1ba
 *
 * by @s-macke
 */

#define NULL 0

#define WASM_IMPORT(module, base) \
    __attribute__((__import_module__(#module), __import_name__(#base)))

#define NOINLINE __attribute__((noinline))

// The Asyncify API

void asyncify_start_unwind(void* buf) WASM_IMPORT(asyncify, start_unwind);
void asyncify_stop_unwind() WASM_IMPORT(asyncify, stop_unwind);
void asyncify_start_rewind(void* buf) WASM_IMPORT(asyncify, start_rewind);
void asyncify_stop_rewind() WASM_IMPORT(asyncify, stop_rewind);

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

__wasi_errno_t __wasi_fd_write(
    __wasi_fd_t fd,
    const __wasi_ciovec_t *iovs,
    size_t iovs_len,
    size_t *nwritten
) WASM_IMPORT(wasi_unstable, fd_write) __attribute__((__warn_unused_result__));

// ----------------------------------------------------
// returns the length of the string
NOINLINE
size_t strlen(const char *str) {
    const char *s;
    for (s = str; *s; ++s);
    return (s - str);
}

// sends string to stdout
NOINLINE
void print(const char *str) {
    const __wasi_fd_t stdout = 1;
    size_t nwritten;
    __wasi_errno_t error;
    __wasi_ciovec_t iovec;
    iovec.buf = str;
    iovec.buf_len = strlen(str);
    error =__wasi_fd_write(stdout, &iovec, 1, &nwritten);
}

// ----------------------------------------------------

//
// The "upper runtime": A weird impl of setjmp etc.
//

struct async_buf {
    void* top; // current top of the buffer
    void* end; // end of the buffer
    void* unwound; // the top of the buffer when fully unwound and ready to rewind
    char buffer[1000];
};

NOINLINE
void init_buf(struct async_buf* buf) {
    buf->top = &buf->buffer[0];
    buf->end = &buf->buffer[1000];
}

NOINLINE
void note_unwound_buf(struct async_buf* buf) {
    buf->unwound = buf->top;
}

NOINLINE
void rewind_buf(struct async_buf* buf) {
    buf->top = buf->unwound;
}

struct async_buf buf1, buf2;

NOINLINE
int setjmp(char* buf) {
    static int counter = 0;
    int ret = 0;
    if (counter == 0) {
        asyncify_start_unwind(&buf1);
    } else if (counter == 1) {
        asyncify_stop_rewind();
    } else if (counter == 2) {
        asyncify_stop_rewind();
        ret = 1;
    } else {
        print("wat\n");
    }
    counter++;
    return ret;
}

NOINLINE
void longjmp(char* buf, int value) {
    asyncify_start_unwind(&buf2);
}

//
// The user program itself
//

// An inner function.
NOINLINE
void inner() {
    print("call longjmp\n");
    longjmp(NULL, 1);
}

// The main part of the program (avoiding main() because of wasi).
NOINLINE
void program() {
    print("start\n");
    if (!setjmp(NULL)) {
        print("call inner\n");
        inner();
    } else {
        print("back from longjmp\n");
    }
    print("end\n");
}

//
// The "lower runtime": Starts everything, is unwound to, etc.
//

void _start() {
    init_buf(&buf1);
    init_buf(&buf2);
    // Call main the first time.
    program();
    // Then it unwinds to here.
    asyncify_stop_unwind();
    // Note the unwound state, as we want to rewind it later.
    note_unwound_buf(&buf1);
    asyncify_start_rewind(&buf1);
    // Call it a second time to return to setjmp().
    program();
    // Longjmp has unwinded to here.
    asyncify_stop_unwind();
    // Now rewind to a different place than we unwinded! We unwinded from longjmp, but rewind to the setjmp.
    rewind_buf(&buf1);
    asyncify_start_rewind(&buf1);
    program();
}
