/*
 * MFD driver for Acer A1 AVR microcontroller
 *
 * Based on avr.c by Elay Hu <Elay_Hu@acer.com.tw>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/notifier.h>
#include <linux/earlysuspend.h>
#include <linux/mfd/core.h>
#include <linux/mfd/avr.h>
#include <linux/err.h>

#define DEV_IOCTLID		0x11
#define IOC_MAXNR		18
#define IOCTL_SET_BL_ON		_IOW(DEV_IOCTLID, 1, int)
#define IOCTL_SET_BL_OFF	_IOW(DEV_IOCTLID, 2, int)
#define IOCTL_SET_BL_LV		_IOW(DEV_IOCTLID, 3, int)
#define IOCTL_SET_LED1_ON	_IOW(DEV_IOCTLID, 4, int)
#define IOCTL_SET_LED1_OFF	_IOW(DEV_IOCTLID, 5, int)
#define IOCTL_SET_LED2_ON	_IOW(DEV_IOCTLID, 6, int)
#define IOCTL_SET_LED2_OFF	_IOW(DEV_IOCTLID, 7, int)
#define IOCTL_KEY_LOCK_TOGGLE	_IOW(DEV_IOCTLID, 8, int)
#define IOCTL_SET_LED_ON	_IOW(DEV_IOCTLID, 10, int)
#define IOCTL_SET_LED_OFF	_IOW(DEV_IOCTLID, 11, int)

#define AVR_DRIVER_NAME           "avr"

#define AVR_LED_DELAY_TIME        10000

#define AVR_I2C_RETRY_COUNT	5

static int __init avr_init(void);
static int avr_probe(struct i2c_client *client, const struct i2c_device_id *id);
static int avr_remove(struct i2c_client *client);
static int avr_suspend(struct i2c_client *client, pm_message_t mesg);
static int avr_resume(struct i2c_client *client);
static irqreturn_t avr_interrupt(int irq, void *dev_id);
static void avr_irq_work_func(struct work_struct *work);
static void avr_set_power_mode(struct avr_chip *chip, int mode);

#ifdef CONFIG_HAS_EARLYSUSPEND
static void avr_early_suspend(struct early_suspend *h);
static void avr_late_resume(struct early_suspend *h);
#endif

static struct mutex avr_query_mutex;

static int __avr_write(struct avr_chip* client, int reg, uint8_t val, int once);
static int __avr_read(struct avr_chip* client, uint8_t *val, int once);
static int __avr_query(struct avr_chip* client, int reg, uint8_t *val, int once);

static const struct i2c_device_id avr_id[] = {
	{ AVR_DRIVER_NAME, 0 },
	{ }
};

/* Data for I2C driver */
struct avr_chip {
	struct avr_platform_data pdata;
	struct i2c_client *client;
	struct device *dev;
	uint8_t firmware_version;
	struct work_struct irq_work;

	wait_queue_head_t wait;

	/* For IRQ / suspend notifications */
	struct blocking_notifier_head notifier_list;

#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
};

/* new style I2C driver struct */
static struct i2c_driver avr_driver = {
	.probe     = avr_probe,
	.remove    = avr_remove,
	.id_table  = avr_id,
	.suspend   = avr_suspend,
	.resume    = avr_resume,
	.driver    = {
		.name      = AVR_DRIVER_NAME,
	},
};

static int __avr_write(struct avr_chip* chip, int reg, uint8_t val, int once)
{
	int res = -1;
	int retry = AVR_I2C_RETRY_COUNT;
	struct i2c_client *client = chip->client;

	uint8_t buf[2] = { (uint8_t)reg, (uint8_t)val };
	int count = (val == -1) ? 1 : 2;

	while (retry-- > 0) {
		if (count == i2c_master_send(client, buf, count )) {
			dev_dbg(chip->dev, "%s: < %02X %02X - OK\n", __func__,
				buf[0], buf[1]);
			res = 0;
			break;
		}

		if (once)
			break;

		dev_dbg(chip->dev, "%s: < %02X %02X - FAIL\n", __func__,
			buf[0], buf[1]);

		msleep(200);
	}

	return res;
}

