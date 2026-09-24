// MIT License
//
// Copyright (c) 2026 Kevin Thomas
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Author:  Kevin Thomas
// Email:   kevin@mytechnotalent.com
// GitHub:  https://github.com/mytechnotalent/picokit-13-dht11-lcd
// File:    monitor.c
// Desc:    Implements the DHT11 and 1602 LCD state machine that pairs each
//          sample with an authenticated LoRa heartbeat.
// Created: 2026

#include "picokit_13_dht11_lcd.h"
#include "monitor.h"
#include "radio.h"
#include "status_led.h"
#include "sensor.h"
#include "display.h"
#include "crypto_aead.h"
#include "crypto_kdf.h"
#include "envelope.h"
#include "field_secrets.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/time.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief Module-ready flag.
 *
 * Set to true by monitor_init() once the peripherals are configured.
 * monitor_step() returns false while this flag is clear.
 */
static bool g_ready;

/**
 * @brief Initialized I2C peripheral handle for the LCD backpack.
 */
static i2c_inst_t *g_i2c;

/**
 * @brief Initialized I2C backpack address for the LCD.
 */
static uint8_t g_i2c_addr;

/**
 * @brief Monotonic transmit sequence number.
 */
static uint16_t g_seq;

/**
 * @brief Absolute time in microseconds of the next DHT11 sample.
 */
static uint64_t g_next_read_us;

/**
 * @brief Absolute time in microseconds of the next authenticated transmit.
 */
static uint64_t g_next_tx_us;

/**
 * @brief Most recent decoded DHT11 reading.
 */
static dht_reading_t g_reading;

/**
 * @brief First LCD render line buffer.
 */
static char g_line1[DISPLAY_LINE_LEN];

/**
 * @brief Second LCD render line buffer.
 */
static char g_line2[DISPLAY_LINE_LEN];

/**
 * @brief Inbound radio line accumulator.
 */
static char g_rx_line[RADIO_LINE_BUF_LEN];

/**
 * @brief Number of bytes currently held in the inbound line accumulator.
 */
static size_t g_rx_len;

/**
 * @brief Derived XChaCha20-Poly1305 session key for telemetry.
 */
static uint8_t g_key[CRYPTO_AEAD_KEY_LEN];

/**
 * @brief True once the telemetry session key has been derived.
 */
static bool g_key_ready;

/**
 * @brief Configure the onboard heartbeat LED as a dark output.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_state_init_io(void) {
    gpio_init(PICOKIT_13_DHT11_LCD_LED_PIN);
    gpio_set_dir(PICOKIT_13_DHT11_LCD_LED_PIN, GPIO_OUT);
    gpio_put(PICOKIT_13_DHT11_LCD_LED_PIN, 0);
}

/**
 * @brief Probe one I2C address and report whether it acknowledges.
 *
 * @param i2c Pointer to the I2C peripheral to probe.
 * @param addr The 7-bit address to probe.
 * @return bool true when the address acknowledged.
 */
static bool i2c_probe(i2c_inst_t *i2c, uint8_t addr) {
    uint8_t dummy = 0u;
    if (i2c_write_blocking(i2c, addr, &dummy, 1u, false) < 0) {
        return false;
    }
    printf("  found 0x%02X\n", (unsigned)addr);
    return true;
}

/**
 * @brief Probe the I2C bus and print every device that acknowledges.
 *
 * @param i2c Pointer to the I2C peripheral to scan.
 * @return void
 */
static void i2c_bus_scan(i2c_inst_t *i2c) {
    uint8_t addr;
    uint8_t found = 0u;
    printf("I2C scan:\n");
    for (addr = 0x08u; addr < 0x78u; ++addr) {
        found += i2c_probe(i2c, addr) ? 1u : 0u;
    }
    if (found == 0u) {
        printf("  no devices\n");
    }
}

