/*
 * Input driver for Acer A1 AVR driven capacitive keypad.
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

#include <linux/init.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/notifier.h>
#include <linux/mfd/avr.h>

struct mutex avr_keypad_lock;

/* Keypad keycodes */
#define AVR_KEY_MENU		(1<<0)
#define AVR_KEY_LEFT		(1<<1)
#define AVR_KEY_DOWN		(1<<2)
#define AVR_KEY_RIGHT		(1<<3)
#define AVR_KEY_BACK		(1<<4)
#define AVR_KEY_UP		(1<<5)

/* Keypad sensitivity */
#define AVR_KEYPAD_THRESHOLD_DEFAULT	20

#define AVR_KEYPAD_THRESHOLD_MIN	0
#define AVR_KEYPAD_THRESHOLD_MAX	42

#define AVR_KEYPAD_DRIVER_NAME	"avr-keypad"

enum layout_type_t {
	/*
	 * AVR_KEY_MENU - KEY_HOME
	 * AVR_KEY_LEFT - SEARCH
	 * AVR_KEY_RIGHT - BACK
	 * AVR_KEY_BACK - KEY_KBDILLUMDOWN (0xE5) mapped to MENU
	 */
	AVR_LAYOUT_TYPE_A,
	/* 
	 * AVR_KEY_MENU	- KEY_HOME
	 * AVR_KEY_LEFT - KEY_SEARCH.
	 * AVR_KEY_DOWN - KEY_BACK
	 * AVR_KEY_RIGHT - KEY_KBDILLUMDOWN (0xE5) mapped to MENU
	 */
	AVR_LAYOUT_TYPE_C,
};

struct avr_keypad {
	struct input_dev	*input;
	struct device		*dev;
	struct avr_chip		*chip;
	enum layout_type_t	layout_type;
	struct notifier_block   notifier;

	bool	sensitivity_cap; 
	uint8_t threshold;

	int last_key;
};

int avr_keypad_threshold_update(struct avr_keypad *keypad,
				  int value)
{
	if (!keypad->sensitivity_cap)
		return 0;

	if (value < AVR_KEYPAD_THRESHOLD_MIN ||
	    value > AVR_KEYPAD_THRESHOLD_MAX) {
		dev_err(keypad->dev, "%s: value %d out of range %d-%d\n",
			__func__, value, AVR_KEYPAD_THRESHOLD_MIN,
			AVR_KEYPAD_THRESHOLD_MAX);
		return -1;
	}

	if ( 0 != avr_write(keypad->chip, I2C_REG_SENSITIVITY, (uint8_t)value, 0)) {
		pr_err("%s: AVR threshold value error\n", __func__);
		return -1;
	}

	keypad->threshold = (uint8_t)value;
	
	return 0;
}

static ssize_t avr_keypad_threshold_set(struct device *device,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	int res;
	long value;
	struct avr_keypad *keypad = dev_get_drvdata(device);

	res = strict_strtol(buf, 10, &value);
	if (res) {
	    dev_err(keypad->dev, "%s: invalid value %s\n", __func__, buf);
	    value = AVR_KEYPAD_THRESHOLD_DEFAULT; 
	}

	if (avr_keypad_threshold_update(keypad, value)) {
		dev_err(keypad->dev, "%s: could not update AVR keypad threshold\n",
			__func__);
		return -1;
	}

	return count;
}

static ssize_t avr_keypad_threshold_get(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct avr_keypad *keypad = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n", keypad->threshold);
}

static struct device_attribute avr_keypad_attrs = __ATTR(threshold, S_IRWXUGO,
		avr_keypad_threshold_get,
		avr_keypad_threshold_set);

static void avr_keypad_key_clear(struct avr_keypad *keypad)
{
	/* To prevent uP holding KEY_INT pin to low without getting value */
	uint8_t dummy;
	avr_query(keypad->chip, I2C_REG_KEY_STATUS, &dummy, 0);
}

static int avr_keypad_notifier_func(struct notifier_block *nb, unsigned long event, void *data)
{
	struct avr_keypad *keypad = container_of(nb, struct avr_keypad, notifier);
	uint8_t avr_key = 0;
	int key_code = 0;
	int count = 0;
	int rc = -1;

	if (event != AVR_EVENT_IRQ)
		return NOTIFY_OK;

	mutex_lock(&avr_keypad_lock);

	/* Step 1. Scan Key */
	while (count < 5) {
		if (avr_query(keypad->chip, I2C_REG_KEY_STATUS, &avr_key, 1)) {
			if (count == 1) {
				input_report_key(keypad->input, keypad->last_key, 0);
				keypad->last_key = key_code;
				goto out;
			}
			msleep(200);
		}
		else {
			rc = 0;
			break;
		}
		count++;
	}

	/* Step 2. Send Key event */
	if(rc == -1 || count > 1){
		avr_keypad_key_clear(keypad);
		goto out;
	}

	switch(avr_key){
	case AVR_KEY_MENU:
		key_code = KEY_HOME;
		break;
	case AVR_KEY_LEFT:
		key_code = KEY_SEARCH;
		break;
	case AVR_KEY_RIGHT:
		if (keypad->layout_type == AVR_LAYOUT_TYPE_C)
		    key_code = 0xE5; /* MENU */
		else
		    key_code = KEY_BACK;
		break;
	case AVR_KEY_DOWN:
		key_code = KEY_BACK;
		break;
	case AVR_KEY_BACK:
		key_code = 0xE5;
		break;
	default:
		key_code = 0;
		break;
	}

	pr_debug("%s: key_st=0x%x, key_code=0x%x, pre=0x%x\n",
             __func__, avr_key, key_code, keypad->last_key);

	if (key_code != keypad->last_key){
		input_report_key(keypad->input, keypad->last_key, 0);
	}

	if (key_code) {
		input_report_key(keypad->input, key_code, 1);
	}

	/* TODO: Add pressed check for gesture or miss touch. */
	keypad->last_key = key_code;

out:
	mutex_unlock(&avr_keypad_lock);

	return NOTIFY_OK;
}

