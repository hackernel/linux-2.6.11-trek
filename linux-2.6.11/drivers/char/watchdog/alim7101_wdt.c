/*
 *	ALi M7101 PMU Computer Watchdog Timer driver
 *
 *	Based on w83877f_wdt.c by Scott Jennings <linuxdrivers@oro.net>
 *	and the Cobalt kernel WDT timer driver by Tim Hockin
 *	                                      <thockin@cobaltnet.com>
 *
 *	(c)2002 Steve Hill <steve@navaho.co.uk>
 *
 *  This WDT driver is different from most other Linux WDT
 *  drivers in that the driver will ping the watchdog by itself,
 *  because this particular WDT has a very short timeout (1.6
 *  seconds) and it would be insane to count on any userspace
 *  daemon always getting scheduled within that time frame.
 *
 *  Additions:
 *   Aug 23, 2004 - Added use_gpio module parameter for use on revision a1d PMUs
 *                  found on very old cobalt hardware.
 *                  -- Mike Waychison <michael.waychison@sun.com>
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/timer.h>
#include <linux/miscdevice.h>
#include <linux/watchdog.h>
#include <linux/ioport.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/pci.h>

#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/system.h>

#define OUR_NAME "alim7101_wdt"
#define PFX OUR_NAME ": "

#define WDT_ENABLE 0x9C
#define WDT_DISABLE 0x8C

#define ALI_7101_WDT    0x92
#define ALI_7101_GPIO   0x7D
#define ALI_7101_GPIO_O 0x7E
#define ALI_WDT_ARM     0x01

/*
 * We're going to use a 1 second timeout.
 * If we reset the watchdog every ~250ms we should be safe.  */

#define WDT_INTERVAL (HZ/4+1)

/*
 * We must not require too good response from the userspace daemon.
 * Here we require the userspace daemon to send us a heartbeat
 * char to /dev/watchdog every 30 seconds.
 */

#define WATCHDOG_TIMEOUT 30            /* 30 sec default timeout */
static int timeout = WATCHDOG_TIMEOUT; /* in seconds, will be multiplied by HZ to get seconds to wait for a ping */
module_param(timeout, int, 0);
MODULE_PARM_DESC(timeout, "Watchdog timeout in seconds. (1<=timeout<=3600, default=" __MODULE_STRING(WATCHDOG_TIMEOUT) ")");

static int use_gpio = 0; /* Use the pic (for a1d revision alim7101) */
module_param(use_gpio, int, 0);
MODULE_PARM_DESC(use_gpio, "Use the gpio watchdog.  (required by old cobalt boards)");

static void wdt_timer_ping(unsigned long);
static struct timer_list timer;
static unsigned long next_heartbeat;
static unsigned long wdt_is_open;
static char wdt_expect_close;
static struct pci_dev *alim7101_pmu;

#ifdef CONFIG_WATCHDOG_NOWAYOUT
static int nowayout = 1;
#else
static int nowayout = 0;
#endif

module_param(nowayout, int, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default=CONFIG_WATCHDOG_NOWAYOUT)");

/*
 *	Whack the dog
 */

static void wdt_timer_ping(unsigned long data)
{
	/* If we got a heartbeat pulse within the WDT_US_INTERVAL
	 * we agree to ping the WDT
	 */
	char	tmp;

	if(time_before(jiffies, next_heartbeat))
	{
		/* Ping the WDT (this is actually a disarm/arm sequence) */
		pci_read_config_byte(alim7101_pmu, 0x92, &tmp);
		pci_write_config_byte(alim7101_pmu, ALI_7101_WDT, (tmp & ~ALI_WDT_ARM));
		pci_write_config_byte(alim7101_pmu, ALI_7101_WDT, (tmp | ALI_WDT_ARM));
		if (use_gpio) {
			pci_read_config_byte(alim7101_pmu, ALI_7101_GPIO_O, &tmp);
			pci_write_config_byte(alim7101_pmu, ALI_7101_GPIO_O, tmp
					| 0x20);
			pci_write_config_byte(alim7101_pmu, ALI_7101_GPIO_O, tmp
					& ~0x20);
		}
	} else {
		printk(KERN_WARNING PFX "Heartbeat lost! Will not ping the watchdog\n");
	}
	/* Re-set the timer interval */
	timer.expires = jiffies + WDT_INTERVAL;
	add_timer(&timer);
}

/*
 * Utility routines
 */

