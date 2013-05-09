#ifndef __LINUX_MFD_AVR_H
#define __LINUX_MFD_AVR_H

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

/* LCD Backlight */
#define MAX_BACKLIGHT_BRIGHTNESS  255
#define AVR_BKL_MAX_LVL		0x20
#define AVR_BKL_MIN_LVL		0x01
#define AVR_BKL_ON		AVR_BKL_MAX_LVL
#define AVR_BKL_OFF		0x00

/* Kepad LEDs */
#define MAX_LED_BRIGHTNESS	255
#define AVR_LED_MAX_LVL		0x20
#define AVR_LED_ON		AVR_LED_MAX_LVL
#define AVR_LED_OFF		0x00

/* Keypad keycodes */
#define AVR_KEY_MENU              (1<<0)
#define AVR_KEY_LEFT              (1<<1)
#define AVR_KEY_DOWN              (1<<2)
#define AVR_KEY_RIGHT             (1<<3)
#define AVR_KEY_BACK              (1<<4)
#define AVR_KEY_UP                (1<<5)

/* Keypad sensitivity */
#define AVR_SENSITIVITY_DEFAULT   20

int avr_write(struct device* dev, int reg, int val, int once);
int avr_read(struct device* dev, int *val, int once);
int avr_query(struct device* dev, int reg, int *val, int once);

#endif // __LINUX_MFD_AVR_H
