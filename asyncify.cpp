#include <emscripten.h>
#include <stdio.h>

int main() {
  // Do a nice smooth color shift.
  EM_ASM({
    setInterval(() => {
      document.body.style.background = "#0000" + Math.floor((Math.sin(Date.now() / 1000) + 1) * 128).toString(16);
    }, 1);
  });
  // But run an infinite loop!
  while (1) {
    puts("Infinite loop iteration.");
    emscripten_sleep(1000);
  }
}