/**
 * @brief Initialize the I2C bus pins and scan the bus.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_bus_init(void) {
    i2c_init(PICOKIT_13_DHT11_LCD_I2C, PICOKIT_13_DHT11_LCD_I2C_BAUD);
    gpio_set_function(PICOKIT_13_DHT11_LCD_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PICOKIT_13_DHT11_LCD_I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PICOKIT_13_DHT11_LCD_I2C_SDA);
    gpio_pull_up(PICOKIT_13_DHT11_LCD_I2C_SCL);
    i2c_bus_scan(PICOKIT_13_DHT11_LCD_I2C);
}

/**
 * @brief Reset the I2C handle, sequence, and the sample and transmit timing.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_state_init(void) {
    uint64_t now_us = time_us_64();
    g_i2c = PICOKIT_13_DHT11_LCD_I2C;
    g_i2c_addr = PICOKIT_13_DHT11_LCD_LCD_ADDR;
    memset(&g_reading, 0, sizeof(g_reading));
    g_seq = 0u;
    g_next_read_us = now_us;
    g_next_tx_us = now_us + (uint64_t)PICOKIT_13_DHT11_LCD_TX_INTERVAL_MS * 1000u;
    g_ready = true;
}

/**
 * @brief Derive the telemetry session key from the field secret.
 *
 * LAB-ONLY: production must provision the session key through OTP rather
 * than deriving it from a committed passphrase and salt.
 *
 * @param void No parameters.
 * @return bool true when the session key was derived.
 */
static bool monitor_derive_key(void) {
    bool ok = crypto_kdf_argon2id((const uint8_t *)FIELD_SECRET_PASSPHRASE, strlen(FIELD_SECRET_PASSPHRASE), FIELD_SECRET_SALT, 16u, g_key);
    g_key_ready = ok;
    return ok;
}

/**
 * @brief Print the boot banner for the DHT11 LCD lesson.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_banner(void) {
    printf("=== PICOKIT-13 DHT11 LCD // DHT11 + 1602 LCD + HEARTBEAT ===\n");
}

/**
 * @brief Derive the field key and announce a ready monitor.
 *
 * @param void No parameters.
 * @return bool true when the field key was derived and installed.
 */
static bool monitor_finish(void) {
    bool ok = monitor_derive_key();
    if (ok) {
        monitor_banner();
    }
    return ok;
}

/**
 * @brief Blink the onboard heartbeat LED exactly once.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_heartbeat(void) {
    gpio_put(PICOKIT_13_DHT11_LCD_LED_PIN, 1);
    sleep_us(MONITOR_HEARTBEAT_BLINK_US);
    gpio_put(PICOKIT_13_DHT11_LCD_LED_PIN, 0);
    sleep_us(MONITOR_HEARTBEAT_BLINK_US);
}

/**
 * @brief Print the most recent temperature and humidity reading.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_log_reading(void) {
    printf("DHT t=%d h=%u\n", (int)g_reading.temperature_tenths, (unsigned)g_reading.humidity_tenths);
}

/**
 * @brief Print a DHT11 read failure with its result code.
 *
 * @param rc Sensor result code returned by the one-wire state machine.
 * @return void
 */
static void monitor_log_error(sensor_result_t rc) {
    printf("READ ERR %d\n", (int)rc);
}

/**
 * @brief Render the failed-read LCD lines.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_render_fail(void) {
    dht_reading_t reading;
    memset(&reading, 0, sizeof(reading));
    display_format_lines(&reading, g_seq, false, g_line1, g_line2);
}

/**
 * @brief Render the current reading onto the two LCD line buffers.
 *
 * @param ok True when the reading passed the sensor checksum.
 * @return void
 */
static void monitor_render(bool ok) {
    if (ok) {
        display_format_lines(&g_reading, g_seq, true, g_line1, g_line2);
    } else {
        monitor_render_fail();
    }
    display_render_lines(g_i2c, g_i2c_addr, g_line1, g_line2);
}

/**
 * @brief Log a sample outcome on the console.
 *
 * @param ok True when the reading passed the sensor checksum.
 * @param rc Sensor result code returned by the one-wire state machine.
 * @return void
 */
static void monitor_log_result(bool ok, sensor_result_t rc) {
    if (ok) {
        monitor_log_reading();
    } else {
        monitor_log_error(rc);
    }
}

/**
 * @brief Sample the DHT11, render the LCD, and schedule the next read.
 *
 * @param now_us Current monotonic time in microseconds.
 * @return void
 */
static void monitor_read_tick(uint64_t now_us) {
    sensor_result_t rc = sensor_read(&g_reading);
    bool ok = (rc == SENSOR_RESULT_OK);
    monitor_log_result(ok, rc);
    monitor_render(ok);
    g_next_read_us = now_us + (uint64_t)MONITOR_READ_INTERVAL_MS * 1000u;
}

