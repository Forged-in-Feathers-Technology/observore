#pragma once

/* The ST77961 initialisation sequence for the Elecrow CrowPanel 1.46.
 *
 * ## Where this came from, and why it is not a JD9855 table
 *
 * Elecrow's product page says the controller is a JD9855. Their own working
 * example says otherwise: the LovyanGFX device in
 * example/V1.0/Arduino/RotaryScreen_1_46_Code_Core3_LVGL9/RotaryScreen_1_46.h
 * instantiates lgfx::Panel_ST77961, and these are the commands that panel
 * class sends. Code that drives the glass beats a comparison table, so this
 * follows the code.
 *
 * Converted mechanically from LovyanGFX's getInitCommands() rather than typed
 * out, because forty-five commands and two hundred data bytes is more than
 * anybody transcribes correctly, and a single wrong gamma byte produces a
 * picture that looks almost right.
 *
 * ## Why it is fed to the ST77916 driver
 *
 * There is no esp_lcd_st77961 -- not in ESP-IDF, not in the component
 * registry. espressif/esp_lcd_st77916 is used instead, and that is a
 * deliberate borrowing rather than a mistake to fix later: everything the
 * driver does after initialisation is the standard MIPI set, and this panel
 * answers 0x2A, 0x2B, 0x2C, 0x36 and 0x3A exactly as that driver expects.
 * The chip-specific part of a panel driver is its initialisation, and that
 * is the part this file replaces. The component also takes plain SPI with a
 * D/C pin -- use_qspi_interface defaults to 0 -- which is how this board is
 * wired, unlike the 1.85.
 *
 * Note 0x3A 0x05: sixteen bits a pixel, which is what the rest of this
 * project assumes. And 0x2A/0x2B set 0x0000 to 0x0167, which is 0 to 359 in
 * both axes -- so the addressable area is the full 360x360 with no offset,
 * unlike the round AMOLED where the controller's memory is wider than the
 * glass.
 */

#include "esp_lcd_st77916.h"

static const uint8_t observore_st77961_d00[] = {0x00};
static const uint8_t observore_st77961_d01[] = {0x98, 0x55};
static const uint8_t observore_st77961_d02[] = {0x1F};
static const uint8_t observore_st77961_d03[] = {0x00, 0x1D, 0x00, 0x45};
static const uint8_t observore_st77961_d04[] = {0x1B, 0x64, 0xC4, 0x1E, 0x3E, 0xF5};
static const uint8_t observore_st77961_d05[] = {0x03, 0x20, 0xF3, 0xC0};
static const uint8_t observore_st77961_d06[] = {0x22, 0xA1};
static const uint8_t observore_st77961_d07[] = {0x00, 0x02, 0x2A, 0x0B, 0x08, 0x48, 0x08, 0x04, 0x62, 0x30, 0x30};
static const uint8_t observore_st77961_d08[] = {0x40, 0x00, 0xAD, 0x68, 0x43, 0x07, 0x04, 0x16, 0x43, 0x07, 0x04};
static const uint8_t observore_st77961_d09[] = {0x3F, 0x36, 0x30, 0x2D, 0x2F, 0x31, 0x2B, 0x2A, 0x27, 0x25, 0x1F, 0x14, 0x10, 0x09, 0x04, 0x00, 0x3F, 0x36, 0x30, 0x2D, 0x2F, 0x31, 0x2B, 0x2A, 0x27, 0x25, 0x1F, 0x14, 0x10, 0x09, 0x04, 0x00};
static const uint8_t observore_st77961_d10[] = {0x28, 0x13};
static const uint8_t observore_st77961_d11[] = {0x00, 0x00, 0xFF, 0x00, 0xF0, 0x00};
static const uint8_t observore_st77961_d12[] = {0x01};
static const uint8_t observore_st77961_d13[] = {0x13, 0xE7, 0x64, 0x39, 0x06, 0x36, 0x19, 0x1C};
static const uint8_t observore_st77961_d14[] = {0x00};
static const uint8_t observore_st77961_d15[] = {0x00, 0x4A, 0x80};
static const uint8_t observore_st77961_d16[] = {0x00, 0x16, 0xDA, 0xE7};
static const uint8_t observore_st77961_d17[] = {0x00, 0x00, 0x00, 0x38, 0x08, 0x08, 0x00, 0x01};
static const uint8_t observore_st77961_d18[] = {0x00, 0x00, 0x00, 0x00, 0x15, 0x3B};
static const uint8_t observore_st77961_d19[] = {0x00, 0x16, 0x06, 0x04, 0x0A};
static const uint8_t observore_st77961_d20[] = {0x08, 0x35, 0x16, 0x1F, 0x1F};
static const uint8_t observore_st77961_d21[] = {0x01, 0x16, 0x07, 0x05, 0x0B};
static const uint8_t observore_st77961_d22[] = {0x09, 0x35, 0x16, 0x1F, 0x1F};
static const uint8_t observore_st77961_d23[] = {0x01, 0x16, 0x09, 0x0B, 0x05};
static const uint8_t observore_st77961_d24[] = {0x07, 0x15, 0x1F, 0x16, 0x1F};
static const uint8_t observore_st77961_d25[] = {0x00, 0x16, 0x08, 0x0A, 0x04};
static const uint8_t observore_st77961_d26[] = {0x06, 0x15, 0x1F, 0x16, 0x1F};
static const uint8_t observore_st77961_d27[] = {0x02, 0x30};
static const uint8_t observore_st77961_d28[] = {0x02, 0x03, 0x52, 0xDF, 0xDD};
static const uint8_t observore_st77961_d29[] = {0x3B, 0x04, 0x48};
static const uint8_t observore_st77961_d30[] = {0x10, 0x10, 0x07, 0x07, 0x0F, 0x94, 0x26};
static const uint8_t observore_st77961_d31[] = {0x00, 0x00, 0x40};
static const uint8_t observore_st77961_d32[] = {0x00, 0x00, 0x20};
static const uint8_t observore_st77961_d33[] = {0x02};
static const uint8_t observore_st77961_d34[] = {0x1C};
static const uint8_t observore_st77961_d35[] = {0x00};
static const uint8_t observore_st77961_d36[] = {0x00};
static const uint8_t observore_st77961_d37[] = {0x00};
static const uint8_t observore_st77961_d38[] = {0x00};
static const uint8_t observore_st77961_d39[] = {0x01};
static const uint8_t observore_st77961_d40[] = {0x00};
static const uint8_t observore_st77961_d42[] = {0x05};
static const uint8_t observore_st77961_d43[] = {0x00, 0x00, 0x01, 0x67};
static const uint8_t observore_st77961_d44[] = {0x00, 0x00, 0x01, 0x67};

