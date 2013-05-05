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
#include <linux/backlight.h>
#include <linux/fb.h>
#include <linux/leds.h>

#define DEV_IOCTLID               0x11
#define IOC_MAXNR                 18
#define IOCTL_SET_BL_ON           _IOW(DEV_IOCTLID, 1, int)
#define IOCTL_SET_BL_OFF          _IOW(DEV_IOCTLID, 2, int)
#define IOCTL_SET_BL_LV           _IOW(DEV_IOCTLID, 3, int)
#define IOCTL_SET_LED1_ON         _IOW(DEV_IOCTLID, 4, int)
#define IOCTL_SET_LED1_OFF        _IOW(DEV_IOCTLID, 5, int)
#define IOCTL_SET_LED2_ON         _IOW(DEV_IOCTLID, 6, int)
#define IOCTL_SET_LED2_OFF        _IOW(DEV_IOCTLID, 7, int)
#define IOCTL_KEY_LOCK_TOGGLE     _IOW(DEV_IOCTLID, 8, int)
#define IOCTL_SET_LED_ON          _IOW(DEV_IOCTLID, 10, int)
#define IOCTL_SET_LED_OFF         _IOW(DEV_IOCTLID, 11, int)
#define IOCTL_SIMPLE_TEST_ON      _IOW(DEV_IOCTLID, 12, int)
#define IOCTL_SIMPLE_TEST_OFF     _IOW(DEV_IOCTLID, 13, int)
#define IOCTL_TEST_KEY            _IOW(DEV_IOCTLID, 15, int)
#define IOCTL_TEST_KEY_UP         _IOW(DEV_IOCTLID, 16, int)
#define IOCTL_TEST_KEY_DOWN       _IOW(DEV_IOCTLID, 17, int)

#define AVR_DRIVER_NAME           "avr"

#define MAX_BACKLIGHT_BRIGHTNESS  255
/* Modify AVR_BKL_LVL for backlight level 30 to 255 */
#define AVR_BKL_MAX_LVL           0x20
#define AVR_BKL_MIN_LVL           0x01
#define AVR_BKL_ON                AVR_BKL_MAX_LVL
#define AVR_BKL_OFF               0x00
#define I2C_REG_FW                0xD0
#define I2C_REG_LED_1             0xD1
// I2C_REG_LED_2 is unused
#define I2C_REG_LED_2             0xD2
#define I2C_REG_BKL               0xD3
#define I2C_REG_KEY_STATUS        0xD4
#define I2C_REG_KEY_LOCK          0xD8
#define I2C_REG_LOW_POWER         0xD9
#define AVR_LED_MAX_LVL           0x20
#define AVR_LED_ON                AVR_LED_MAX_LVL
#define AVR_LED_OFF               0x00
#define AVR_POWER_NORMAL          0x00
#define AVR_POWER_LOW             0x01

/* AVR Keycode */
#define AVR_KEY_MENU              (1<<0)
#define AVR_KEY_LEFT              (1<<1)
#define AVR_KEY_DOWN              (1<<2)
#define AVR_KEY_RIGHT             (1<<3)
#define AVR_KEY_BACK              (1<<4)
#define AVR_KEY_UP                (1<<5)

#define AVR_KEYMASK_DIRECTION     (AVR_KEY_UP|AVR_KEY_DOWN|AVR_KEY_LEFT|AVR_KEY_RIGHT)

#define AVR_LED_DELAY_TIME        10000

#define BACKLIGHT_LEVEL_ON        0x8

/* AVR Sensitivity */
#define USE_FS                    1
#define I2C_REG_SENSITIVITY       0x60
#define AVR_SENSITIVITY_DEFAULT   20

static int __init avr_init(void);
static int avr_probe(struct i2c_client *client, const struct i2c_device_id *id);
static int avr_remove(struct i2c_client *client);
static int avr_suspend(struct i2c_client *client, pm_message_t mesg);
static int avr_resume(struct i2c_client *client);
static irqreturn_t avr_interrupt(int irq, void *dev_id);
static void avr_work_func(struct work_struct *work);
static int __init avr_register_input(struct input_dev *input);
static int avr_open(struct inode *inode, struct file *file);
static int avr_close(struct inode *inode, struct file *file);
static int avr_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg);
static int i2c_write(struct i2c_client *client, char *buf);
static int i2c_read(struct i2c_client *client, char *buf, int count);
static void led_on(struct i2c_client *client);
static void led_off(struct i2c_client *client);
static void low_power_mode(struct i2c_client *client, int mode);
static void key_clear(struct i2c_client *client);
static void avr_led_work_func(struct work_struct *work);
#ifdef CONFIG_HAS_EARLYSUSPEND
static void avr_early_suspend(struct early_suspend *h);
static void avr_early_resume(struct early_suspend *h);
#endif
static int kpd_fw = 0xff;
static bool bsimple_test = false;
static bool kpd_fw_check = false;
static bool kpd_resume_check = true;
static bool kpd_pwr_key_check = false;
static struct mutex avr_mutex;
static bool led_call = false;