static int __devinit avr_keypad_probe(struct platform_device *pdev)
{
	struct avr_keypad *keypad;
	struct avr_chip *chip;
	int rc;
	int firmware_version;
	
	chip = platform_get_drvdata(pdev);
	if (chip == NULL) {
		dev_err(&pdev->dev, "no parent data passed in\n");
		return -EFAULT;
	}

	keypad = kzalloc(sizeof(*keypad), GFP_KERNEL);
	if (!keypad)
		return -ENOMEM;

	platform_set_drvdata(pdev, keypad);

	keypad->dev	= &pdev->dev;
	keypad->chip	= chip;

	firmware_version = avr_get_firmware_version(chip);

	if (firmware_version > 0x30)
		keypad->layout_type = AVR_LAYOUT_TYPE_C;
	else
		keypad->layout_type = AVR_LAYOUT_TYPE_A;

	if (firmware_version < 0x38)
		keypad->sensitivity_cap = false;
	else
		keypad->sensitivity_cap = true;

	if (device_create_file(keypad->dev, &avr_keypad_attrs)) {
		dev_err(keypad->dev, "unable to create sysfs entries\n");
		rc = -EFAULT;
		goto err_create_sysfs_entry;
	}

	keypad->input = input_allocate_device();
	if (!keypad->input) {
		dev_err(keypad->dev, "unable to allocate input device\n");
		rc = -ENOMEM;
		goto err_alloc_device;
	}

	mutex_init(&avr_keypad_lock);

	/*
	 * We don't need setting up an irq handler directly because avr core
	 * will notify us about IRQ event.
	 */

	keypad->notifier.notifier_call = avr_keypad_notifier_func;

	avr_notify_register(keypad->chip, &keypad->notifier);

	keypad->input->name = AVR_KEYPAD_DRIVER_NAME;
	keypad->input->id.bustype = BUS_I2C;
	keypad->input->evbit[0] = BIT_MASK(EV_SYN)|BIT_MASK(EV_KEY);
	keypad->input->keybit[BIT_WORD(KEY_HOME)] = BIT_MASK(KEY_HOME);
	keypad->input->keybit[BIT_WORD(KEY_BACK)] = BIT_MASK(KEY_BACK)|BIT_MASK(KEY_MENU);
	keypad->input->keybit[BIT_WORD(KEY_SEND)] |= BIT_MASK(KEY_SEND);
	keypad->input->keybit[BIT_WORD(0xE5)] |= BIT_MASK(0xE5);
	keypad->input->keybit[BIT_WORD(KEY_SEARCH)] |= BIT_MASK(KEY_SEARCH);

	rc = input_register_device(keypad->input);
	if (rc)
	    goto err_register_device;

	avr_keypad_threshold_update(keypad, AVR_KEYPAD_THRESHOLD_DEFAULT);

	return 0;

err_register_device:
	input_free_device(keypad->input);
err_alloc_device:
	device_remove_file(keypad->dev, &avr_keypad_attrs);
err_create_sysfs_entry:
	kfree(keypad);
	return rc;
}

static int __devexit avr_keypad_remove(struct platform_device *pdev)
{
	struct avr_keypad *keypad = platform_get_drvdata(pdev);

	input_unregister_device(keypad->input);
	platform_set_drvdata(pdev, NULL);
	kfree(keypad);

	return 0;
}

#ifdef CONFIG_PM
static int avr_keypad_suspend(struct platform_device *pdev, pm_message_t msg)
{
	return 0;
}

static int avr_keypad_resume(struct platform_device *pdev)
{
	return 0;
}
#else
#define avr_keypad_suspend	NULL
#define avr_keypad_resume	NULL
#endif // CONFIG_PM

static struct platform_driver avr_keypad_driver = {
	.probe		= avr_keypad_probe,
	.remove		= __devexit_p(avr_keypad_remove),
	.suspend	= avr_keypad_suspend,
	.resume		= avr_keypad_resume,
	.driver		= {
		.name = AVR_KEYPAD_DRIVER_NAME,
		.owner = THIS_MODULE,
	},
};

static int __init avr_keypad_init(void)
{
	return platform_driver_register(&avr_keypad_driver);
}
module_init(avr_keypad_init);

static void __exit avr_keypad_exit(void)
{
	platform_driver_unregister(&avr_keypad_driver);
}
module_exit(avr_keypad_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Acer AVR keypad driver");
MODULE_ALIAS("platform:avr_keypad");
MODULE_AUTHOR("Roman Yepishev");