static const st77916_lcd_init_cmd_t OBSERVORE_ST77961_INIT[] = {
    {0xDE, observore_st77961_d00, 1, 0},
    {0xDF, observore_st77961_d01, 2, 0},
    {0xB2, observore_st77961_d02, 1, 0},
    {0xB7, observore_st77961_d03, 4, 0},
    {0xBB, observore_st77961_d04, 6, 0},
    {0xBC, observore_st77961_d05, 4, 0},
    {0xC0, observore_st77961_d06, 2, 0},
    {0xC3, observore_st77961_d07, 11, 0},
    {0xC4, observore_st77961_d08, 11, 0},
    {0xC8, observore_st77961_d09, 32, 0},
    {0xD3, observore_st77961_d10, 2, 0},
    {0xD9, observore_st77961_d11, 6, 0},
    {0xDE, observore_st77961_d12, 1, 0},
    {0xB7, observore_st77961_d13, 8, 0},
    {0xBE, observore_st77961_d14, 1, 0},
    {0xC1, observore_st77961_d15, 3, 0},
    {0xC2, observore_st77961_d16, 4, 0},
    {0xC7, observore_st77961_d17, 8, 0},
    {0xC8, observore_st77961_d18, 6, 0},
    {0xC9, observore_st77961_d19, 5, 0},
    {0xCA, observore_st77961_d20, 5, 0},
    {0xCB, observore_st77961_d21, 5, 0},
    {0xCC, observore_st77961_d22, 5, 0},
    {0xCD, observore_st77961_d23, 5, 0},
    {0xCE, observore_st77961_d24, 5, 0},
    {0xCF, observore_st77961_d25, 5, 0},
    {0xD0, observore_st77961_d26, 5, 0},
    {0xD1, observore_st77961_d27, 2, 0},
    {0xD2, observore_st77961_d28, 5, 0},
    {0xD3, observore_st77961_d29, 3, 0},
    {0xD5, observore_st77961_d30, 7, 0},
    {0xD6, observore_st77961_d31, 3, 0},
    {0xD7, observore_st77961_d32, 3, 0},
    {0xDE, observore_st77961_d33, 1, 0},
    {0xB6, observore_st77961_d34, 1, 0},
    {0xDE, observore_st77961_d35, 1, 0},
    {0x4D, observore_st77961_d36, 1, 0},
    {0x4E, observore_st77961_d37, 1, 0},
    {0x4F, observore_st77961_d38, 1, 0},
    {0x4C, observore_st77961_d39, 1, 10},
    {0x4C, observore_st77961_d40, 1, 0},
    {0x35, NULL, 0, 0},
    {0x3A, observore_st77961_d42, 1, 0},
    {0x2A, observore_st77961_d43, 4, 0},
    {0x2B, observore_st77961_d44, 4, 0},
    {0x11, NULL, 0, 120},
    {0x29, NULL, 0, 0},
};