static int __avr_read(struct avr_chip *chip, uint8_t *val, int once)
{
	int res = -1;
	int retry = AVR_I2C_RETRY_COUNT;
	struct i2c_client *client = chip->client;

	while (retry-- > 0) {
		if (1 == i2c_master_recv(client, val, 1)) {
			dev_dbg(chip->dev, "%s: > %02X - OK\n", __func__,
				*val);
			res = 0;
			break;
		}

		if (once)
			break;

		dev_dbg(chip->dev, "%s: <- FAIL\n", __func__);

		msleep(200);
	}

	return res;
}

static int __avr_query(struct avr_chip *chip, int reg, uint8_t *val, int once)
{
	int rc = -1;
	mutex_lock(&avr_query_mutex);
	if (__avr_write(chip, reg, -1, once))
		goto out;
	if (__avr_read(chip, val, once))
		goto out;
	rc = 0;
out:
	mutex_unlock(&avr_query_mutex);
	return rc;
}

int avr_write(struct avr_chip *chip, int reg, uint8_t val, int once)
{
	return __avr_write(chip, reg, val, once);
}
EXPORT_SYMBOL_GPL(avr_write);

int avr_read(struct avr_chip *chip, uint8_t *val, int once)
{
	return __avr_read(chip, val, once);
}
EXPORT_SYMBOL_GPL(avr_read);

int avr_query(struct avr_chip *chip, int reg, uint8_t *val, int once)
{
	return __avr_query(chip, reg, val, once);
}
EXPORT_SYMBOL_GPL(avr_query);

