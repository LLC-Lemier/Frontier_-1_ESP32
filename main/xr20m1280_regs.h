#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    XR20_REG_RHR_THR = 0,
    XR20_REG_IER = 1,
    XR20_REG_IIR_FCR = 2,
    XR20_REG_LCR = 3,
    XR20_REG_MCR = 4,
    XR20_REG_LSR = 5,
    XR20_REG_MSR = 6,
    XR20_REG_SCR = 7,
};

#define XR20_LSR_DR 0x01
#define XR20_LSR_OE 0x02
#define XR20_LSR_PE 0x04
#define XR20_LSR_FE 0x08
#define XR20_LSR_BI 0x10
#define XR20_LSR_THRE 0x20
#define XR20_LSR_TEMT 0x40
#define XR20_LSR_FIFOE 0x80

#define XR20_LCR_DLAB 0x80

static inline uint8_t xr20m1280_spi_header(uint8_t reg, bool read)
{
    return (read ? 0x80u : 0u) | (uint8_t)((reg & 7u) << 3);
}