static const struct i2c_device_id avr_id[] = {
	{ AVR_DRIVER_NAME, 0 },
	{ }
};


/* Data for I2C driver */
struct avr_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct backlight_device *bd;
	struct led_classdev led_cdev;

	struct work_struct work;
	struct delayed_work led_work;

	wait_queue_head_t wait;

#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
	int lcd_brightness;
	enum led_brightness led_brightness;

	int last_key;
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

#if USE_FS
static ssize_t set_avr_sensitivity(struct device *device,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	uint8_t data_buf[2] = { I2C_REG_SENSITIVITY, 0 };
	struct avr_data* data = dev_get_drvdata(device);

	int res;
	long value;

	if(!(kpd_fw>0x38))
		return count;

	res = strict_strtol(buf, 10, &value);
	if (res) {
	    // FIXME: check for range
	    pr_err("[AVR] strict_strol() failed for %s\n", buf);
	    value = AVR_SENSITIVITY_DEFAULT; 
	}

	data_buf[1] = (int)value;

	pr_info("[AVR] Threshold value = %d\n",data_buf[1]);

	if ( 0 != i2c_write(data->client, data_buf))
		pr_err("[AVR] Set AVR threshold value error\n");

	return count;
}

static struct device_attribute avr_attrs =
__ATTR(threshold, S_IRWXUGO,NULL, set_avr_sensitivity);

static ssize_t get_avr_firmware(struct device *dev, struct device_attribute *attr,
             char *buf)
{
	int fw_check = 0xff;
	fw_check = (kpd_fw>0x38) ? 1 : 0;

	pr_info("[AVR] Firmware ver=0x%02X, fw_check = %d\n", kpd_fw, fw_check);

	return sprintf(buf, "%d\n", fw_check);
}

static struct device_attribute avr_fw_attrs =
__ATTR(fw_check, S_IRUGO,get_avr_firmware, NULL);

#endif

// New backlight code handling via sysfs
static int avr_backlight_get_intensity(struct backlight_device *bd)
{
	struct avr_data* data = dev_get_drvdata(&bd->dev);
        return data->lcd_brightness;
}

// This should get merged into update_status
static void avr_backlight_set_brightness(int value)
{
	uint8_t data_buf[2] = {0};
	int count = 0, rc;

	/* This should go through bl_get_data() */
	struct i2c_client *client = the_avr_data->client;

	data_buf[0] = I2C_REG_BKL;
	data_buf[1] = value;

	while( count < 5 ){
		rc = i2c_write(client, data_buf);

		if(rc){
			/* Retry if i2c_read error */
			msleep(200);
		}else{
			/* i2c_read success */
			break;
		}
		count++;
	}

	the_avr_data->lcd_brightness = value;
}

static int avr_backlight_update_status(struct backlight_device *bd)
{
	int brightness = bd->props.brightness;

	if (bd->props.power != FB_BLANK_UNBLANK)
		brightness = 0;

	if (bd->props.fb_blank != FB_BLANK_UNBLANK)
		brightness = 0;

	avr_backlight_set_brightness(brightness);
	return 0;
}

static struct backlight_ops avr_backlight_ops = {
	.get_brightness = avr_backlight_get_intensity,
	.update_status  = avr_backlight_update_status,
};

// New LEDs code sysfs handling
static void avr_keypad_led_set(struct led_classdev *led_cdev,
	                         enum led_brightness value)
{
	uint8_t data_buf[2] = {0};
	int avr_brightness = AVR_LED_ON;
	struct avr_data* data = container_of(led_cdev, struct avr_data, led_cdev);

	// By convention LED_FULL is 255, and we have 32 values.
	// Recent kernels have max_brightness field, we don't, yet.
	// We will use (value + 1) / 8 
	//
	avr_brightness = (value + 1) / 8;

