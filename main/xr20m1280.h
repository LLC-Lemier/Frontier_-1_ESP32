#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/spi_master.h"

esp_err_t xr20m1280_write_reg(spi_device_handle_t dev, uint8_t reg, uint8_t val);
esp_err_t xr20m1280_read_reg(spi_device_handle_t dev, uint8_t reg, uint8_t *val);
esp_err_t xr20m1280_set_line_config(spi_device_handle_t dev, uint32_t uart_clock_hz, uint32_t baud,
                                    uint8_t data_bits, uint8_t stop_bits, uint8_t parity);
esp_err_t xr20m1280_fifo_enable(spi_device_handle_t dev);
esp_err_t xr20m1280_read_lsr(spi_device_handle_t dev, uint8_t *lsr);
esp_err_t xr20m1280_read_rhr(spi_device_handle_t dev, uint8_t *byte);
esp_err_t xr20m1280_write_thr(spi_device_handle_t dev, uint8_t byte);
