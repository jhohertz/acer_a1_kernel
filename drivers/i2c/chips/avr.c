/*
 * First release: 2009.04.15
 * - Enable keypad single touch mode
 * - IOCTL for backlight
 * - IOCTL for keypad toggle
 */
#if defined (CONFIG_ACER_DEBUG)
#define DEBUG
#endif

#define DEBUG

#include <linux/input.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include <linux/earlysuspend.h>
#include <linux/fb.h>
#include <linux/leds.h>
#include <linux/mfd/avr.h>

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

#define BACKLIGHT_LEVEL_ON        0x8

#define AVR_I2C_RETRY_COUNT	5

static int __init avr_init(void);
static int avr_probe(struct i2c_client *client, const struct i2c_device_id *id);
static int avr_remove(struct i2c_client *client);
static int avr_suspend(struct i2c_client *client, pm_message_t mesg);
static int avr_resume(struct i2c_client *client);
static irqreturn_t avr_interrupt(int irq, void *dev_id);
static void avr_input_work_func(struct work_struct *work);
static int __init avr_input_register(struct input_dev *input);
static int avr_open(struct inode *inode, struct file *file);
static int avr_close(struct inode *inode, struct file *file);
static int avr_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg);
static void led_on(struct i2c_client *client);
static void led_off(struct i2c_client *client);
static void avr_set_power_mode(struct i2c_client *client, int mode);
static void avr_input_key_clear(struct device *dev);
static void avr_led_work_func(struct work_struct *work);
#ifdef CONFIG_HAS_EARLYSUSPEND
static void avr_early_suspend(struct early_suspend *h);
static void avr_early_resume(struct early_suspend *h);
#endif
static bool kpd_fw_check = false;
static bool kpd_resume_check = true;
static bool kpd_pwr_key_check = false;
static struct mutex avr_mutex;
static bool led_call = false;

static int __avr_write(struct i2c_client* client, int reg, int val, int once);
static int __avr_read(struct i2c_client* client, int *val, int once);
static int __avr_query(struct i2c_client* client, int reg, int *val, int once);

static const struct i2c_device_id avr_id[] = {
	{ AVR_DRIVER_NAME, 0 },
	{ }
};

/* Data for I2C driver */
struct avr_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct led_classdev led_bl;
	struct led_classdev led_keypad;

	struct work_struct irq_work;
	struct delayed_work led_work;

	wait_queue_head_t wait;

#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
	int last_key;
	int keypad_firmware_version;
};

/* FIXME: This is required because of ioctl */
static struct avr_data* the_avr_data;

/*File operation of AVR device file */
static const struct file_operations avr_fops = {
	.owner     = THIS_MODULE,
	.open      = avr_open,
	.release   = avr_close,
	.ioctl     = avr_ioctl,
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

static struct miscdevice avr_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = AVR_DRIVER_NAME,
	.fops = &avr_fops,
};

static int __avr_write(struct i2c_client* client, int reg, int val, int once)
{
	int res = -1;
	int retry = AVR_I2C_RETRY_COUNT;
	uint8_t buf[2] = { (uint8_t)reg, (uint8_t)val };
	int count = (val == -1) ? 1 : 2;

	while (retry-- > 0) {
		if (count == i2c_master_send(client, buf, count )) {
			res = 0;
			break;
		}

		if (once)
			break;

		msleep(200);
	}

	return res;
}

static int __avr_read(struct i2c_client* client, int* val, int once)
{
	int res = -1;
	uint8_t response;
	int retry = AVR_I2C_RETRY_COUNT;

	while (retry-- > 0) {
		if (1 == i2c_master_recv(client, &response, 1)) {
			res = 0;
			break;
		}

		if (once)
			break;

		msleep(200);
	}

	*val = response;

	return res;
}

static int __avr_query(struct i2c_client* client, int reg, int *val, int once)
{
	if (__avr_write(client, reg, -1, once))
		return -1;
	if (__avr_read(client, val, once))
		return -1;
	return 0;
}