	data_buf[0] = I2C_REG_LED_1;
	data_buf[1] = avr_brightness;
	i2c_write(data->client, data_buf);

	data->led_brightness = value;
}

enum led_brightness avr_keypad_led_get(struct led_classdev *led_cdev)
{
	struct avr_data* data = container_of(led_cdev, struct avr_data, led_cdev);
	return data->led_brightness;
}

static int __init avr_init(void)
{
	int res=0;

	res = i2c_add_driver(&avr_driver);

	if (res){
		pr_err("[AVR]i2c_add_driver failed! \n");
		return res;
	}

	return 0;
}

static int avr_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int err = 0;
	uint8_t data_buf[2] = {0};
	int count = 0;
	int rc;
	struct backlight_device *bd;
	struct avr_data *data;

	data = kzalloc(sizeof(struct avr_data), GFP_KERNEL);

	// Sneaky-sneaky
	the_avr_data = data;

	data->client = client;
	data->last_key = 1;

	pr_debug("[AVR] %s ++ entering\n", __FUNCTION__);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("[AVR] i2c_check_functionality error!\n");
		return -ENOTSUPP;
	}
	strlcpy(client->name, AVR_DRIVER_NAME, I2C_NAME_SIZE);
	i2c_set_clientdata(client, data);

	mutex_init(&avr_mutex);

	INIT_WORK(&data->work, avr_work_func);
	init_waitqueue_head(&data->wait);

	INIT_DELAYED_WORK(&data->led_work, avr_led_work_func);

	/* input register */
	data->input = input_allocate_device();
	if (data->input == NULL) {
		pr_err("[AVR] input_allocate_device error!\n");
		return -ENOMEM;
	}

	err = avr_register_input(data->input);
	if (err < 0) {
		pr_err("[AVR] AVR_register_input error\n");
		goto error;
	}

	if (client->irq) {
		err = request_irq(client->irq, avr_interrupt, IRQF_TRIGGER_FALLING,
				  AVR_DRIVER_NAME, data);
		if (err < 0) {
			pr_err("[AVR] request_irq error! %d\n", err);
			free_irq(client->irq, data);
		}
	}

	// New sysfs backlight code
	bd = backlight_device_register("avr-backlight",
			&client->dev, data, &avr_backlight_ops);
	if (IS_ERR(bd)) {
		goto error_avr_dev;
	}

	data->bd = bd;

	data->led_cdev.name = "avr::keypad";
	data->led_cdev.brightness_set = avr_keypad_led_set;
	data->led_cdev.brightness_get = avr_keypad_led_get;
	data->led_cdev.flags = LED_CORE_SUSPENDRESUME;

	bd->props.max_brightness = AVR_BKL_MAX_LVL;
	bd->props.brightness = AVR_BKL_MAX_LVL;
	// end of new sysfs backlight code

	err = led_classdev_register(&client->dev, &data->led_cdev);
	if (err < 0)
		return err;

	// ioctl interface
	err = misc_register(&avr_dev);
	if (err) {
		pr_err("avr_probe: avr_dev register failed\n");
		goto error_avr_dev;
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	/* To set BLANK_SCREEN level that prevent wrong-touch while talking */
	data->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN;
	data->early_suspend.suspend = avr_early_suspend;
	data->early_suspend.resume = avr_early_resume;
	register_early_suspend(&data->early_suspend);
#endif

	avr_backlight_set_brightness(BACKLIGHT_LEVEL_ON);

	/* Get keypad FW version */
	data_buf[0] = I2C_REG_FW;
	data_buf[1] = 0;

	while( count < 5 ){
		rc = i2c_read(client, data_buf, 1);
		if (rc) {
			/* Retry if i2c_read error */
			msleep(200);
		}else{
			/* i2c_read success */
			break;
		}
		count++;
	}

	if (rc) {
		pr_err("[AVR] i2c_read fail\n");
		goto error_avr_dev;
	}

	kpd_fw = data_buf[0];
	/* To check keypad for Type Proposal_A  / Type Proposal_C (0x30) */
	kpd_fw_check = (data_buf[0]>0x30) ? true : false;

	pr_info("[AVR] Firmware ver=0x%02X\n", data_buf[0]);

#if USE_FS
	if(device_create_file(&client->dev, &avr_attrs))
		pr_err("[AVR] device_create_file avr_attrs error \n");

	if(device_create_file(&client->dev, &avr_fw_attrs))
		pr_err("[AVR] device_create_file avr_fw_attrs error \n");
#endif

	pr_debug("[AVR] %s -- leaving\n", __FUNCTION__);

	return 0;

error_avr_dev:
	free_irq(client->irq, data);
error:
	input_free_device(data->input);
	pr_err("[AVR] probe error\n");
	return err;
}