static void wdt_change(int writeval)
{
	char	tmp;

	pci_read_config_byte(alim7101_pmu, ALI_7101_WDT, &tmp);
	if (writeval == WDT_ENABLE) {
		pci_write_config_byte(alim7101_pmu, ALI_7101_WDT, (tmp | ALI_WDT_ARM));
		if (use_gpio) {
			pci_read_config_byte(alim7101_pmu, ALI_7101_GPIO_O, &tmp);
			pci_write_config_byte(alim7101_pmu, ALI_7101_GPIO_O, tmp & ~0x20);
		}

	} else {
		pci_write_config_byte(alim7101_pmu, ALI_7101_WDT, (tmp & ~ALI_WDT_ARM));
		if (use_gpio) {
			pci_read_config_byte(alim7101_pmu, ALI_7101_GPIO_O, &tmp);
			pci_write_config_byte(alim7101_pmu, ALI_7101_GPIO_O, tmp | 0x20);
		}
	}
}

static void wdt_startup(void)
{
	next_heartbeat = jiffies + (timeout * HZ);

	/* We must enable before we kick off the timer in case the timer
	   occurs as we ping it */

	wdt_change(WDT_ENABLE);

	/* Start the timer */
	timer.expires = jiffies + WDT_INTERVAL;
	add_timer(&timer);


	printk(KERN_INFO PFX "Watchdog timer is now enabled.\n");
}

static void wdt_turnoff(void)
{
	/* Stop the timer */
	del_timer_sync(&timer);
	wdt_change(WDT_DISABLE);
	printk(KERN_INFO PFX "Watchdog timer is now disabled...\n");
}

static void wdt_keepalive(void)
{
	/* user land ping */
	next_heartbeat = jiffies + (timeout * HZ);
}

/*
 * /dev/watchdog handling
 */

static ssize_t fop_write(struct file * file, const char __user * buf, size_t count, loff_t * ppos)
{
	/* See if we got the magic character 'V' and reload the timer */
	if(count) {
		if (!nowayout) {
			size_t ofs;

			/* note: just in case someone wrote the magic character
			 * five months ago... */
			wdt_expect_close = 0;

			/* now scan */
			for (ofs = 0; ofs != count; ofs++) {
				char c;
				if (get_user(c, buf+ofs))
					return -EFAULT;
				if (c == 'V')
					wdt_expect_close = 42;
			}
		}
		/* someone wrote to us, we should restart timer */
		wdt_keepalive();
	}
	return count;
}

static int fop_open(struct inode * inode, struct file * file)
{
	/* Just in case we're already talking to someone... */
	if(test_and_set_bit(0, &wdt_is_open))
		return -EBUSY;
	/* Good, fire up the show */
	wdt_startup();
	return nonseekable_open(inode, file);
}

static int fop_close(struct inode * inode, struct file * file)
{
	if(wdt_expect_close == 42)
		wdt_turnoff();
	else {
		/* wim: shouldn't there be a: del_timer(&timer); */
		printk(KERN_CRIT PFX "device file closed unexpectedly. Will not stop the WDT!\n");
	}
	clear_bit(0, &wdt_is_open);
	wdt_expect_close = 0;
	return 0;
}

static int fop_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int __user *p = argp;
	static struct watchdog_info ident =
	{
		.options = WDIOF_KEEPALIVEPING | WDIOF_SETTIMEOUT | WDIOF_MAGICCLOSE,
		.firmware_version = 1,
		.identity = "ALiM7101",
	};

	switch(cmd)
	{
		case WDIOC_GETSUPPORT:
			return copy_to_user(argp, &ident, sizeof(ident))?-EFAULT:0;
		case WDIOC_GETSTATUS:
		case WDIOC_GETBOOTSTATUS:
			return put_user(0, p);
		case WDIOC_KEEPALIVE:
			wdt_keepalive();
			return 0;
		case WDIOC_SETOPTIONS:
		{
			int new_options, retval = -EINVAL;

			if(get_user(new_options, p))
				return -EFAULT;

			if(new_options & WDIOS_DISABLECARD) {
				wdt_turnoff();
				retval = 0;
			}

			if(new_options & WDIOS_ENABLECARD) {
				wdt_startup();
				retval = 0;
			}

			return retval;
		}
		case WDIOC_SETTIMEOUT:
		{
			int new_timeout;

			if(get_user(new_timeout, p))
				return -EFAULT;

			if(new_timeout < 1 || new_timeout > 3600) /* arbitrary upper limit */
				return -EINVAL;

			timeout = new_timeout;
			wdt_keepalive();
			/* Fall through */
		}
		case WDIOC_GETTIMEOUT:
			return put_user(timeout, p);
		default:
			return -ENOIOCTLCMD;
	}
}