int avr_write(struct device* dev, int reg, int val, int once)
{
	return __avr_write(to_i2c_client(dev), reg, val, once);
}
EXPORT_SYMBOL_GPL(avr_write);

int avr_read(struct device* dev, int* val, int once)
{
	return __avr_read(to_i2c_client(dev), val, once);
}
EXPORT_SYMBOL_GPL(avr_read);

int avr_query(struct device* dev, int reg, int *val, int once)
{
	return __avr_query(to_i2c_client(dev), reg, val, once);
}
EXPORT_SYMBOL_GPL(avr_query);

static void avr_set_power_mode(struct i2c_client *client, int mode){
	if (avr_write(&client->dev, I2C_REG_LOW_POWER, mode, 0))
		pr_err("%s: error setting mode", __func__);
}

/* LEDs */
static void avr_backlight_led_set(struct led_classdev *led_cdev,
	                         enum led_brightness value)
{
	int avr_brightness;
	struct avr_data *data = container_of(led_cdev, struct avr_data, led_bl);

	avr_brightness = (2 * value * AVR_BKL_MAX_LVL + MAX_BACKLIGHT_BRIGHTNESS)
			/(2 * MAX_BACKLIGHT_BRIGHTNESS);

	if (avr_write(&data->client->dev, I2C_REG_BKL, avr_brightness, 0))
		pr_err("%s: Error setting LCD brightness\n", __func__);

}

static void avr_keypad_led_set(struct led_classdev *led_cdev,
	                         enum led_brightness value)
{
	int avr_brightness;
	struct avr_data *data = container_of(led_cdev, struct avr_data, led_keypad);

	avr_brightness = (2 * value * AVR_LED_MAX_LVL + MAX_LED_BRIGHTNESS)
			/(2 * MAX_LED_BRIGHTNESS);

	if (avr_write(&data->client->dev, I2C_REG_LED_1, avr_brightness, 0))
		pr_err("%s: Error setting keypad brightness\n", __func__);
}

static void led_on(struct i2c_client *client){
	struct avr_data *data = i2c_get_clientdata(client);
	avr_keypad_led_set(&data->led_keypad, 16);
}

static void led_off(struct i2c_client *client){
	struct avr_data *data = i2c_get_clientdata(client);
	avr_keypad_led_set(&data->led_keypad, AVR_LED_OFF);
}

static void avr_led_work_func(struct work_struct *work)
{
	struct delayed_work *led_work = container_of(work, struct delayed_work, work);
	struct avr_data* data = container_of(led_work, struct avr_data, led_work);

	led_off(data->client);
	led_call = false;
}

/* Keypad */
static ssize_t avr_set_keypad_sensitivity(struct device *device,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	int res;
	long value;
	struct avr_data *data = dev_get_drvdata(device);

	if (!(data->keypad_firmware_version > 0x38))
		return count;

	res = strict_strtol(buf, 10, &value);
	if (res) {
	    /* FIXME: check for range */
	    pr_err("[AVR] strict_strol() failed for %s\n", buf);
	    value = AVR_SENSITIVITY_DEFAULT; 
	}

	pr_info("[AVR] Threshold value = %ld\n", value);

	if ( 0 != avr_write(device, I2C_REG_SENSITIVITY, value, 0))
		pr_err("[AVR] Set AVR threshold value error\n");

	return count;
}

static struct device_attribute avr_attrs =
__ATTR(threshold, S_IRWXUGO,NULL, avr_set_keypad_sensitivity);

static ssize_t get_avr_firmware(struct device *dev, struct device_attribute *attr,
             char *buf)
{
	int fw_check = 0xff;
	struct avr_data *data = dev_get_drvdata(dev);

	fw_check = (data->keypad_firmware_version > 0x38) ? 1 : 0;

	pr_info("[AVR] Firmware ver=0x%02X, fw_check = %d\n",
		data->keypad_firmware_version, fw_check);

	return sprintf(buf, "%d\n", fw_check);
}

static struct device_attribute avr_fw_attrs =
__ATTR(fw_check, S_IRUGO,get_avr_firmware, NULL);

