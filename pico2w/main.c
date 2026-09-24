#include <pico/stdlib.h>

#include "bt/transport/bt_transport.h"
#include "joypad_backend.h"

int main(void)
{
    stdio_init_all();
    if (!rjm_joypad_backend_init()) return 1;

    while (true) {
        bt_task();
        rjm_joypad_backend_task();
        tight_loop_contents();
    }
}
