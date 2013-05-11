#define DEBUG

#include <linux/device.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>

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

#define AVR_DRIVER_NAME           "avr"

static int __init avr_dummy_init(void);
static int avr_open(struct inode *inode, struct file *file);
static int avr_close(struct inode *inode, struct file *file);
static int avr_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg);

/*File operation of AVR device file */
static const struct file_operations avr_fops = {
	.owner     = THIS_MODULE,
	.open      = avr_open,
	.release   = avr_close,
	.ioctl     = avr_ioctl,
};

static struct miscdevice avr_dummy_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = AVR_DRIVER_NAME,
	.fops = &avr_fops,
};

static int __init avr_dummy_init(void)
{
	int rc;
/* ioctl interface */
	rc = misc_register(&avr_dummy_dev);
	if (rc) {
		pr_err("%s: misc_register failed\n", __func__);
		return -1;
	}

	return 0;
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
	char *ioctl_name;

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

	/* cmd mapping */
	switch(cmd){
	case IOCTL_SET_LED_ON:
		ioctl_name = "IOCTL_SET_LED_ON";
		break;
	case IOCTL_SET_LED_OFF:
		ioctl_name = "IOCTL_SET_LED_OFF";
		break;
	case IOCTL_SET_BL_ON:
		ioctl_name = "IOCTL_SET_BL_ON";
		break;
	case IOCTL_SET_BL_OFF:
		ioctl_name = "IOCTL_SET_BL_OFF";
		break;
	case IOCTL_SET_BL_LV:
		ioctl_name = "IOCTL_SET_BL_LV";
		break;
	case IOCTL_KEY_LOCK_TOGGLE:
		ioctl_name = "IOCTL_KEY_LOCK_TOGGLE";
		break;
	default:
		pr_info("%s: unknown ioctl %d %ld\n", __func__, cmd, arg);
		return -1;
	}

	pr_info("%s: dummy ioctl %s %ld\n", __func__, ioctl_name, arg);
	return 0;
}

static void __exit avr_dummy_exit(void)
{
	misc_deregister(&avr_dummy_dev);
}

module_init(avr_dummy_init);
module_exit(avr_dummy_exit);

MODULE_AUTHOR("Elay Hu <Elay_Hu@acer.com.tw>");
MODULE_DESCRIPTION("AVR micro-P driver");
