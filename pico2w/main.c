#include <btstack_run_loop.h>
#include <pico/cyw43_arch.h>
#include <pico/stdlib.h>
#include <uni.h>

#include "sdkconfig.h"

struct uni_platform *rjm_bluepad_platform(void);

int main(void)
{
    if (cyw43_arch_init()) return 1;
    uni_platform_set_custom(rjm_bluepad_platform());
    uni_init(0, NULL);
    btstack_run_loop_execute();
    return 0;
}
