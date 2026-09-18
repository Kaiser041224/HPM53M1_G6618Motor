#include "app_usb.h"

#include "intf_usb_cdc.h"

#include <string.h>

/* Driver registration */
extern void hpm_usb_cdc_driver_register(void);

void app_usb_init(void)
{
    hpm_usb_cdc_driver_register();

    (void) intf_usb_cdc_init();
}

int app_usb_write(const uint8_t *data, size_t len)
{
    return intf_usb_cdc_write(data, len, 100U);
}

int app_usb_write_str(const char *str)
{
    return app_usb_write((const uint8_t *) str, strlen(str));
}
