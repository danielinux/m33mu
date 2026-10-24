/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef M33MU_I2C_BUS_H
#define M33MU_I2C_BUS_H

#include "m33mu/types.h"

typedef mm_bool (*mm_i2c_write_fn)(void *opaque, mm_u8 addr,
                                  const mm_u8 *data, size_t len);
typedef mm_bool (*mm_i2c_read_fn)(void *opaque, mm_u8 addr,
                                 mm_u8 *data, size_t len);
typedef void (*mm_i2c_reset_fn)(void *opaque);
typedef void (*mm_i2c_shutdown_fn)(void *opaque);

struct mm_i2c_device {
    int bus;
    mm_u8 addr;
    mm_i2c_write_fn write;
    mm_i2c_read_fn read;
    mm_i2c_reset_fn reset;
    mm_i2c_shutdown_fn shutdown;
    void *opaque;
};

enum mm_i2c_status {
    MM_I2C_OK = 0,
    MM_I2C_NACK,
    MM_I2C_BUSY,        /* a line is held low: the controller cannot start */
    MM_I2C_ARLO         /* another driver won arbitration */
};

/* Optional host model of the wires. It sees every transfer first: return
 * MM_TRUE with *status set to handle it, or MM_FALSE to fall through to the
 * registered devices. lines_busy reports SCL/SDA held low between transfers. */
struct mm_i2c_host {
    mm_bool (*xfer)(void *opaque, int bus, mm_u8 addr, mm_bool read,
                    mm_u8 *data, size_t len, enum mm_i2c_status *status);
    mm_bool (*lines_busy)(void *opaque, int bus);
    void *opaque;
};

void mm_i2c_bus_set_host(const struct mm_i2c_host *host);
enum mm_i2c_status mm_i2c_bus_transfer(int bus, mm_u8 addr, mm_bool read,
                                       mm_u8 *data, size_t len);
mm_bool mm_i2c_bus_lines_busy(int bus);
mm_bool mm_i2c_bus_register_device(const struct mm_i2c_device *dev);
mm_bool mm_i2c_bus_write(int bus, mm_u8 addr, const mm_u8 *data, size_t len);
mm_bool mm_i2c_bus_read(int bus, mm_u8 addr, mm_u8 *data, size_t len);
void mm_i2c_bus_reset_all(void);
void mm_i2c_bus_shutdown_all(void);
/* Probe: returns MM_TRUE (ACK) if any device responds at this address. */
mm_bool mm_i2c_bus_probe(int bus, mm_u8 addr);
/* Reset all devices on one specific bus only. */
void mm_i2c_bus_reset(int bus);

#endif /* M33MU_I2C_BUS_H */