static struct file_operations wdt_fops = {
	.owner=		THIS_MODULE,
	.llseek=	no_llseek,
	.write=		fop_write,
	.open=		fop_open,
	.release=	fop_close,
	.ioctl=		fop_ioctl,
};

static struct miscdevice wdt_miscdev = {
	.minor=WATCHDOG_MINOR,
	.name="watchdog",
	.fops=&wdt_fops,
};

/*
 *	Notifier for system down
 */

static int wdt_notify_sys(struct notifier_block *this, unsigned long code, void *unused)
{
	if (code==SYS_DOWN || code==SYS_HALT)
		wdt_turnoff();

	if (code==SYS_RESTART) {
		/*
		 * Cobalt devices have no way of rebooting themselves other than
		 * getting the watchdog to pull reset, so we restart the watchdog on
		 * reboot with no heartbeat
		 */
		wdt_change(WDT_ENABLE);
		printk(KERN_INFO PFX "Watchdog timer is now enabled with no heartbeat - should reboot in ~1 second.\n");
	}
	return NOTIFY_DONE;
}

/*
 *	The WDT needs to learn about soft shutdowns in order to
 *	turn the timebomb registers off.
 */

static struct notifier_block wdt_notifier=
{
	.notifier_call = wdt_notify_sys,
};

static void __exit alim7101_wdt_unload(void)
{
	wdt_turnoff();
	/* Deregister */
	misc_deregister(&wdt_miscdev);
	unregister_reboot_notifier(&wdt_notifier);
}

static int __init alim7101_wdt_init(void)
{
	int rc = -EBUSY;
	struct pci_dev *ali1543_south;
	char tmp;

	printk(KERN_INFO PFX "Steve Hill <steve@navaho.co.uk>.\n");
	alim7101_pmu = pci_find_device(PCI_VENDOR_ID_AL, PCI_DEVICE_ID_AL_M7101,NULL);
	if (!alim7101_pmu) {
		printk(KERN_INFO PFX "ALi M7101 PMU not present - WDT not set\n");
		return -EBUSY;
	}

	/* Set the WDT in the PMU to 1 second */
	pci_write_config_byte(alim7101_pmu, ALI_7101_WDT, 0x02);

	ali1543_south = pci_find_device(PCI_VENDOR_ID_AL, PCI_DEVICE_ID_AL_M1533, NULL);
	if (!ali1543_south) {
		printk(KERN_INFO PFX "ALi 1543 South-Bridge not present - WDT not set\n");
		return -EBUSY;
	}
	pci_read_config_byte(ali1543_south, 0x5e, &tmp);
	if ((tmp & 0x1e) == 0x00) {
		if (!use_gpio) {
			printk(KERN_INFO PFX "Detected old alim7101 revision 'a1d'.  If this is a cobalt board, set the 'use_gpio' module parameter.\n");
			return -EBUSY;
		} 
		nowayout = 1;
	} else if ((tmp & 0x1e) != 0x12 && (tmp & 0x1e) != 0x00) {
		printk(KERN_INFO PFX "ALi 1543 South-Bridge does not have the correct revision number (???1001?) - WDT not set\n");
		return -EBUSY;
	}

	if(timeout < 1 || timeout > 3600) /* arbitrary upper limit */
	{
		timeout = WATCHDOG_TIMEOUT;
		printk(KERN_INFO PFX "timeout value must be 1<=x<=3600, using %d\n",
			timeout);
	}

	init_timer(&timer);
	timer.function = wdt_timer_ping;
	timer.data = 1;

	rc = misc_register(&wdt_miscdev);
	if (rc) {
		printk(KERN_ERR PFX "cannot register miscdev on minor=%d (err=%d)\n",
			wdt_miscdev.minor, rc);
		goto err_out;
	}

	rc = register_reboot_notifier(&wdt_notifier);
	if (rc) {
		printk(KERN_ERR PFX "cannot register reboot notifier (err=%d)\n",
			rc);
		goto err_out_miscdev;
	}

	if (nowayout) {
		__module_get(THIS_MODULE);
	}

	printk(KERN_INFO PFX "WDT driver for ALi M7101 initialised. timeout=%d sec (nowayout=%d)\n",
		timeout, nowayout);
	return 0;

err_out_miscdev:
	misc_deregister(&wdt_miscdev);
err_out:
	return rc;
}

module_init(alim7101_wdt_init);
module_exit(alim7101_wdt_unload);

MODULE_AUTHOR("Steve Hill");
MODULE_DESCRIPTION("ALi M7101 PMU Computer Watchdog Timer driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS_MISCDEV(WATCHDOG_MINOR);
