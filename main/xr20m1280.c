#include "xr20m1280.h"
#include "xr20m1280_regs.h"
#include "esp_check.h"
#include "driver/spi_master.h"
#include <string.h>

static esp_err_t spi_xfer2(spi_device_handle_t dev, uint8_t tx0, uint8_t tx1, uint8_t *rx1_out)
{
    uint8_t tx[2] = {tx0, tx1};
    uint8_t rx[2] = {0, 0};
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 16;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    esp_err_t err = spi_device_polling_transmit(dev, &t);
    if (rx1_out) {
        *rx1_out = rx[1];
    }
    return err;
}

esp_err_t xr20m1280_write_reg(spi_device_handle_t dev, uint8_t reg, uint8_t val)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t hdr = xr20m1280_spi_header(reg, false);
    return spi_xfer2(dev, hdr, val, NULL);
}

esp_err_t xr20m1280_read_reg(spi_device_handle_t dev, uint8_t reg, uint8_t *val)
{
    if (!dev || !val) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t hdr = xr20m1280_spi_header(reg, true);
    return spi_xfer2(dev, hdr, 0, val);
}

esp_err_t xr20m1280_fifo_enable(spi_device_handle_t dev)
{
    ESP_RETURN_ON_ERROR(xr20m1280_write_reg(dev, XR20_REG_IIR_FCR, 0x07), "xr20", "FCR");
    return ESP_OK;
}

static uint8_t make_lcr(uint8_t data_bits, uint8_t stop_bits, uint8_t parity)
{
    uint8_t lcr = 0;
    switch (data_bits) {
        case 5:
            lcr |= 0x00;
            break;
        case 6:
            lcr |= 0x01;
            break;
        case 7:
            lcr |= 0x02;
            break;
        default:
            lcr |= 0x03;
            break;
    }
    if (stop_bits >= 2) {
        lcr |= 0x04;
    }
    if (parity == 1) {
        lcr |= 0x08;
    } else if (parity == 2) {
        lcr |= 0x18;
    }
    return lcr;
}

esp_err_t xr20m1280_set_line_config(spi_device_handle_t dev, uint32_t uart_clock_hz, uint32_t baud,
                                    uint8_t data_bits, uint8_t stop_bits, uint8_t parity)
{
    if (!dev || baud == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t divisor = (uart_clock_hz + 8u * baud) / (16u * baud);
    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 0xFFFF) {
        divisor = 0xFFFF;
    }

    uint8_t lcr = make_lcr(data_bits, stop_bits, parity);
    ESP_RETURN_ON_ERROR(xr20m1280_write_reg(dev, XR20_REG_LCR, lcr | XR20_LCR_DLAB), "xr20", "LCR DLAB");
    ESP_RETURN_ON_ERROR(xr20m1280_write_reg(dev, XR20_REG_RHR_THR, (uint8_t)(divisor & 0xFF)), "xr20", "DLL");
    ESP_RETURN_ON_ERROR(xr20m1280_write_reg(dev, XR20_REG_IER, (uint8_t)((divisor >> 8) & 0xFF)), "xr20", "DLM");
    ESP_RETURN_ON_ERROR(xr20m1280_write_reg(dev, XR20_REG_LCR, lcr), "xr20", "LCR final");
    return ESP_OK;
}

esp_err_t xr20m1280_read_lsr(spi_device_handle_t dev, uint8_t *lsr)
{
    return xr20m1280_read_reg(dev, XR20_REG_LSR, lsr);
}

esp_err_t xr20m1280_read_rhr(spi_device_handle_t dev, uint8_t *byte)
{
    return xr20m1280_read_reg(dev, XR20_REG_RHR_THR, byte);
}

esp_err_t xr20m1280_write_thr(spi_device_handle_t dev, uint8_t byte)
{
    return xr20m1280_write_reg(dev, XR20_REG_RHR_THR, byte);
}