/* Keypad input */
static int __init avr_input_register(struct input_dev *input)
{
	input->name = AVR_DRIVER_NAME;
	input->id.bustype = BUS_I2C;
	input->evbit[0] = BIT_MASK(EV_SYN)|BIT_MASK(EV_KEY);
	input->keybit[BIT_WORD(KEY_HOME)] = BIT_MASK(KEY_HOME);
	input->keybit[BIT_WORD(KEY_BACK)] = BIT_MASK(KEY_BACK)|BIT_MASK(KEY_MENU);
	input->keybit[BIT_WORD(KEY_SEND)] |= BIT_MASK(KEY_SEND);
	input->keybit[BIT_WORD(0xE5)] |= BIT_MASK(0xE5);
	input->keybit[BIT_WORD(KEY_SEARCH)] |= BIT_MASK(KEY_SEARCH);
	return input_register_device(input);
}

static void avr_input_key_clear(struct device *dev)
{
	/* To prevent uP holding KEY_INT pin to low without getting value */
	int dummy;
	avr_query(dev, I2C_REG_KEY_STATUS, &dummy, 0);
}

static void avr_input_work_func(struct work_struct *work)
{
	struct avr_data* data = container_of(work, struct avr_data, irq_work);
	struct i2c_client *client = data->client;
	struct device *dev = &client->dev;

	int key_st = 0;
	int key_code = 0;
	int count = 0;
	int res = -1;

	mutex_lock(&avr_mutex);

	/* Step 1. Scan Key */
	while (count < 5) {
		if (avr_query(dev, I2C_REG_KEY_STATUS, &key_st, 1)) {
			if (count == 1) {
				input_report_key(data->input, data->last_key, 0);
				data->last_key = key_code;
			}
			msleep(200);
		}
		else {
			res = 0;
			break;
		}
		count++;
	}

	/* Step 2. Send Key event */
	if(res == -1 || key_st == data->keypad_firmware_version || count > 1){
		avr_input_key_clear(dev);
		mutex_unlock(&avr_mutex);
		return;
	}

	cancel_delayed_work(&data->led_work);

	if ( key_st != 0)
	{
		led_on(client);
		led_call = true;
	}
	else
		schedule_delayed_work(&data->led_work, msecs_to_jiffies(AVR_LED_DELAY_TIME));

	switch(key_st){
	case AVR_KEY_MENU:
		key_code = KEY_HOME;
		break;
	case AVR_KEY_LEFT:
		key_code = KEY_SEARCH;
		break;
	case AVR_KEY_RIGHT:
		if (kpd_fw_check)
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
             __func__, key_st, key_code, data->last_key);

	/* Send key release if not equal to last key */
	if( key_code != data->last_key){
		input_report_key(data->input, data->last_key, 0);
	}
	/* Send key press if key_code != 0 */
	if( key_code ) {
		input_report_key(data->input, key_code, 1);
	}
	/* TODO: Add pressed check for gesture or miss touch. */
	data->last_key = key_code;
	/* Step 2. End Send Key event */

	mutex_unlock(&avr_mutex);
}

static int __init avr_init(void)
{
	int res=0;

	res = i2c_add_driver(&avr_driver);

	if (res)
		pr_err("%s: i2c_add_driver failed\n", __func__);

	return res;
}

