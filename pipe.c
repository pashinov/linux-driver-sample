#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/ioctl.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>

#define DRIVER_NAME "pipe-drv"
#define PROCFS_NAME "pipe-drv-proc"

#define MAGIC_NUM	1
#define IOC_MAXNR	5
#define IOC_SET		_IOW(MAGIC_NUM, 0, int)
#define IOC_GET		_IOR(MAGIC_NUM, 1, int)
#define IOC_RUN		_IO(MAGIC_NUM, 2)
#define IOC_STOP	_IO(MAGIC_NUM, 3)

#define BUF_SIZE 	1024

static int major_number = 0;
static int minor_number = 0;

static int increment = 0;
static int period_inc_ms = 1000;

static struct device *device;
static struct class *ppdev_class;

static struct task_struct *ts;
static struct proc_dir_entry *our_proc_file;

static int offset = 0;
static char data_buf[BUF_SIZE];

struct pipe_dev {
	char *data_buf;
	int device_open;
	struct cdev cdev;
};

struct pipe_dev dev;

static int thread_increment(void *data) {
	while(1) {
		increment++; msleep(period_inc_ms);
		if (kthread_should_stop()) { break; }
	}
	return 0;
}

static int pipe_open(struct inode *pinode, struct file *filp) {
	if (dev.device_open) { return -EBUSY; }
	dev.device_open++;
	return 0;
}

static ssize_t pipe_write(struct file *filp, const char __user *buf, size_t len, loff_t *off) {
	int res = 0;
	int temp = 0;
	if ((sizeof(data_buf) - offset) < len) {
		res = copy_from_user(data_buf + offset, buf, sizeof(data_buf) - offset);
		temp = sizeof(data_buf) - offset;
		offset = sizeof(data_buf);
		return temp;
	} else {
		res = copy_from_user(data_buf + offset, buf, len);
		offset = offset + len;
		return len;
	}
}

static ssize_t pipe_read(struct file *filp, char __user *buf, size_t len, loff_t *off) {
	int i = 0;
	int res = 0;
	int temp = 0;
	if (offset < len) {
		res = copy_to_user(buf, data_buf, offset);
		temp = offset;
		offset = 0;
		return temp;
	} else {
		char temp_buf[offset - len];
		res = copy_to_user(buf, data_buf, len);
		for (i = 0; i < offset - len; i++) {
			temp_buf[i] = data_buf[len + i];
		}
		memset(data_buf, 0, sizeof(data_buf));
		for (i = 0; i < offset - len; i++) {
			data_buf[i] = temp_buf[i];
		}
		offset = offset - len;
		return len;
	}
}

static long pipe_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	if (_IOC_TYPE(cmd) != MAGIC_NUM) return -ENOTTY;
	if (_IOC_NR(cmd) > IOC_MAXNR) return -ENOTTY;

	switch(cmd) {

		case IOC_SET:
			copy_from_user(&period_inc_ms, (void*)arg, sizeof(period_inc_ms));
			break;

		case IOC_GET:
			copy_to_user((void*)arg, &increment, sizeof(increment));
			break;

		case IOC_RUN:
			ts = kthread_run(thread_increment, NULL, "start increment thread");
			break;

		case IOC_STOP:
			kthread_stop(ts);
			break;

		default :
			return -ENOTTY;
	}

	return 0;
}

static int pipe_close(struct inode *pinode, struct file *filp) {
	dev.device_open--;
	return 0;
}

static ssize_t procfile_read(struct file *filp, char *buffer, size_t length, loff_t *offset) {
	int ret = 0;
	static int finished = 0;

	printk(KERN_INFO "procfile_read (/proc/%s) called\n", PROCFS_NAME);

	if (finished) {
		printk(KERN_INFO "procfs_read: END\n");
		finished = 0;
		return 0;
	}

	finished = 1;
	ret = sprintf(buffer, "%d\n", increment);

	return ret;
}

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = pipe_open,
	.write = pipe_write,
	.read = pipe_read,
	.unlocked_ioctl = pipe_ioctl,
	.release = pipe_close,
};

static struct file_operations proc_fops = {
	.owner = THIS_MODULE,
	.read = procfile_read,
};

static char *mydevnode(struct device *dev, umode_t *mode) {
	if(mode) {
		*mode = 0666;
	}
	return 0;
}

static int __init pipe_init(void) {
	int res = 0;
	dev_t devno = 0;
	dev.device_open = 0;
	memset(data_buf, 0, sizeof(data_buf));

	our_proc_file = proc_create(PROCFS_NAME, 0666, NULL, &proc_fops);
	if (our_proc_file == NULL) {
		remove_proc_entry(PROCFS_NAME, our_proc_file);
		printk(KERN_ALERT "Error: Could not initialize /proc/%s\n", PROCFS_NAME);
		goto out_proc;
	}

	proc_set_user(our_proc_file, KUIDT_INIT(0), KGIDT_INIT(0));
	proc_set_size(our_proc_file, 37);

	res = alloc_chrdev_region(&devno, minor_number, 1, DRIVER_NAME);
	if (res) {
		goto out_alloc;
	}
	major_number = MAJOR(devno);

	ppdev_class = class_create(THIS_MODULE, DRIVER_NAME);
	if (IS_ERR(ppdev_class)) {
		goto out_chrdev;
	}
	ppdev_class->devnode=mydevnode;

	cdev_init(&dev.cdev, &fops);
	dev.cdev.owner = THIS_MODULE;
	dev.cdev.ops = &fops;
	res = cdev_add(&dev.cdev, devno, 1);
	if (res < 0) {
		goto out_chrdev;
	}

	device = device_create(ppdev_class, NULL, MKDEV(major_number, minor_number), NULL, DRIVER_NAME);
	if (IS_ERR(device)) {
		goto out_class;
	}

	printk(KERN_INFO "%s init - major: %d\r\n", DRIVER_NAME, major_number);

	return 0;

out_chrdev:
	unregister_chrdev_region(devno, 1);

out_class:
	class_destroy(ppdev_class);
	cdev_del(&dev.cdev);

out_proc:
	return -ENOMEM;

out_alloc:
	return res;
}

static void __exit pipe_exit(void) {
	dev_t devno = 0;
	devno = MKDEV(major_number, minor_number);
	remove_proc_entry(PROCFS_NAME, NULL);
	device_destroy(ppdev_class, devno);
	cdev_del(&dev.cdev);
	class_destroy(ppdev_class);
	printk(KERN_INFO "unrigester %s driver\n", DRIVER_NAME);
	return unregister_chrdev_region(devno, 1);
}

module_init(pipe_init);
module_exit(pipe_exit);

MODULE_DESCRIPTION("Linux device driver sample");
MODULE_AUTHOR("Alexey Pashinov <pashinov@outlook.com>");
MODULE_VERSION("1.0");
MODULE_LICENSE("MIT");
