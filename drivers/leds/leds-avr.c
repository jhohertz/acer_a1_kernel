/*
 * LED driver for Acer A1 AVR driven LEDs.
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>
#include <linux/mfd/avr.h>

#include "leds.h"

#define AVR_LED_DRIVER_NAME "avr-led"

/* LCD Backlight */
#define MAX_BACKLIGHT_BRIGHTNESS	255
#define DEFAULT_BACKLIGHT_BRIGHTNESS	16
#define AVR_BKL_MAX_LVL		0x20
#define AVR_BKL_MIN_LVL		0x01
#define AVR_BKL_ON		AVR_BKL_MAX_LVL
#define AVR_BKL_OFF		0x00

/* Kepad LEDs */
#define MAX_LED_BRIGHTNESS	255
#define DEFAULT_LED_BRIGHTNESS	16
#define AVR_LED_MAX_LVL		0x20
#define AVR_LED_ON		AVR_LED_MAX_LVL
#define AVR_LED_OFF		0x00

#define AVR_LED_DELAY_TIME	10000

#define AVR_CHANGE_FLAG		(1 << 8)

struct mutex avr_led_mutex;

struct avr_led {
	struct avr_chip *chip;
	struct device *dev;
	struct led_classdev bl;
	struct led_classdev keypad;

	int bl_brightness;
	int keypad_brightness;

	struct work_struct work;

	struct notifier_block notifier;
};

static void avr_led_work(struct work_struct *work)
{
	struct avr_led *led = container_of(work, struct avr_led, work);
	int avr_brightness;
	int value;

	mutex_lock(&avr_led_mutex);

	dev_dbg(led->dev, "%s: called\n", __func__);

	/* LCD */
	if (led->bl_brightness & AVR_CHANGE_FLAG) {
	    value = led->bl_brightness ^ AVR_CHANGE_FLAG;

	    avr_brightness = (2 * value * AVR_BKL_MAX_LVL + MAX_BACKLIGHT_BRIGHTNESS)
			    /(2 * MAX_BACKLIGHT_BRIGHTNESS);

	    if (avr_write(led->chip, I2C_REG_BKL, avr_brightness, 0))
		    pr_err("%s: Error setting LCD brightness\n", __func__);

	    led->bl_brightness = value;
	}

	/* Keypad */
	if (led->keypad_brightness & AVR_CHANGE_FLAG) {
	    value = led->keypad_brightness ^ AVR_CHANGE_FLAG;

	    avr_brightness = (2 * value * AVR_LED_MAX_LVL + MAX_LED_BRIGHTNESS)
			    /(2 * MAX_LED_BRIGHTNESS);

	    if (avr_write(led->chip, I2C_REG_LED_1, avr_brightness, 0))
		    pr_err("%s: Error setting keypad brightness\n", __func__);

	    led->keypad_brightness = value;
	}

	mutex_unlock(&avr_led_mutex);
}

static void avr_led_backlight_set(struct led_classdev *led_cdev,
	                         enum led_brightness value)
{
	struct avr_led *led = container_of(led_cdev, struct avr_led, bl);
	led->bl_brightness = value | AVR_CHANGE_FLAG;

	schedule_work(&led->work);
}

static enum led_brightness avr_led_backlight_get(struct led_classdev *led_cdev)
{
	struct avr_led *led = container_of(led_cdev, struct avr_led, bl);
	return led->bl_brightness & AVR_CHANGE_FLAG ? led->bl_brightness ^ AVR_CHANGE_FLAG
						    : led->bl_brightness;
}

static void avr_led_keypad_set(struct led_classdev *led_cdev,
	                         enum led_brightness value)
{
	struct avr_led *led = container_of(led_cdev, struct avr_led, keypad);
	led->keypad_brightness = value | AVR_CHANGE_FLAG;

	schedule_work(&led->work);
}

