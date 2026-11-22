/*
 * sx1302_hal stubs for features this gateway does not use: USB concentrators
 * and Linux i2c-dev (no sensor is reachable through the carrier).
 */
#include "loragw_i2c.h"
#include "loragw_usb.h"
#include "sx1250_usb.h"
#include "sx1261_usb.h"

/* ---- i2c-dev: not reachable on the carrier ------------------------------- */

/* lgw_start aborts if the *open* fails, but treats a failed sensor probe
 * (stts751_configure) as "no sensor". So open succeeds with a dummy handle and
 * every transfer fails: the HAL then uses the fixed temperature. */
#define LGW_PORT_I2C_DUMMY_FD 1000

int i2c_linuxdev_open(const char *path, uint8_t device_addr, int *i2c_fd)
{
    (void)path;
    (void)device_addr;
    if (i2c_fd == NULL) {
        return LGW_I2C_ERROR;
    }
    *i2c_fd = LGW_PORT_I2C_DUMMY_FD;
    return LGW_I2C_SUCCESS;
}

int i2c_linuxdev_close(int i2c_fd)
{
    return (i2c_fd == LGW_PORT_I2C_DUMMY_FD) ? LGW_I2C_SUCCESS : LGW_I2C_ERROR;
}

int i2c_linuxdev_read(int i2c_fd, uint8_t device_addr, uint8_t reg_addr, uint8_t *data)
{
    (void)i2c_fd;
    (void)device_addr;
    (void)reg_addr;
    (void)data;
    return LGW_I2C_ERROR;
}

int i2c_linuxdev_write(int i2c_fd, uint8_t device_addr, uint8_t reg_addr, uint8_t data)
{
    (void)i2c_fd;
    (void)device_addr;
    (void)reg_addr;
    (void)data;
    return LGW_I2C_ERROR;
}

int i2c_linuxdev_write_buffer(int i2c_fd, uint8_t device_addr, uint8_t *buffer, uint8_t size)
{
    (void)i2c_fd;
    (void)device_addr;
    (void)buffer;
    (void)size;
    return LGW_I2C_ERROR;
}

/* ---- USB concentrators: not supported ------------------------------------ */

int lgw_usb_open(const char *com_path, void **com_target_ptr)
{
    (void)com_path;
    (void)com_target_ptr;
    return LGW_USB_ERROR;
}

int lgw_usb_close(void *com_target)
{
    (void)com_target;
    return LGW_USB_ERROR;
}

int lgw_usb_w(void *com_target, uint8_t spi_mux_target, uint16_t address, uint8_t data)
{
    (void)com_target;
    (void)spi_mux_target;
    (void)address;
    (void)data;
    return LGW_USB_ERROR;
}

int lgw_usb_r(void *com_target, uint8_t spi_mux_target, uint16_t address, uint8_t *data)
{
    (void)com_target;
    (void)spi_mux_target;
    (void)address;
    (void)data;
    return LGW_USB_ERROR;
}

int lgw_usb_wb(void *com_target, uint8_t spi_mux_target, uint16_t address, const uint8_t *data,
               uint16_t size)
{
    (void)com_target;
    (void)spi_mux_target;
    (void)address;
    (void)data;
    (void)size;
    return LGW_USB_ERROR;
}

int lgw_usb_rb(void *com_target, uint8_t spi_mux_target, uint16_t address, uint8_t *data,
               uint16_t size)
{
    (void)com_target;
    (void)spi_mux_target;
    (void)address;
    (void)data;
    (void)size;
    return LGW_USB_ERROR;
}

int lgw_usb_rmw(void *com_target, uint16_t address, uint8_t offs, uint8_t leng, uint8_t data)
{
    (void)com_target;
    (void)address;
    (void)offs;
    (void)leng;
    (void)data;
    return LGW_USB_ERROR;
}

int lgw_usb_set_write_mode(lgw_com_write_mode_t write_mode)
{
    (void)write_mode;
    return LGW_USB_ERROR;
}

int lgw_usb_flush(void *com_target)
{
    (void)com_target;
    return LGW_USB_ERROR;
}

uint16_t lgw_usb_chunk_size(void)
{
    return 0;
}

int lgw_usb_get_temperature(void *com_target, float *temperature)
{
    (void)com_target;
    (void)temperature;
    return LGW_USB_ERROR;
}

int sx1250_usb_w(void *com_target, uint8_t spi_mux_target, sx1250_op_code_t op_code,
                 uint8_t *data, uint16_t size)
{
    (void)com_target;
    (void)spi_mux_target;
    (void)op_code;
    (void)data;
    (void)size;
    return LGW_USB_ERROR;
}

int sx1250_usb_r(void *com_target, uint8_t spi_mux_target, sx1250_op_code_t op_code,
                 uint8_t *data, uint16_t size)
{
    (void)com_target;
    (void)spi_mux_target;
    (void)op_code;
    (void)data;
    (void)size;
    return LGW_USB_ERROR;
}

int sx1261_usb_w(void *com_target, sx1261_op_code_t op_code, uint8_t *data, uint16_t size)
{
    (void)com_target;
    (void)op_code;
    (void)data;
    (void)size;
    return LGW_USB_ERROR;
}

int sx1261_usb_r(void *com_target, sx1261_op_code_t op_code, uint8_t *data, uint16_t size)
{
    (void)com_target;
    (void)op_code;
    (void)data;
    (void)size;
    return LGW_USB_ERROR;
}

int sx1261_usb_set_write_mode(lgw_com_write_mode_t write_mode)
{
    (void)write_mode;
    return LGW_USB_ERROR;
}

int sx1261_usb_flush(void *com_target)
{
    (void)com_target;
    return LGW_USB_ERROR;
}