int avr_notify_register(struct avr_chip *chip, struct notifier_block *nb)
{
	if (blocking_notifier_chain_register(&chip->notifier_list, nb)) {
	      dev_err(chip->dev, "%s: failed to register notifier\n", __func__);
	      return -1;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(avr_notify_register);

int avr_notify_unregister(struct avr_chip *chip, struct notifier_block *nb)
{
	if (blocking_notifier_chain_unregister(&chip->notifier_list, nb)) {
	      dev_err(chip->dev, "%s: failed to unregister notifier\n", __func__);
	      return -1;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(avr_notify_unregister);

int avr_get_firmware_version(struct avr_chip *chip)
{
	return chip->firmware_version;
}
EXPORT_SYMBOL_GPL(avr_get_firmware_version);

static void avr_set_power_mode(struct avr_chip *chip, int mode)
{
	dev_dbg(chip->dev, "%s: setting mode %s\n", __func__,
		mode == AVR_POWER_LOW ? "AVR_POWER_LOW" : "AVR_POWER_NORMAL");

	if (avr_write(chip, I2C_REG_LOW_POWER, mode, 0))
		dev_err(chip->dev, "%s: error setting mode", __func__);
}

static int __init avr_init(void)
{
	int res=0;

	res = i2c_add_driver(&avr_driver);

	if (res)
		pr_err("%s: i2c_add_driver failed\n", __func__);

	return res;
}

static void avr_irq_work_func(struct work_struct *work)
{
	struct avr_chip *chip = container_of(work, struct avr_chip, irq_work);
	dev_dbg(chip->dev, "%s: calling call chain with AVR_EVENT_IRQ\n",
				  __func__);
	blocking_notifier_call_chain(&chip->notifier_list, AVR_EVENT_IRQ, NULL);
	dev_dbg(chip->dev, "%s: finished calling chain with AVR_EVENT_IRQ\n",
				  __func__);
}

static int avr_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int rc = 0;
	int i;
	struct avr_chip *chip;
	struct avr_platform_data *pdata = client->dev.platform_data;

	dev_dbg(&client->dev, "%s: probing\n", __func__);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "%s: i2c_check_functionality failed\n",
			__func__);
		return -ENOTSUPP;
	}

	if (pdata->platform_init != NULL) {
		rc = pdata->platform_init();
		if (rc != 0) {
			dev_err(&client->dev, "%s: platform init failed\n",
				__func__);
			goto error;
		}
	}

	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (IS_ERR(chip)) {
		return -ENOMEM;
	}

	chip->client = client;
	chip->dev = &client->dev;

	strlcpy(client->name, AVR_DRIVER_NAME, I2C_NAME_SIZE);
	i2c_set_clientdata(client, chip);

	mutex_init(&avr_query_mutex);

	INIT_WORK(&chip->irq_work, avr_irq_work_func);
	init_waitqueue_head(&chip->wait);

	BLOCKING_INIT_NOTIFIER_HEAD(&chip->notifier_list);

	if (client->irq) {
		rc = request_irq(client->irq, avr_interrupt,
				  IRQF_TRIGGER_FALLING,
				  AVR_DRIVER_NAME, chip);
		if (rc < 0) {
			dev_err(chip->dev, "%s: request_irq failed\n", __func__);
		}
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	chip->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN;
	chip->early_suspend.suspend = avr_early_suspend;
	chip->early_suspend.resume = avr_late_resume;
	register_early_suspend(&chip->early_suspend);
#endif

	if (avr_query(chip, I2C_REG_FW,
		      &chip->firmware_version, 0)) {
		dev_err(chip->dev,"%s: firmware query failed\n", __func__);
		goto error_avr_dev;
	}

	for (i = 0; i < pdata->num_subdevs; i++)
		pdata->sub_devices[i].driver_data = chip;	
	rc = mfd_add_devices(chip->dev, -1, pdata->sub_devices,
			     pdata->num_subdevs, NULL, 0);
	if (rc) {
		dev_err(chip->dev, "%s: mfd_add_devices failed\n", __func__);
		goto error_avr_dev;
	}

	dev_info(chip->dev, "%s: avr intialized (FW 0x%x)\n", __func__,
			chip->firmware_version);
	return 0;

error_avr_dev:
#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&chip->early_suspend);
#endif
	free_irq(client->irq, chip);
	kfree(chip);
error:
	i2c_set_clientdata(client, NULL);
	dev_err(&client->dev, "%s: failed\n", __func__);
	return rc;
}

static int avr_remove(struct i2c_client *client)
{
	struct avr_chip *data = i2c_get_clientdata(client);
	free_irq(client->irq, data);

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&data->early_suspend);
#endif

	mfd_remove_devices(&client->dev);

	i2c_set_clientdata(client, NULL);

	kfree(data);
	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void avr_early_suspend(struct early_suspend *h)
{
	struct avr_chip *chip = container_of(h, struct avr_chip, early_suspend);

	blocking_notifier_call_chain(&chip->notifier_list, AVR_EVENT_EARLYSUSPEND, NULL);

	disable_irq(chip->client->irq);
	avr_set_power_mode(chip, AVR_POWER_LOW);
}

static void avr_late_resume(struct early_suspend *h)
{
	struct avr_chip *chip = container_of(h, struct avr_chip, early_suspend);

	avr_set_power_mode(chip, AVR_POWER_NORMAL);
	enable_irq(chip->client->irq);

	blocking_notifier_call_chain(&chip->notifier_list, AVR_EVENT_LATERESUME, NULL);
}
#endif

static int avr_suspend(struct i2c_client *client, pm_message_t mesg)
{
	return 0;
}

static int avr_resume(struct i2c_client *client)
{
	return 0;
}

static irqreturn_t avr_interrupt(int irq, void *data)
{
	disable_irq(irq);
	schedule_work(&((struct avr_chip *)data)->irq_work);
	enable_irq(irq);
	return IRQ_HANDLED;
}

static void __exit avr_exit(void)
{
	i2c_del_driver(&avr_driver);
}

module_init(avr_init);
module_exit(avr_exit);

MODULE_AUTHOR("Roman Yepishev");
MODULE_DESCRIPTION("AVR micro-P driver");