static int avr_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int err = 0;
	struct avr_data *data;

	pr_debug("[AVR] %s ++ entering\n", __FUNCTION__);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("%s: i2c_check_functionality failed\n", __func__);
		return -ENOTSUPP;
	}

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (IS_ERR(data)) {
		return -ENOMEM;
	}

	// Sneaky-sneaky
	the_avr_data = data;

	data->client = client;
	data->last_key = 1;

	strlcpy(client->name, AVR_DRIVER_NAME, I2C_NAME_SIZE);
	i2c_set_clientdata(client, data);

	mutex_init(&avr_mutex);

	INIT_WORK(&data->irq_work, avr_input_work_func);
	init_waitqueue_head(&data->wait);

	INIT_DELAYED_WORK(&data->led_work, avr_led_work_func);

	/* input register */
	data->input = input_allocate_device();
	if (data->input == NULL) {
		pr_err("[AVR] input_allocate_device error!\n");
		err = -ENOMEM;
		goto error_free;
	}

	err = avr_input_register(data->input);
	if (err < 0) {
		pr_err("[AVR] avr_input_register error\n");
		goto error_input;
	}

	if (client->irq) {
		err = request_irq(client->irq, avr_interrupt,
				  IRQF_TRIGGER_FALLING,
				  AVR_DRIVER_NAME, data);
		if (err < 0) {
			pr_err("[AVR] request_irq error! %d\n", err);
			free_irq(client->irq, data);
		}
	}

	data->led_keypad.name = "button-backlight";
	data->led_keypad.brightness_set = avr_keypad_led_set;
	data->led_keypad.flags = LED_CORE_SUSPENDRESUME;

	data->led_bl.name = "lcd-backlight";
	data->led_bl.brightness_set = avr_backlight_led_set;
	data->led_bl.flags = LED_CORE_SUSPENDRESUME;

	err = led_classdev_register(&client->dev, &data->led_keypad);
	if (err < 0)
		goto error_input;

	err = led_classdev_register(&client->dev, &data->led_bl);
	if (err < 0)
		goto error_input;

	// ioctl interface
	err = misc_register(&avr_dev);
	if (err) {
		pr_err("%s: avr_dev register failed\n", __func__);
		goto error_leds;
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	/* To set BLANK_SCREEN level that prevent wrong-touch while talking */
	data->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN;
	data->early_suspend.suspend = avr_early_suspend;
	data->early_suspend.resume = avr_early_resume;
	register_early_suspend(&data->early_suspend);
#endif

	avr_backlight_led_set(&data->led_bl, BACKLIGHT_LEVEL_ON);

	if (avr_query(&client->dev, I2C_REG_FW,
		      &data->keypad_firmware_version, 0)) {
		pr_err("[AVR] i2c_read fail\n");
		goto error_avr_dev;
	}

	/* To check keypad for Type Proposal_A  / Type Proposal_C (0x30) */
	kpd_fw_check = (data->keypad_firmware_version > 0x30) ? true : false;

	pr_info("[AVR] Firmware ver=0x%02X\n", data->keypad_firmware_version);

#if USE_FS
	if(device_create_file(&client->dev, &avr_attrs))
		pr_err("[AVR] device_create_file avr_attrs error \n");

	if(device_create_file(&client->dev, &avr_fw_attrs))
		pr_err("[AVR] device_create_file avr_fw_attrs error \n");
#endif

	pr_debug("[AVR] %s -- leaving\n", __FUNCTION__);

	return 0;

error_leds:
	// unregister leds?
error_avr_dev:
	free_irq(client->irq, data);
error_input:
	input_free_device(data->input);
error_free:
	kfree(data);
	pr_err("[AVR] probe error\n");
	return err;
}

static int avr_remove(struct i2c_client *client)
{
	struct avr_data *data = i2c_get_clientdata(client);
	input_unregister_device(data->input);
	free_irq(client->irq, data);

	led_classdev_unregister(&data->led_bl);
	led_classdev_unregister(&data->led_keypad);

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&data->early_suspend);
#endif

	kfree(data);

	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void avr_early_suspend(struct early_suspend *h)
{
	struct avr_data* data = container_of(h, struct avr_data, early_suspend);
	pr_info("[AVR] %s ++ entering\n", __FUNCTION__);

	led_call = false;
	kpd_resume_check = false;
	avr_input_key_clear(&data->client->dev);
	disable_irq(data->client->irq);
	avr_set_power_mode(data->client, AVR_POWER_LOW);

	pr_info("[AVR] %s -- leaving\n", __FUNCTION__);
}

