#include "bt/bthid/bthid.h"
#include "bt/bthid/bthid_registry.h"
#include "bt/bthid/devices/generic/bthid_gamepad.h"
#include "bt/bthid/devices/vendors/sony/ds3_bt.h"
#include "bt/bthid/devices/vendors/sony/ds4_bt.h"
#include "bt/bthid/devices/vendors/sony/ds5_bt.h"

/* Keep this adapter's JoypadOS surface deliberately small. Xbox and 8BitDo
 * controllers use JoypadOS' descriptor-driven generic gamepad parser. */
void bthid_registry_init(void)
{
    bthid_init();
    ds3_bt_register();
    ds4_bt_register();
    ds5_bt_register();
    bthid_gamepad_register();
}
