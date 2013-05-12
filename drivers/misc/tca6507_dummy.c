/*
 * Dummy driver for /dev/tca6507
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <asm/uaccess.h>

#define LED_DEV_IOCTLID		0x12
#define IOC_MAXNR		4
#define IOCTL_SET_MID_LED	_IOW(LED_DEV_IOCTLID, 1, int)
#define IOCTL_SET_RIGHT_LED	_IOW(LED_DEV_IOCTLID, 2, int)
#define IOCTL_SET_LEFT_LED	_IOW(LED_DEV_IOCTLID, 3, int)
#define IOCTL_SET_ALL_LED	_IOW(LED_DEV_IOCTLID, 4, int)

#define LED_OFF			0
#define LED_SLOW_BLINKING	1
#define LED_FAST_BLINKING	2
#define LED_ON			3

static int tca6507_dummy_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int tca6507_dummy_close(struct inode *inode, struct file *file)
{
	return 0;
}

static int tca6507_dummy_ioctl(struct inode *inode, struct file *file,
				unsigned int cmd, unsigned long arg)
{
	int rc = 0;
	char *ioctl_name;
	char *led_modes[] = { "Off", "Slow blinking", "Fast blinking", "On" };

	if (_IOC_TYPE(cmd) != LED_DEV_IOCTLID){
               pr_err("%s: cmd magic type error\n", __func__);
               return -ENOTTY;
	}

	if (_IOC_NR(cmd) > IOC_MAXNR){
		pr_err("%s: ioctl %d is not supported\n", __func__, cmd);
		return -ENOTTY;
	}

	if (_IOC_DIR(cmd) & _IOC_READ)
		rc = !access_ok(VERIFY_WRITE,(void __user*)arg, _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		rc = !access_ok(VERIFY_READ, (void __user*)arg, _IOC_SIZE(cmd));

	if (rc) {
		pr_err("%s: access_ok error\n", __func__);
		return -EFAULT;
	}

	switch (cmd) {
	case IOCTL_SET_MID_LED:
		ioctl_name = "IOCTL_SET_MID_LED";
		break;
	case IOCTL_SET_RIGHT_LED:
		ioctl_name = "IOCTL_SET_RIGHT_LED";
		break;
	case IOCTL_SET_LEFT_LED:
		ioctl_name = "IOCTL_SET_LEFT_LED";
		break;
	case IOCTL_SET_ALL_LED:
		ioctl_name = "IOCTL_SET_ALL_LED";
		break;
	default:
		ioctl_name = "(unknown)";
	}

	pr_info("%s: dummy %s %s\n", __func__, ioctl_name, led_modes[arg]);

	return 0;
}

static struct file_operations tca6507_dummy_ops = {
	.owner = THIS_MODULE,
	.open = tca6507_dummy_open,
	.release = tca6507_dummy_close,
	.ioctl = tca6507_dummy_ioctl,
};

static struct miscdevice tca6507_dummy_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "tca6507",
	.fops = &tca6507_dummy_ops,
};

static int __init tca6507_dummy_init(void)
{
	int rc;

	rc = misc_register(&tca6507_dummy_dev);
	if (rc) {
		pr_err("%s: misc_register failed\n", __func__);
		return rc;
	}
	return 0;
}
module_init(tca6507_dummy_init);

static void __exit tca6507_dummy_exit(void)
{
       misc_deregister(&tca6507_dummy_dev);
}
module_exit(tca6507_dummy_exit);

MODULE_AUTHOR("Roman Yepishev");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Dummy /dev/tca6507 device driver");
