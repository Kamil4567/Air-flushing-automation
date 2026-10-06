#include "wokwi-api.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define REG_CONFIG       0x00
#define REG_SHUNT_VOLT   0x01
#define REG_BUS_VOLT     0x02
#define REG_POWER        0x03
#define REG_CURRENT      0x04
#define REG_CALIBRATION  0x05
#define INA219_ADDRESS   0x40

#define BUS_VOLTAGE_V    12.0f
#define SHUNT_OHMS       0.1f
#define WET_RUN_DELAY_MS 12000
#define WET_RISE_MS      1200
#define DRY_RUN_DELAY_MS 100
#define DRY_RISE_MS      600
#define CURRENT_NOISE_MA 3

typedef struct {
    pin_t scl;
    pin_t sda;
    pin_t z2;
    pin_t pump;
    i2c_dev_t i2c;
    uint8_t register_pointer;
    uint16_t config;
    uint16_t calibration;
    uint8_t write_state;
    uint8_t pending_high;
    bool read_high;
    float current_mA;
    int displayed_current;
    uint64_t pump_started_ms;
    uint32_t random_state;
    bool previous_pump_on;
    bool previous_z2_on;
    uint32_t current_attr;
    string_t current_exact_attr;
    uint32_t previous_current;
    char previous_exact[32];
    buffer_t framebuffer;
    uint32_t display_width;
    uint32_t display_height;
} chip_state_t;