/**
 * @brief Format the heartbeat JSON body for the latest reading.
 *
 * @param frame Pointer to the mutable frame output buffer.
 * @param frame_len Capacity of the frame output buffer in bytes.
 * @return size_t Number of JSON bytes written, or zero on overflow.
 */
static size_t monitor_build_frame(char *frame, size_t frame_len) {
    int written = snprintf(frame, frame_len, "{\"n\":%u,\"s\":%u,\"t\":%d,\"h\":%u}", (unsigned)PACKET_NODE_ID, (unsigned)g_seq, (int)g_reading.temperature_tenths, (unsigned)g_reading.humidity_tenths);
    return (written > 0 && (size_t)written < frame_len) ? (size_t)written : 0u;
}

/**
 * @brief Seal the current heartbeat body into a hex envelope.
 *
 * @param hex Pointer to the NUL-terminated hex output buffer.
 * @param hex_len Capacity of the hex output buffer in bytes.
 * @return bool true when the heartbeat was sealed and encoded.
 */
static bool monitor_seal_frame(char *hex, size_t hex_len) {
    char frame[PICOKIT_13_DHT11_LCD_FRAME_SIZE];
    uint8_t nonce[ENVELOPE_NONCE_LEN];
    uint8_t ad = (uint8_t)PACKET_NODE_ID;
    size_t frame_len = monitor_build_frame(frame, sizeof(frame));
    envelope_fill_nonce(nonce);
    return envelope_seal_hex(g_key, nonce, &ad, 1u, (const uint8_t *)frame, frame_len, hex, hex_len);
}

/**
 * @brief Build and transmit the authenticated heartbeat frame.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_transmit(void) {
    char hex[ENVELOPE_MAX_HEX_LEN];
    if (!g_key_ready) {
        return;
    }
    if (monitor_seal_frame(hex, sizeof(hex))) {
        radio_send_frame(PICOKIT_13_DHT11_LCD_UART, (const uint8_t *)hex, strlen(hex));
        g_seq += 1u;
    }
}

/**
 * @brief Transmit one heartbeat and schedule the next transmit.
 *
 * @param now_us Current monotonic time in microseconds.
 * @return void
 */
static void monitor_tx_tick(uint64_t now_us) {
    monitor_heartbeat();
    monitor_transmit();
    g_next_tx_us = now_us + (uint64_t)PICOKIT_13_DHT11_LCD_TX_INTERVAL_MS * 1000u;
}

/**
 * @brief Drain inbound radio lines and log every valid +RCV report.
 *
 * @param void No parameters.
 * @return void
 */
static void monitor_rx_tick(void) {
    radio_rcv_t rcv;
    while (radio_line_pump(PICOKIT_13_DHT11_LCD_UART, g_rx_line, &g_rx_len)) {
        if (radio_parse_rcv(g_rx_line, &rcv) == RADIO_RESULT_OK) {
            printf("RX from 0x%04X, %u bytes\n", (unsigned)rcv.sender, (unsigned)rcv.len);
        }
    }
}

/**
 * @brief Service the DHT11 sample and heartbeat transmit timers.
 *
 * @param now_us Current monotonic time in microseconds.
 * @return void
 */
static void monitor_service_timers(uint64_t now_us) {
    if (now_us >= g_next_read_us) {
        monitor_read_tick(now_us);
    }
    if (now_us >= g_next_tx_us) {
        monitor_tx_tick(now_us);
    }
}

/**
 * @brief Initialize the sensor, radio, LEDs, and LCD peripherals.
 *
 * @param void No parameters.
 * @return bool true when every peripheral initialized.
 */
static bool monitor_peripherals_init(void) {
    bool leds = status_led_init();
    bool sensor = sensor_init();
    bool radio = radio_init(PICOKIT_13_DHT11_LCD_UART);
    bool lcd = display_init(PICOKIT_13_DHT11_LCD_I2C, PICOKIT_13_DHT11_LCD_LCD_ADDR);
    return leds && sensor && radio && lcd;
}

bool monitor_init(void) {
    bool ok;
    monitor_bus_init();
    ok = monitor_peripherals_init();
    if (!ok) {
        return false;
    }
    monitor_state_init_io();
    monitor_state_init();
    return monitor_finish();
}

void monitor_deinit(void) {
    g_ready = false;
}

bool monitor_step(void) {
    uint64_t now_us;
    if (!g_ready) {
        return false;
    }
    now_us = time_us_64();
    monitor_service_timers(now_us);
    monitor_rx_tick();
    return true;
}
