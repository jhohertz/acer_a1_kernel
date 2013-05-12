/*
 * MFD driver for Acer A1 AVR microcontroller
 *
 * Based on avr.c by Elay Hu <Elay_Hu@acer.com.tw>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#ifndef __LINUX_MFD_AVR_H
#define __LINUX_MFD_AVR_H

#include <linux/notifier.h>
#include <linux/mfd/core.h>

/* Registers */
#define I2C_REG_FW		0xD0
#define I2C_REG_LED_1		0xD1
/* I2C_REG_LED_2 is unused */
#define I2C_REG_LED_2		0xD2
#define I2C_REG_BKL		0xD3
#define I2C_REG_KEY_STATUS	0xD4
#define I2C_REG_KEY_LOCK	0xD8
#define I2C_REG_LOW_POWER	0xD9
#define I2C_REG_SENSITIVITY	0x60

/* Power */
#define AVR_POWER_NORMAL	0x00
#define AVR_POWER_LOW		0x01

/* Notifiers */
#define AVR_EVENT_IRQ		1
#define AVR_EVENT_EARLYSUSPEND	2
#define AVR_EVENT_LATERESUME	3

struct avr_chip;

struct avr_platform_data {
    int (*platform_init) (void);
    int num_subdevs;
    struct mfd_cell *sub_devices;
};

int avr_write(struct avr_chip* chip, int reg, uint8_t val, int once);
int avr_read(struct avr_chip* chip, uint8_t *val, int once);
int avr_query(struct avr_chip* chip, int reg, uint8_t *val, int once);

int avr_notify_register(struct avr_chip* chip, struct notifier_block *nb);
int avr_notify_unregister(struct avr_chip* chip, struct notifier_block *nb);

int avr_get_firmware_version(struct avr_chip *chip);

#endif // __LINUX_MFD_AVR_H