static int clamp_int(int value, int min, int max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static uint8_t high_byte(uint16_t value) {
    return (uint8_t)((value >> 8) & 0xff);
}

static uint8_t low_byte(uint16_t value) {
    return (uint8_t)(value & 0xff);
}

static void simulation_timer_callback(void *user_data);

static void get_glyph(
    char c,
    uint8_t glyph[5]
) {

    switch (c) {

        case 'I':
            glyph[0] = 0x1f;
            glyph[1] = 0x04;
            glyph[2] = 0x04;
            glyph[3] = 0x04;
            glyph[4] = 0x1f;
            break;

        case 'N':


                glyph[0] = 0x11;
                glyph[1] = 0x19;
                glyph[2] = 0x15;
                glyph[3] = 0x13;
                glyph[4] = 0x11;
                break;
        case 'A':
            glyph[0] = 0x0e;
            glyph[1] = 0x11;
            glyph[2] = 0x1f;
            glyph[3] = 0x11;
            glyph[4] = 0x11;
            break;

        case 'm':
            glyph[0] = 0x1e;
            glyph[1] = 0x15;
            glyph[2] = 0x15;
            glyph[3] = 0x15;
            glyph[4] = 0x15;
            break;

        case 'a':
            glyph[0] = 0x00;
            glyph[1] = 0x0e;
            glyph[2] = 0x11;
            glyph[3] = 0x1f;
            glyph[4] = 0x10;
            break;

        case '0':
            glyph[0] = 0x0e;
            glyph[1] = 0x11;
            glyph[2] = 0x11;
            glyph[3] = 0x11;
            glyph[4] = 0x0e;
            break;

        case '1':
            glyph[0] = 0x04;
            glyph[1] = 0x0c;
            glyph[2] = 0x04;
            glyph[3] = 0x04;
            glyph[4] = 0x0e;
            break;

        case '2':
            glyph[0] = 0x1e;
            glyph[1] = 0x01;
            glyph[2] = 0x0e;
            glyph[3] = 0x10;
            glyph[4] = 0x1f;
            break;

        case '3':
            glyph[0] = 0x1e;
            glyph[1] = 0x01;
            glyph[2] = 0x0e;
            glyph[3] = 0x01;
            glyph[4] = 0x1e;
            break;

        case '4':
            glyph[0] = 0x12;
            glyph[1] = 0x12;
            glyph[2] = 0x1f;
            glyph[3] = 0x02;
            glyph[4] = 0x02;
            break;

        case '5':
            glyph[0] = 0x1f;
            glyph[1] = 0x10;
            glyph[2] = 0x1e;
            glyph[3] = 0x01;
            glyph[4] = 0x1e;
            break;

        case '6':
            glyph[0] = 0x0e;
            glyph[1] = 0x10;
            glyph[2] = 0x1e;
            glyph[3] = 0x11;
            glyph[4] = 0x0e;
            break;

        case '7':
            glyph[0] = 0x1f;
            glyph[1] = 0x01;
            glyph[2] = 0x02;
            glyph[3] = 0x04;
            glyph[4] = 0x08;
            break;

        case '8':
            glyph[0] = 0x0e;
            glyph[1] = 0x11;
            glyph[2] = 0x0e;
            glyph[3] = 0x11;
            glyph[4] = 0x0e;
            break;

        case '9':
            glyph[0] = 0x0e;
            glyph[1] = 0x11;
            glyph[2] = 0x0f;
            glyph[3] = 0x01;
            glyph[4] = 0x0e;
            break;

        case '.':
            glyph[0] = 0x00;
            glyph[1] = 0x00;
            glyph[2] = 0x00;
            glyph[3] = 0x00;
            glyph[4] = 0x04;
            break;

        case ' ':
            glyph[0] = 0x00;
            glyph[1] = 0x00;
            glyph[2] = 0x00;
            glyph[3] = 0x00;
            glyph[4] = 0x00;
            break;

        default:
            glyph[0] = 0x00;
            glyph[1] = 0x00;
            glyph[2] = 0x00;
            glyph[3] = 0x00;
            glyph[4] = 0x00;
            break;
    }
}


/* ============================================================
 * DISPLAY
 * ============================================================ */

static void set_pixel(
    uint32_t *pixels,
    uint32_t width,
    uint32_t height,
    int x,
    int y,
    uint32_t color
) {

    if (x < 0 || y < 0) {
        return;
    }

    if ((uint32_t)x >= width) {
        return;
    }

    if ((uint32_t)y >= height) {
        return;
    }

    pixels[
        ((uint32_t)y * width) +
        (uint32_t)x
    ] = color;
}


static void clear_display(
    uint32_t *pixels,
    uint32_t width,
    uint32_t height
) {

    for (
        uint32_t i = 0;
        i < width * height;
        i++
    ) {

        pixels[i] = 0xff000000;
    }
}


static void draw_char(
    uint32_t *pixels,
    uint32_t width,
    uint32_t height,
    int x,
    int y,
    char c,
    int scale,
    uint32_t color
 ) {

    uint8_t glyph[5];

    get_glyph(
        c,
        glyph
    );

    for (int row = 0; row < 5; row++) {

        for (int column = 0; column < 5; column++) {

            if (
                glyph[row] &
                (1 << (4 - column))
            ) {

                for (
                    int sx = 0;
                    sx < scale;
                    sx++
                ) {

                    for (
                        int sy = 0;
                        sy < scale;
                        sy++
                    ) {

                        set_pixel(
                            pixels,
                            width,
                            height,
                            x +
                                column * scale +
                                sx,
                            y +
                                row * scale +
                                sy,
                            color
                        );
                    }
                }
            }
        }
    }
}


static void draw_text(
    uint32_t *pixels,
    uint32_t width,
    uint32_t height,
    int x,
    int y,
    const char *text,
    int scale,
    uint32_t color
) {

    while (*text) {

        draw_char(
            pixels,
            width,
            height,
            x,
            y,
            *text,
            scale,
            color
        );

        x += 6 * scale;

        text++;
    }
}


/* ------------------------------------------------------------
 * Update chip display
 * ------------------------------------------------------------ */

static void update_display(
    chip_state_t *chip
) {

    uint32_t width =
        chip->display_width;

    uint32_t height =
        chip->display_height;

    /*
     * Fixed-size framebuffer.
     */
    uint32_t pixels[128 * 64];


    clear_display(
        pixels,
        width,
        height
    );


    const uint32_t WHITE =
        0xffffffff;


    /*
     * Header.
     */
    draw_text(
        pixels,
        width,
        height,
        6,
        5,
        "INA219",
        2,
        WHITE
    );


    /*
     * Current as integer mA.
     */
    int current =
        (int)(
            chip->current_mA + 0.5f
        );


    current =
        clamp_int(
            current,
            0,
            3000
        );


    /*
     * Four digit current.
     *
     * 0    -> 0000
     * 125  -> 0125
     * 1375 -> 1375
     * 3000 -> 3000
     */
    char text[16];

    snprintf(
        text,
        sizeof(text),
        "%04d",
        current
    );


    /*
     * Large number.
     */
    draw_text(
        pixels,
        width,
        height,
        10,
        27,
        text,
        4,
        WHITE
    );


    /*
     * mA.
     */
    draw_text(
        pixels,
        width,
        height,
        105,
        38,
        "mA",
        2,
        WHITE
    );


    /*
     * Current bar.
     */
    const int bar_x = 8;
    const int bar_y = 57;
    const int bar_width = 112;
    const int bar_height = 5;


    int filled =
        (
            current *
            bar_width
        ) / 3000;


    /*
     * Top and bottom.
     */
    for (
        int x = 0;
        x < bar_width;
        x++
    ) {

        set_pixel(
            pixels,
            width,
            height,
            bar_x + x,
            bar_y,
            WHITE
        );

        set_pixel(
            pixels,
            width,
            height,
            bar_x + x,
            bar_y +
                bar_height -
                1,
            WHITE
        );
    }


    /*
     * Left and right.
     */
    for (
        int y = 0;
        y < bar_height;
        y++
    ) {

        set_pixel(
            pixels,
            width,
            height,
            bar_x,
            bar_y + y,
            WHITE
        );

        set_pixel(
            pixels,
            width,
            height,
            bar_x +
                bar_width -
                1,
            bar_y + y,
            WHITE
        );
    }


    /*
     * Filled part.
     */
    if (filled > 2) {

        for (
            int x = 1;
            x < filled - 1;
            x++
        ) {

            for (
                int y = 1;
                y < bar_height - 1;
                y++
            ) {

                set_pixel(
                    pixels,
                    width,
                    height,
                    bar_x + x,
                    bar_y + y,
                    WHITE
                );
            }
        }
    }


    /*
     * Send framebuffer to Wokwi.
     */
    buffer_write(
        chip->framebuffer,
        0,
        pixels,
        sizeof(pixels)
    );
}


static float calculate_pump_current(
    chip_state_t *chip,
    uint64_t elapsed_ms,
    bool z2_on
) {
    const uint32_t delay_ms = z2_on ? WET_RUN_DELAY_MS : DRY_RUN_DELAY_MS;
    const uint32_t rise_ms = z2_on ? WET_RISE_MS : DRY_RISE_MS;
    float rise = 0.0f;

    if (elapsed_ms > delay_ms) {
        rise = (float)(elapsed_ms - delay_ms) / (float)rise_ms;
        if (rise > 1.0f) {
            rise = 1.0f;
        }
    }

    chip->random_state = chip->random_state * 1664525u + 1013904223u;
    const int noise = (int)((chip->random_state >> 16) %
                            (CURRENT_NOISE_MA * 2 + 1)) - CURRENT_NOISE_MA;
    return 300.0f + 200.0f * rise + (float)noise;
}


static void simulation_timer_callback(void *user_data) {
    chip_state_t *chip = (chip_state_t *)user_data;
    const uint64_t now_ms = get_sim_nanos() / 1000000ULL;
    const bool z2_on = pin_read(chip->z2) == HIGH;
    const bool pump_on = pin_read(chip->pump) == HIGH;

    if (pump_on &&
        (!chip->previous_pump_on || z2_on != chip->previous_z2_on)) {
        chip->pump_started_ms = now_ms;
    }

    chip->previous_pump_on = pump_on;
    chip->previous_z2_on = z2_on;
    chip->current_mA = pump_on
                           ? calculate_pump_current(
                                 chip, now_ms - chip->pump_started_ms, z2_on)
                           : 0.0f;

    const int display_current = (int)(chip->current_mA + 0.5f);
    if (display_current != chip->displayed_current) {
        chip->displayed_current = display_current;
        update_display(chip);
    }
}


/* ============================================================
 * EXACT CURRENT INPUT
 * ============================================================ */

static bool read_exact_current(
    chip_state_t *chip,
    float *value
) {

    if (
        chip->current_exact_attr ==
        STRING_NULL
    ) {

        return false;
    }


    uint32_t length =
        string_get_length(
            chip->current_exact_attr
        );


    if (length == 0) {
        return false;
    }


    if (length >= 31) {
        length = 31;
    }


    char buffer[32];

    memset(
        buffer,
        0,
        sizeof(buffer)
    );


    string_read(
        chip->current_exact_attr,
        buffer,
        sizeof(buffer)
    );


    /*
     * Only process changed text.
     */
    if (
        strcmp(
            buffer,
            chip->previous_exact
        ) == 0
    ) {

        return false;
    }


    strcpy(
        chip->previous_exact,
        buffer
    );


    /*
     * Parse digits.
     */
    int parsed = 0;

    bool found_digit = false;


    for (
        uint32_t i = 0;
        i < length;
        i++
    ) {

        char c =
            buffer[i];


        if (
            c >= '0' &&
            c <= '9'
        ) {

            found_digit = true;
            parsed = parsed * 10 + (c - '0');

            if (parsed > 3000) {
                parsed = 3000;
            }
        }
    }


    if (!found_digit) {
        return false;
    }


    parsed =
        clamp_int(
            parsed,
            0,
            3000
        );


    *value =
        (float)parsed;


    return true;
}


/* ============================================================
 * CONTROL UPDATE
 * ============================================================ */

static void update_controls(
    chip_state_t *chip
) {

    bool changed = false;


    /*
     * Range slider.
     *
     * IMPORTANT:
     *
     * attr_read() is the correct API for a
     * custom-chip range control.
     *
     * There is no analog_read() here.
     */
    uint32_t slider_value =
        attr_read(
            chip->current_attr
        );


    if (
        slider_value !=
        chip->previous_current
    ) {

        chip->previous_current =
            slider_value;


        chip->current_mA =
            (float)slider_value;


        changed = true;
    }


    /*
     * Redraw display if necessary.
     */
    if (changed) {

        update_display(
            chip
        );
    }
}


static void controls_timer_callback(
    void *user_data
) {

    update_controls(
        (chip_state_t *)user_data
    );
}


/* ============================================================
 * INA219 REGISTER CALCULATIONS
 * ============================================================ */

static uint16_t get_shunt_voltage_register(
    chip_state_t *chip
) {

    /*
     * INA219 shunt-voltage LSB:
     *
     *     10 uV / bit
     *
     * V = I * R
     *
     * R = 0.1 ohm
     */
    float current_A =
        chip->current_mA /
        1000.0f;


    float shunt_voltage =
        current_A *
        SHUNT_OHMS;


    int raw =
        (int)(
            shunt_voltage /
            0.00001f
        );


    raw =
        clamp_int(
            raw,
            -32768,
            32767
        );


    return (uint16_t)(
        (int16_t)raw
    );
}


static uint16_t get_bus_voltage_register(
    chip_state_t *chip
) {

    (void)chip;


    /*
     * INA219 bus-voltage LSB:
     *
     *     4 mV / bit
     */
    uint16_t raw =
        (uint16_t)(
            BUS_VOLTAGE_V /
            0.004f
        );


    /*
     * Shift into bits 15..3.
     */
    raw <<= 3;


    /*
     * Conversion ready.
     */
    raw |=
        (1 << 1);


    return raw;
}


static uint16_t get_current_register(
    chip_state_t *chip
) {

    /*
     * Current LSB:
     *
     *     0.04096 /
     *     (Calibration * Rshunt)
     *
     * With:
     *
     *     Calibration = 4096
     *     Rshunt      = 0.1 ohm
     *
     * Current LSB =
     *
     *     0.0001 A
     *     = 0.1 mA
     */
    float current_lsb_mA;


    if (
        chip->calibration != 0
    ) {

        current_lsb_mA =
            409.6f /
            (float)chip->calibration;

    } else {

        current_lsb_mA =
            0.1f;
    }


    /*
     * Convert mA to register units.
     */
    float raw_float =
        chip->current_mA /
        current_lsb_mA;


    int raw;


    if (raw_float >= 0.0f) {

        raw =
            (int)(
                raw_float +
                0.5f
            );

    } else {

        raw =
            (int)(
                raw_float -
                0.5f
            );
    }


    raw =
        clamp_int(
            raw,
            -32768,
            32767
        );


    return (uint16_t)(
        (int16_t)raw
    );
}


static uint16_t get_power_register(
    chip_state_t *chip
) {

    /*
     * Power LSB = 2 mW.
     *
     * P = V * I
     */
    float power_mW =
        (
            chip->current_mA /
            1000.0f
        )
        *
        BUS_VOLTAGE_V
        *
        1000.0f;


    int raw =
        (int)(
            power_mW /
            2.0f
        );


    raw =
        clamp_int(
            raw,
            0,
            65535
        );


    return (uint16_t)raw;
}


/* ============================================================
 * REGISTER READ
 * ============================================================ */

static uint16_t read_register(
    chip_state_t *chip,
    uint8_t reg
) {

    switch (reg) {

        case REG_CONFIG:

            return chip->config;


        case REG_SHUNT_VOLT:

            return get_shunt_voltage_register(
                chip
            );


        case REG_BUS_VOLT:

            return get_bus_voltage_register(
                chip
            );


        case REG_POWER:

            return get_power_register(
                chip
            );


        case REG_CURRENT:

            return get_current_register(
                chip
            );


        case REG_CALIBRATION:

            return chip->calibration;


        default:

            return 0;
    }
}


/* ============================================================
 * I2C CONNECT
 * ============================================================ */

static bool i2c_connect(
    void *user_data,
    uint32_t address,
    bool connect
) {

    chip_state_t *chip =
        (chip_state_t *)user_data;


    if (!connect) {
        return true;
    }


    /*
     * Only respond to 0x40.
     */
    if (
        address !=
        INA219_ADDRESS
    ) {

        return false;
    }


    /*
     * New transaction.
     *
     * Keep register_pointer because
     * a register write is normally
     * followed by a repeated-start read.
     */
    chip->write_state = 0;

    chip->read_high = true;


    return true;
}


/* ============================================================
 * I2C WRITE
 * ============================================================ */

static bool i2c_write(
    void *user_data,
    uint8_t data
) {

    chip_state_t *chip =
        (chip_state_t *)user_data;


    /*
     * First byte:
     *
     *     register address
     */
    if (
        chip->write_state == 0
    ) {

        chip->register_pointer =
            data;

        chip->write_state = 1;

        return true;
    }


    /*
     * Second byte:
     *
     *     register high byte
     */
    if (
        chip->write_state == 1
    ) {

        chip->pending_high =
            data;

        chip->write_state = 2;

        return true;
    }


    /*
     * Third byte:
     *
     *     register low byte
     */
    if (
        chip->write_state == 2
    ) {

        uint16_t value =
            (
                (uint16_t)
                chip->pending_high
                << 8
            )
            |
            data;


        switch (
            chip->register_pointer
        ) {

            case REG_CONFIG:

                chip->config =
                    value;

                break;


            case REG_CALIBRATION:

                chip->calibration =
                    value;

                break;


            default:

                /*
                 * Read-only registers.
                 */
                break;
        }


        /*
         * Auto-increment register.
         */
        chip->register_pointer++;


        chip->write_state = 3;


        return true;
    }


    return true;
}


/* ============================================================
 * I2C READ
 * ============================================================ */

static uint8_t i2c_read(
    void *user_data
) {

    chip_state_t *chip =
        (chip_state_t *)user_data;


    /*
     * Get current register value.
     */
    uint16_t value =
        read_register(
            chip,
            chip->register_pointer
        );


    uint8_t result;


    /*
     * INA219 sends MSB first.
     */
    if (
        chip->read_high
    ) {

        result =
            high_byte(value);

        chip->read_high =
            false;

    } else {

        result =
            low_byte(value);

        chip->read_high =
            true;


        /*
         * Advance register after
         * sending both bytes.
         */
        chip->register_pointer++;
    }


    return result;
}


/* ============================================================
 * I2C DISCONNECT
 * ============================================================ */

static void i2c_disconnect(
    void *user_data
) {

    chip_state_t *chip =
        (chip_state_t *)user_data;


    (void)chip;
}


/* ============================================================
 * CHIP INITIALIZATION
 * ============================================================ */

void chip_init(void) {

    /*
     * Allocate state.
     */
    chip_state_t *chip =
        (chip_state_t *)malloc(
            sizeof(chip_state_t)
        );


    if (chip == NULL) {
        return;
    }


    memset(
        chip,
        0,
        sizeof(chip_state_t)
    );


    /* --------------------------------------------------------
     * I2C pins
     * -------------------------------------------------------- */

    chip->scl =
        pin_init(
            "SCL",
            INPUT_PULLUP
        );


    chip->sda =
        pin_init(
            "SDA",
            INPUT_PULLUP
        );


    chip->z2 =
        pin_init(
            "Z2",
            INPUT
        );

    chip->pump =
        pin_init(
            "PUMP",
            INPUT
        );

    chip->current_mA = 0.0f;
    chip->displayed_current = -1;
    chip->pump_started_ms = get_sim_nanos() / 1000000ULL;
    chip->random_state = 0x6D2B79F5u;


    /* --------------------------------------------------------
     * INA219 power-on configuration
     * -------------------------------------------------------- */

    chip->config =
        0x399F;


    /*
     * Calibration used by the simulation.
     */
    chip->calibration =
        4096;


    chip->register_pointer =
        0;


    chip->write_state =
        0;


    chip->read_high =
        true;


    /* --------------------------------------------------------
     * Display
     * -------------------------------------------------------- */

    chip->framebuffer =
        framebuffer_init(
            &chip->display_width,
            &chip->display_height
        );


    /*
     * The .chip.json declares:
     *
     *     128 x 64
     */
    if (
        chip->display_width != 128 ||
        chip->display_height != 64
    ) {

        chip->display_width = 128;
        chip->display_height = 64;
    }


    /* --------------------------------------------------------
     * I2C configuration
     * -------------------------------------------------------- */

    const i2c_config_t i2c_config = {

        .address =
            INA219_ADDRESS,

        .scl =
            chip->scl,

        .sda =
            chip->sda,

        .connect =
            i2c_connect,

        .read =
            i2c_read,

        .write =
            i2c_write,

        .disconnect =
            i2c_disconnect,

        .user_data =
            chip
    };


    chip->i2c =
        i2c_init(
            &i2c_config
        );


    /* --------------------------------------------------------
     * Initial display
     * -------------------------------------------------------- */

    update_display(
        chip
    );


    printf(
        "INA219 initialized at I2C address 0x40\n"
    );


    const timer_config_t timer_config = {

        .callback = simulation_timer_callback,
        .user_data = chip
    };


    timer_t controls_timer =
        timer_init(
            &timer_config
        );


    timer_start(
        controls_timer,
        10000,
        true
    );
}