static void avr_early_resume(struct early_suspend *h)
{
	struct avr_data* data = container_of(h, struct avr_data, early_suspend);
	pr_info("[AVR] %s ++ entering\n", __FUNCTION__);

	avr_set_power_mode(data->client, AVR_POWER_NORMAL);
	enable_irq(data->client->irq);

	kpd_resume_check = true;

	if(kpd_pwr_key_check){
		kpd_pwr_key_check = false;
		led_on(data->client);
	}

	pr_info("[AVR] %s -- leaving\n", __FUNCTION__);
}
#endif

static int avr_suspend(struct i2c_client *client, pm_message_t mesg)
{
	pr_debug("[AVR] low power suspend init done.\n");

	return 0;
}

static int avr_resume(struct i2c_client *client)
{
	pr_debug("[AVR] normal resume init done.\n");

	return 0;
}

static irqreturn_t avr_interrupt(int irq, void *dev_id)
{
	/* TODO: Remove mdelay() to prevent listening */
	/*       music delay on BT Headset via A2DP   */
	disable_irq(irq);
	schedule_work(&the_avr_data->irq_work);
	enable_irq(irq);
	return IRQ_HANDLED;
}

/* open command for AVR device file	*/
static int avr_open(struct inode *inode, struct file *file)
{
	return 0;
}
/* close command for AVR device file */
static int avr_close(struct inode *inode, struct file *file)
{
	return 0;
}

/*
 * FIXME: This will be removed completely once brightness and leds are fixed
 */
static int avr_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
{
	int err = 0;
	struct i2c_client *client = the_avr_data->client;

	/* check cmd */
	if(_IOC_TYPE(cmd) != DEV_IOCTLID){
		pr_err("cmd magic type error\n");
		return -ENOTTY;
	}
	if(_IOC_NR(cmd) > IOC_MAXNR){
		pr_err("cmd number error\n");
		return -ENOTTY;
	}

	if (_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok(VERIFY_WRITE, (void __user*)arg, _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		err = !access_ok(VERIFY_READ, (void __user*)arg, _IOC_SIZE(cmd));
	if (err){
		pr_err("cmd access_ok error\n");
		return -EFAULT;
	}
	if (client == NULL){
		pr_err("I2C driver not install (AVR_ioctl)\n");
		return -EFAULT;
	}

	/* cmd mapping */
	switch(cmd){
	case IOCTL_SET_LED_ON:
		if(led_call){
			led_call = false;
			return 0;
		}

		if(kpd_resume_check){
			led_on(client);
		}else{
			kpd_pwr_key_check = true;
		}

		pr_debug("[AVR] IOCTL_SET_LED_ON.\n");
		break;
	case IOCTL_SET_LED_OFF:
		led_off(client);
		pr_debug("[AVR] IOCTL_SET_LED_OFF.\n");
		break;
	case IOCTL_SET_BL_ON:
		avr_backlight_led_set(&the_avr_data->led_bl, AVR_BKL_ON);
		pr_debug("[AVR] IOCTL_SET_BL_ON. \n");
		break;
	case IOCTL_SET_BL_OFF:
		avr_backlight_led_set(&the_avr_data->led_bl, AVR_BKL_OFF);
		pr_debug("[AVR] IOCTL_SET_BL_OFF. \n");
		break;
	case IOCTL_SET_BL_LV:
		avr_backlight_led_set(&the_avr_data->led_bl, arg);
		pr_debug("[AVR] IOCTL_SET_BL_LV, Set backlight %ld. \n", arg);
		break;
	case IOCTL_KEY_LOCK_TOGGLE:
		if(arg != 0 || arg != 1){
			arg = 0;
		}
		avr_write(&client->dev, I2C_REG_KEY_LOCK, (int)arg, 0);
		break;
	default:
		return -1;
	}

	return 0;
}

static void __exit avr_exit(void)
{
	i2c_del_driver(&avr_driver);
}

module_init(avr_init);
module_exit(avr_exit);

MODULE_AUTHOR("Elay Hu <Elay_Hu@acer.com.tw>");
MODULE_DESCRIPTION("AVR micro-P driver");
