/*
 * LED driver for Acer A1 AVR driven LEDs.
 *
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
#include <linux/jiffies.h>
#include <linux/mfd/avr.h>

#define AVR_LED_DRIVER_NAME "avr-led"

/* LCD Backlight */
#define MAX_BACKLIGHT_BRIGHTNESS  255
#define AVR_BKL_MAX_LVL		0x20
#define AVR_BKL_MIN_LVL		0x01
#define AVR_BKL_ON		AVR_BKL_MAX_LVL
#define AVR_BKL_OFF		0x00

/* Kepad LEDs */
#define MAX_LED_BRIGHTNESS	255
#define NORMAL_LED_BRIGHTNESS	16
#define AVR_LED_MAX_LVL		0x20
#define AVR_LED_ON		AVR_LED_MAX_LVL
#define AVR_LED_OFF		0x00

#define AVR_LED_DELAY_TIME	10000

struct avr_led {
	struct avr_chip *chip;
	struct device *dev;
	struct led_classdev led_bl;
	struct led_classdev led_keypad;
	struct delayed_work work;

	struct notifier_block notifier;
};

static void avr_led_backlight_set(struct led_classdev *led_cdev,
	                         enum led_brightness value)
{
	int avr_brightness;
	struct avr_led *led = container_of(led_cdev, struct avr_led, led_bl);

	avr_brightness = (2 * value * AVR_BKL_MAX_LVL + MAX_BACKLIGHT_BRIGHTNESS)
			/(2 * MAX_BACKLIGHT_BRIGHTNESS);

	if (avr_write(led->chip, I2C_REG_BKL, avr_brightness, 0))
		pr_err("%s: Error setting LCD brightness\n", __func__);

}

static void avr_led_keypad_set(struct led_classdev *led_cdev,
	                         enum led_brightness value)
{
	int avr_brightness;
	struct avr_led *led = container_of(led_cdev, struct avr_led, led_keypad);

	avr_brightness = (2 * value * AVR_LED_MAX_LVL + MAX_LED_BRIGHTNESS)
			/(2 * MAX_LED_BRIGHTNESS);

	if (avr_write(led->chip, I2C_REG_LED_1, avr_brightness, 0))
		pr_err("%s: Error setting keypad brightness\n", __func__);
}

static void avr_led_work_func(struct work_struct *work)
{
	struct delayed_work *led_work = container_of(work, struct delayed_work, work);
	struct avr_led *led = container_of(led_work, struct avr_led, work);

	avr_led_keypad_set(&led->led_keypad, AVR_LED_OFF);
}

static int avr_led_notifier_func(struct notifier_block *nb, unsigned long action, void *dev)
{
	struct avr_led *led = container_of(nb, struct avr_led, notifier);
	if (action == AVR_EVENT_IRQ || action == AVR_EVENT_LATERESUME) {
		cancel_delayed_work(&led->work);
		avr_led_keypad_set(&led->led_keypad, NORMAL_LED_BRIGHTNESS);
		schedule_delayed_work(&led->work, msecs_to_jiffies(AVR_LED_DELAY_TIME));
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

        led->led_keypad.name = "button-backlight";
        led->led_keypad.brightness_set = avr_led_keypad_set;
        led->led_keypad.flags = LED_CORE_SUSPENDRESUME;

        led->led_bl.name = "lcd-backlight";
        led->led_bl.brightness_set = avr_led_backlight_set;
        led->led_bl.flags = LED_CORE_SUSPENDRESUME;

        rc = led_classdev_register(led->dev, &led->led_keypad);
        if (rc < 0)
                goto out_led_keypad;

        rc = led_classdev_register(led->dev, &led->led_bl);
        if (rc < 0)
                goto out_led_bl;

	INIT_DELAYED_WORK(&led->work, avr_led_work_func);

	led->notifier.notifier_call = avr_led_notifier_func;
	avr_notify_register(led->chip, &led->notifier);

	return 0;

	/* led_classdev_unregister(&led->led_bl); */
out_led_bl:
	led_classdev_unregister(&led->led_keypad);

out_led_keypad:
	kfree(led);
	return rc;
}

static int __devexit avr_led_remove(struct platform_device *pdev)
{
	struct avr_led *led = platform_get_drvdata(pdev);

	led_classdev_unregister(&led->led_bl);
	led_classdev_unregister(&led->led_keypad);

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