static enum led_brightness avr_led_keypad_get(struct led_classdev *led_cdev)
{
	struct avr_led *led = container_of(led_cdev, struct avr_led, keypad);
	return led->keypad_brightness & AVR_CHANGE_FLAG ? led->keypad_brightness ^ AVR_CHANGE_FLAG
							: led->keypad_brightness;
}

static int avr_led_notifier_func(struct notifier_block *nb, unsigned long event, void *dev)
{
	struct avr_led *led = container_of(nb, struct avr_led, notifier);

	dev_dbg(led->dev, "%s: received event %ld\n", __func__, event);

	if (event == AVR_EVENT_LATERESUME) {
		led_classdev_resume(&led->bl);
		led_classdev_resume(&led->keypad);
	} else if (event == AVR_EVENT_EARLYSUSPEND) {
		led_classdev_suspend(&led->bl);
		led_classdev_suspend(&led->keypad);
	}

	return NOTIFY_OK;
}

static int avr_led_probe(struct platform_device *pdev)
{
	struct avr_led *led;
	struct avr_chip *chip;

	int rc;

	chip = platform_get_drvdata(pdev);
	if (chip == NULL) {
		dev_err(&pdev->dev, "no parent data passed in\n");
		return -EFAULT;
	}

	led = kzalloc(sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	platform_set_drvdata(pdev, led);

	led->dev	= &pdev->dev;
	led->chip	= chip;

        led->keypad.name = "button-backlight";
        led->keypad.brightness_set = avr_led_keypad_set;
        led->keypad.brightness_get = avr_led_keypad_get;
        led->keypad.flags = LED_CORE_SUSPENDRESUME;

        led->bl.name = "lcd-backlight";
        led->bl.brightness_set = avr_led_backlight_set;
        led->bl.brightness_get = avr_led_backlight_get;
        led->bl.flags = LED_CORE_SUSPENDRESUME;

        rc = led_classdev_register(led->dev, &led->keypad);
        if (rc < 0)
                goto out_led_keypad;

        rc = led_classdev_register(led->dev, &led->bl);
        if (rc < 0)
                goto out_led_bl;

	led->notifier.notifier_call = avr_led_notifier_func;
	avr_notify_register(led->chip, &led->notifier);

	INIT_WORK(&led->work, avr_led_work);

	mutex_init(&avr_led_mutex);

	led_set_brightness(&led->bl, DEFAULT_BACKLIGHT_BRIGHTNESS);
	led_set_brightness(&led->keypad, DEFAULT_LED_BRIGHTNESS);

	dev_info(led->dev, "%s: initialized\n", __func__);

	return 0;

	/* led_classdev_unregister(&led->led_bl); */
out_led_bl:
	led_classdev_unregister(&led->keypad);

out_led_keypad:
	kfree(led);
	return rc;
}

static int __devexit avr_led_remove(struct platform_device *pdev)
{
	struct avr_led *led = platform_get_drvdata(pdev);

	led_classdev_unregister(&led->bl);
	led_classdev_unregister(&led->keypad);

	kfree(led);
	return 0;
}

#ifdef CONFIG_PM
static int avr_led_suspend(struct platform_device *pdev, pm_message_t msg)
{
	return 0;
}

static int avr_led_resume(struct platform_device *pdev)
{
	return 0;
}
#else
#define avr_led_suspend	NULL
#define avr_led_resume	NULL
#endif // CONFIG_PM

static struct platform_driver avr_led_driver = {
	.probe		= avr_led_probe,
	.remove		= __devexit_p(avr_led_remove),
	.suspend	= avr_led_suspend,
	.resume		= avr_led_resume,
	.driver		= {
		.name = AVR_LED_DRIVER_NAME,
		.owner = THIS_MODULE,
	},
};

static int __init avr_led_init(void)
{
	return platform_driver_register(&avr_led_driver);
}
module_init(avr_led_init);

static void __exit avr_led_exit(void)
{
	return platform_driver_unregister(&avr_led_driver);
}
module_exit(avr_led_exit);

MODULE_AUTHOR("Roman Yepishev");
MODULE_DESCRIPTION("AVR LED driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:avr-leds");