static int avr_remove(struct i2c_client *client)
{
	struct avr_data *data = i2c_get_clientdata(client);
	input_unregister_device(data->input);
	free_irq(client->irq, data);
	
	backlight_device_unregister(data->bd);
	led_classdev_unregister(&data->led_cdev);

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
	key_clear(data->client);
	disable_irq(data->client->irq);
	low_power_mode(data->client,1);

	pr_info("[AVR] %s -- leaving\n", __FUNCTION__);
}

static void avr_early_resume(struct early_suspend *h)
{
	struct avr_data* data = container_of(h, struct avr_data, early_suspend);
	pr_info("[AVR] %s ++ entering\n", __FUNCTION__);

	low_power_mode(data->client, 0);
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

static void avr_work_func(struct work_struct *work)
{
	struct avr_data* data = container_of(work, struct avr_data, work);
	struct i2c_client *client = data->client;

	uint8_t data_buf[2] = {0};
	int key_st = 0;
	int key_code = 0;
	int count = 0;

	mutex_lock(&avr_mutex);

	/* Step 1. Scan Key */
	data_buf[0] = I2C_REG_KEY_STATUS;
	data_buf[1] = 0;

	while( count < 5 ){
		if ( i2c_read(client, data_buf, 1) ) {
			if (count == 1){
				input_report_key(data->input, data->last_key, 0);
				data->last_key = key_code;
			}
			/* Retry if i2c_read error */
			msleep(200);
		}else{
			/* i2c_read success */
			break;
		}
		count++;
	}
	/* Step 1. End Scan key */

	/* Step 2. Send Key event */
	key_st = data_buf[0];

	// WTF???
	if(key_st == I2C_REG_KEY_STATUS || key_st == kpd_fw || count > 1){
		key_clear(client);
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

	if(kpd_fw_check) {
		switch(key_st){
		case AVR_KEY_MENU:
			if(!bsimple_test)
				key_code = KEY_HOME;
			else
				key_code = KEY_SEND;
			break;
		case AVR_KEY_LEFT:
			key_code = KEY_SEARCH;
			break;
		case AVR_KEY_DOWN:
			key_code = KEY_BACK;
			break;
		case AVR_KEY_RIGHT:
			key_code = 0xE5; /* MENU */
			break;
		default:
			key_code = 0;
			break;
		}
	}
	else {
		switch(key_st){
		case AVR_KEY_MENU:
			if(!bsimple_test)
				key_code = KEY_HOME;
			else
				key_code = KEY_SEND;
			break;
		case AVR_KEY_LEFT:
			key_code = KEY_SEARCH;
			break;
		case AVR_KEY_RIGHT:
			key_code = KEY_BACK;
			break;
		case AVR_KEY_BACK:
			key_code = 0xE5; /* MENU */
			break;
		default:
			key_code = 0;
			break;
		}
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

static irqreturn_t avr_interrupt(int irq, void *dev_id)
{
	/* TODO: Remove mdelay() to prevent listening */
	/*       music delay on BT Headset via A2DP   */
	disable_irq(irq);
	schedule_work(&the_avr_data->work);
	enable_irq(irq);
	return IRQ_HANDLED;
}

static int __init avr_register_input(struct input_dev *input)
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
	uint8_t data_buf[2] = {0};
	struct i2c_client *client = the_avr_data->client;
	uint32_t bl_lvl;

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
	case IOCTL_SIMPLE_TEST_ON:
		bsimple_test = true;
		pr_debug("[AVR] IOCTL_bSIMPLE_TEST_ON. \n");
		break;
	case IOCTL_SIMPLE_TEST_OFF:
		bsimple_test = false;
		pr_debug("[AVR] IOCTL_bSIMPLE_TEST_ON. \n");
		break;
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
		avr_backlight_set_brightness(AVR_BKL_ON);
		pr_debug("[AVR] IOCTL_SET_BL_ON. \n");
		break;
	case IOCTL_SET_BL_OFF:
		avr_backlight_set_brightness(AVR_BKL_OFF);
		pr_debug("[AVR] IOCTL_SET_BL_OFF. \n");
		break;
	case IOCTL_SET_BL_LV:
		// Good enough :)
		bl_lvl = (arg + 1) / 8;

		avr_backlight_set_brightness(bl_lvl);
		pr_debug("[AVR] IOCTL_SET_BL_LV, Set backlight 0x%02X. \n", bl_lvl);
		break;
	case IOCTL_KEY_LOCK_TOGGLE:
		data_buf[0] = I2C_REG_KEY_LOCK;
		data_buf[1] = (unsigned int)arg;
		if(data_buf[1] != 0 || data_buf[1] != 1){
			data_buf[1]=0;
		}
		i2c_write(client, data_buf);
		break;
	case IOCTL_TEST_KEY:
		/* if touch locked, unlock it!. */
		data_buf[0] = I2C_REG_KEY_STATUS;
		data_buf[1] = (unsigned int)arg;
		if(data_buf[1] <= 0 || data_buf[1] >= 0x80){
			data_buf[1] = 0;
		}
		i2c_write(client, data_buf);
		pr_debug("[AVR] IOCTL_TEST_KEY, Set key as 0x%02X. \n", data_buf[1]);
		return err;
	case IOCTL_TEST_KEY_UP:
		pr_debug("[AVR] IOCTL_TEST_KEY_UP, KEY %d UP! \n", (unsigned int)arg);
		input_report_key(the_avr_data->input, (unsigned int)arg, 0);
		break;
	case IOCTL_TEST_KEY_DOWN:
		pr_debug("[AVR] IOCTL_TEST_KEY_DOWN, KEY %d DOWN! \n", (unsigned int)arg);
		input_report_key(the_avr_data->input, (unsigned int)arg, 1);
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

/*
 * client: target client
 * buf: target register
 * count: length of response
 */
static int i2c_read(struct i2c_client *client, char *buf, int count)
{
	/* Send target reg. info. */
	if(1 != i2c_master_send(client, buf, 1)){
		return -1;
	}

	/* Get response data and set to buf */
	if(count != i2c_master_recv(client, buf, count)){
		return -1;
	}
	return 0;
}

/*
 * client: target client
 * buf: target register with command
 */
static int i2c_write(struct i2c_client *client, char *buf)
{
	/* buf[0] -> target reg. info. */
	/* buf[1] -> cmd */
	if(2 != i2c_master_send(client, buf, 2)){
		return -1;
	}
	return 0;
}

static void led_on(struct i2c_client *client){
	uint8_t data_buf[2] = {0};

	data_buf[0] = I2C_REG_LED_1;
	data_buf[1] = 0x2; // Low power
	i2c_write(client, data_buf);
}

static void led_off(struct i2c_client *client){
	uint8_t data_buf[2] = {0};

	data_buf[0] = I2C_REG_LED_1;
	data_buf[1] = AVR_LED_OFF;
	i2c_write(client, data_buf);
}

static void low_power_mode(struct i2c_client *client, int mode){
	uint8_t data_buf[2] = {0};

	if (mode) {
		data_buf[0] = I2C_REG_LOW_POWER;
		data_buf[1] = AVR_POWER_LOW;
		i2c_write(client, data_buf);
		pr_debug("[AVR] Enter Low Power\n");
	} else {
		data_buf[0] = I2C_REG_LOW_POWER;
		data_buf[1] = AVR_POWER_NORMAL;
		i2c_write(client, data_buf);
		pr_debug("[AVR] Enter Normal power\n");
	}
}

static void key_clear(struct i2c_client *client)
{
	/* To prevent uP holding KEY_INT pin to low without getting value */
	uint8_t data_buf[2] = {0};

	data_buf[0] = I2C_REG_KEY_STATUS;
	data_buf[1] = 0;
	i2c_read(client, data_buf, 1);

	pr_debug("[AVR] Clear Key Value.\n");
}

static void avr_led_work_func(struct work_struct *work)
{
	// to_delayed_work()
	struct delayed_work *led_work = container_of(work, struct delayed_work, work);
	struct avr_data* data = container_of(led_work, struct avr_data, led_work);

	led_off(data->client);
	led_call = false;
}

module_init(avr_init);
module_exit(avr_exit);

MODULE_AUTHOR("Elay Hu <Elay_Hu@acer.com.tw>");
MODULE_DESCRIPTION("AVR micro-P driver");
