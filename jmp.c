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
 *  wasmer run jmp.wasm
 *  wasmtime jmp_async.wasm
 *  etc.
 *
 * Code based off the very useful
 *
 *  https://gist.github.com/s-macke/6dd78c78be46214d418454abb667a1ba
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
  const void* buf;
  size_t buf_len;
} __wasi_ciovec_t;

__wasi_errno_t __wasi_fd_write(
  __wasi_fd_t fd,
  const __wasi_ciovec_t* iovs,
  size_t iovs_len,
  size_t* nwritten
) WASM_IMPORT(wasi_unstable, fd_write) __attribute__((__warn_unused_result__));

// ----------------------------------------------------
// returns the length of the string
NOINLINE
size_t strlen(const char* str) {
  const char* s;
  for (s = str;* s; ++s);
  return (s - str);
}

// sends string to stdout
NOINLINE
void print(const char* str) {
  const __wasi_fd_t stdout = 1;
  size_t nwritten;
  __wasi_errno_t error;
  __wasi_ciovec_t iovec;
  iovec.buf = str;
  iovec.buf_len = strlen(str);
  error =__wasi_fd_write(stdout, &iovec, 1, &nwritten);
}

NOINLINE
void puts(const char* str) {
  print(str);
  print("\n");
}

// ----------------------------------------------------

//
// The "upper runtime": A weird impl of setjmp etc.
//

// An Asyncify buffer.
struct async_buf {
  void* top; // current top of the buffer
  void* end; // end of the buffer
  void* unwound; // the top of the buffer when fully unwound and ready to rewind
  char buffer[1000];
};

NOINLINE
void async_buf_init(struct async_buf* buf) {
  buf->top = &buf->buffer[0];
  buf->end = &buf->buffer[1000];
}

NOINLINE
void async_buf_note_unwound(struct async_buf* buf) {
  buf->unwound = buf->top;
}

NOINLINE
void async_buf_rewind(struct async_buf* buf) {
  buf->top = buf->unwound;
}

// A setjmp/longjmp buffer.
struct jmp_buf {
  // A buffer for the setjmp. unwound and rewound immediately, and can
  // be rewound a second time to return from the longjmp.
  struct async_buf setjmp_buf;
  // A buffer for the longjmp. unwound once (and we rewind to the setjmp).
  struct async_buf longjmp_buf;
  // The value to return.
  int value;
  // FIXME We assume this is initialized to zero
  int state;
};

static struct jmp_buf* active_jmp_buf = NULL;

NOINLINE
int setjmp(struct jmp_buf* buf) {
  if (buf->state == 0) {
    active_jmp_buf = buf;
    async_buf_init(&buf->setjmp_buf);
    asyncify_start_unwind(&buf->setjmp_buf);
    buf->state = 1;
  } else {
    asyncify_stop_rewind();
    if (buf->state == 3) {
      // We returned from the longjmp, all done.
      active_jmp_buf = NULL;
    }
  }
  buf->state++;
  return buf->value;
}

NOINLINE
void longjmp(struct jmp_buf* buf, int value) {
  buf->value = value;
  async_buf_init(&buf->longjmp_buf);
  asyncify_start_unwind(&buf->longjmp_buf);
}

//
// The user program itself
//

static struct jmp_buf my_buf;

// An inner function.
NOINLINE
void inner() {
  puts("call longjmp");
  longjmp(&my_buf, 1);
}

// The main part of the program (avoiding main() because of wasi).
NOINLINE
void program() {
  puts("start");
  if (!setjmp(&my_buf)) {
    puts("call inner");
    inner();
  } else {
    puts("back from longjmp");
  }
  puts("end");
}

//
// The "lower runtime": Starts everything, is unwound to, resumes, etc.
//

void _start() {
  // This has enough logic to handle one longjmp.
  int state = 0;
  while (1) {
    program();
    if (!active_jmp_buf) {
      // The program has run to the end.
      return;
    }
    // The program is still working, just the stack has unwound to here.
    asyncify_stop_unwind();
    if (state == 0) {
      // Setjmp unwound to here. Prepare to rewind it twice.
      async_buf_note_unwound(&active_jmp_buf->setjmp_buf);
      asyncify_start_rewind(&active_jmp_buf->setjmp_buf);
    } else if (state == 1) {
      // Longjmp unwound to here. Rewind to the setjmp.
      async_buf_rewind(&active_jmp_buf->setjmp_buf);
      asyncify_start_rewind(&active_jmp_buf->setjmp_buf);
    }
    state++;
  }
}
