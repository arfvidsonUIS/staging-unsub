/* visordiag_main.c
 *
 * Copyright (C) 2010 - 2013 UNISYS CORPORATION
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 */

#include "visordiag_private.h"
#include "easyproc.h"
#include "diagnostics/appos_subsystems.h"
#include "uisutils.h"
#include <linux/time.h>
#include <linux/timex.h>
#include <linux/timer.h>
#include <linux/rtc.h>
#include <linux/utsname.h>
#include <linux/version.h>
#include <linux/platform_device.h>

#define SVLOG_ENABLE(x)

#define MSGRETRYMAX 100
#define MSECS_FOR_DSET_THROTTLING 250
#define MAX_THROTTLE_TRIES 1200	/* (1200 * .25 sec = 5 min) */
static ulong visordiag_channeladdress;
static int visordiag_major;

static spinlock_t devnopool_lock;
static void *dev_no_pool;	/**< pool to grab device numbers from */
static enum {
	SPTYPE_UNDECIDED,
	SPTYPE_GENERIC,
} sp_type = SPTYPE_UNDECIDED;

static inline char *
sptype_to_s(int sptype)
{
	switch (sptype) {
	case SPTYPE_UNDECIDED:
		return "UNDECIDED";
	case SPTYPE_GENERIC:
		return "GENERIC";
	default:
		return "???";
	}
	return "???";
}

static int visordiag_probe(struct visor_device *dev);
static void visordiag_remove(struct visor_device *dev);

static int visordiag_file_open(struct inode *inode, struct file *file);
static int visordiag_file_release(struct inode *inode, struct file *file);
static ssize_t visordiag_file_write_guts(struct file *file,
					 const char __user *buf,
					 size_t count, loff_t *ppos,
					 int default_pri);
static ssize_t visordiag_file_write(struct file *file,
				    const char __user *buf,
				    size_t count, loff_t *ppos);
static int visordiag_mmap(struct file *file, struct vm_area_struct *vma);
static ssize_t visordiag_file_xfer(struct file *file,
				   const char __user *buf,
				   size_t count, u8 *module_name);

static int
simplebus_match(struct device *xdev, struct device_driver *xdrv)
{
	return 1;
}

/** This describes the TYPE of bus.
 *  (Don't confuse this with an INSTANCE of the bus.)
 */
static struct bus_type simplebus_type = {
	.name = "visordiag",
	.match = simplebus_match,
};

static struct visor_device *standalone_device;

/* /sys/devices/platform/visordiag */
static struct platform_device visordiag_platform_device_template = {
	.name = "visordiag",
	.id = -1,
};

enum {
	CHRDEV_FIRST,
	CHRDEV_LOGALL = CHRDEV_FIRST,	/* default is DIAG_SEVERITY_INFO */
	CHRDEV_LOGVER,		/* default is DIAG_SEVERITY_VERBOSE */
	CHRDEV_LOGINF,		/* default is DIAG_SEVERITY_INFO */
	CHRDEV_LOGWRN,		/* default is DIAG_SEVERITY_WARNING */
	CHRDEV_LOGERR,		/* default is DIAG_SEVERITY_ERR */
	CHRDEV_PLAT_DIAG,	/* Used for file transfer of diag info
				 * (e.g. DSET & MegaSAS info) */
	CHRDEV_DUMP,		/* TBD: Placeholder for future
				 * development. Plan is for it to be
				 * used for partition messages
				 * indicating progress of ldump */
	CHRDEV_LASTPLUS1
} char_device_types;
#define NCHARDEVICES (CHRDEV_LASTPLUS1-CHRDEV_FIRST)
static const struct file_operations visordiag_fops = {
	.owner = THIS_MODULE,
	.open = visordiag_file_open,
	.write = visordiag_file_write,
	.release = visordiag_file_release,
	.mmap = visordiag_mmap,
};

/** These are all the counters we maintain for each device.
 *  They will all be reported under /sys/bus/visorbus/devices/<devicename>.
 */
struct DEVDATA_COUNTERS {
	u64 host_messages_out;  /**< \# messages we have output to the host */
	u64 host_messages_out_failed;  /**< \# messages we have failed to
				     *   output to the host */
	u64 umode_bytes_in;  /**< \# bytes we have input from user mode */
};

/** These are all the devdata properties we maintain for each device.
 *  They will all be reported under /sys/bus/visorbus/devices/<devicename>.
 */
enum visordiag_devdata_properties {
	prop_open_file_count,
	/* Add items above, but don't forget to modify
	 * register_devdata_attributes whenever you do...
	 */
	prop_DEVDATAMAX
};

/** This is the private data that we store for each device.
 *  A pointer to this struct is kept in each "struct device", and can be
 *  obtained using visor_get_drvdata(dev).
 */
struct visordiag_devdata {
	int devno;
	struct visor_device *dev;

	/** lock for dev */
	struct rw_semaphore lock_visor_dev;
	char name[99];
	struct list_head list_all;   /**< link within list_all_devices list */

	/** head of list of visordiag_filedata structs, linked via the
	 *  list_all member */
	struct list_head list_files;
	uint open_file_count;

	/** lock for list_files */
	rwlock_t lock_files;

	/** lock for open_file_count */
	struct rw_semaphore lock_open_file_count;
	struct DEVDATA_COUNTERS counter;
	struct device_attribute devdata_property[prop_DEVDATAMAX];
	struct kref kref;
	struct easyproc_device_info procinfo;
	int xmitqueue;

	struct diag_channel_protocol_header __iomem *diag_channel_header;
	struct diag_channel_protocol_header dummy_diag_channel_header;
	dev_t devt; /* major, minor for first of NCHARDEVICES devices */
	BOOL char_device_registered;
	struct cdev cdev_diag;
	struct {
		struct platform_device platform_device;
		BOOL platform_device_registered;
	} char_devices[NCHARDEVICES];
	BOOL last_send_was_good;
	uint dropped_msg_cnt;
	unsigned long long start_cycles;
};

/** List of all visordiag_devdata structs, linked via the list_all member */
static LIST_HEAD(list_all_devices);
static DEFINE_SPINLOCK(lock_all_devices);

/** This is the private data that we store for each file descriptor that is
 *  opened to the diag character device.
 */
struct visordiag_filedata {
	struct visordiag_devdata *devdata;
	/** link within devdata.list_files list */
	struct list_head list_all;
	unsigned char buf[NFILEWRITEBYTESTOBUFFER];
	uint nbuf;
	uint offset;		/* For file transfers the offset
				 * within the file */
	int minor;
};

static char __iomem *subsystem_severity_filter_global;

char __iomem *
visordiag_get_severityfilter(void)
{
	/* *** WARNING *** */
	/* There is no reliable way to know that the
	 * subsystem_severity_filter_global address is still pointing to
	 * the correct location, or that the diagChannel even exists at
	 * this point.  TBD: Implement a use_count mechanism in the
	 * lower-level functions (e.g.  visorchannel_get_memregion and
	 * memregion_get_pointer to prevent the channel from
	 * dissapearing out from under us.  For now, just return the
	 * subsystem_severity_filter_global in blind faith. */
	return subsystem_severity_filter_global;
}
EXPORT_SYMBOL_GPL(visordiag_get_severityfilter);

void
visordiag_release_severityfilter(char *filter)
{
	/* Do nothing for now... */
	/* TBD: Decrement use_count as described in */
	/* visordiag_get_severityfilter above. */
}
EXPORT_SYMBOL_GPL(visordiag_release_severityfilter);

static void new_message_to_host(void *context,
				struct diag_channel_event *event);
static void destroy_file(struct visordiag_filedata *filedata);
static void host_side_disappeared(struct visordiag_devdata *devdata);
static void visordiag_online(struct visordiag_devdata *devdata);
static void visordiag_offline(struct visordiag_devdata *devdata);
static void free_char_devices(struct visordiag_devdata *devdata);

/*  DEVICE attributes
 *
 *  define & implement display of device attributes under
 *  /sys/bus/visorbus/devices/<devicename>.
 *
 */

static ssize_t
devdata_property_show(struct device *ddev,
		      struct device_attribute *attr, char *buf)
{
	struct visordiag_devdata *devdata = dev_get_drvdata(ddev);
	ulong offset = (ulong)(attr) - (ulong)(devdata->devdata_property);
	ulong ix = offset / sizeof(struct device_attribute);

	if (ix >= prop_DEVDATAMAX) {
		WARN(1, "%s:%d trouble in paradise; ix=%lu\n",
		     __FILE__, __LINE__, ix);
		return 0;
	}
	switch (ix) {
	case prop_open_file_count:
		return sprintf(buf, "%u\n", devdata->open_file_count);
	default:
		WARN(1, "%s:%d trouble in paradise; ix=%lu\n",
		     __FILE__, __LINE__, ix);
		return 0;
	}
	return 0;
}

static int
register_devdata_attributes(struct visor_device *dev)
{
	int rc = 0, i = 0;
	struct visordiag_devdata *devdata = visor_get_drvdata(dev);
	struct device_attribute *pattr = devdata->devdata_property;

	pattr[prop_open_file_count].attr.name = "open_file_count";
	for (i = 0; i < prop_DEVDATAMAX; i++) {
		pattr[i].attr.mode = S_IRUGO;
		pattr[i].show = devdata_property_show;
		pattr[i].store = NULL;
		rc = device_create_file(&dev->device, &pattr[i]);
		if (rc < 0)
				return rc;
	}
	return rc;
}

static int
register_device_attributes(struct visor_device *dev)
{
	return register_devdata_attributes(dev);
}

static int
unregister_devdata_attributes(struct visor_device *dev)
{
	int i;
	struct visordiag_devdata *devdata = visor_get_drvdata(dev);
	struct device_attribute *pattr = devdata->devdata_property;

	for (i = 0; i < prop_DEVDATAMAX; i++)
		device_remove_file(&dev->device, &pattr[i]);
	return 0;
}

static int
unregister_device_attributes(struct visor_device *dev)
{
	unregister_devdata_attributes(dev);
	return 0;
}

static struct visordiag_devdata *
devdata_create(struct visor_device *dev)
{
	void *rc = NULL;
	struct visordiag_devdata *devdata = NULL;
	int devno = -1;
	int i, errcode;

	devdata = kmalloc(sizeof(*devdata),
			  GFP_KERNEL|__GFP_NORETRY);
	if (devdata == NULL)
			goto away;

	memset(devdata, '\0', sizeof(struct visordiag_devdata));
	cdev_init(&devdata->cdev_diag, NULL);
	spin_lock(&devnopool_lock);
	devno = find_first_zero_bit(dev_no_pool, MAXDEVICES);
	set_bit(devno, dev_no_pool);
	spin_unlock(&devnopool_lock);
	if (devno < 0)
			goto away;

	devdata->devno = devno;
	devdata->dev = dev;
	dev_set_name(&devdata->dev->device, devdata->name);
	devdata->xmitqueue = 0;
	devdata->diag_channel_header =
		(__force struct diag_channel_protocol_header __iomem *)
		&devdata->dummy_diag_channel_header;

	cdev_init(&devdata->cdev_diag, &visordiag_fops);
	devdata->cdev_diag.owner = THIS_MODULE;
	if (visordiag_major == 0) {
		/* dynamic major device number registration required */
		errcode = alloc_chrdev_region(&devdata->devt, /* dest */
					      devdata->devno, /* start minor */
					      NCHARDEVICES,   /* count */
					      MYDRVNAME);
		if (errcode < 0)
				goto away;
		devdata->char_device_registered = TRUE;
	} else {
		/* static major device number registration required */
		devdata->devt = MKDEV(visordiag_major, devdata->devno);
		errcode = register_chrdev_region(devdata->devt,
						 NCHARDEVICES, MYDRVNAME);
		if (errcode < 0)
				goto away;
		devdata->char_device_registered = TRUE;
	}
	errcode = cdev_add(&devdata->cdev_diag, devdata->devt, NCHARDEVICES);
	if (errcode < 0)
			goto away;
	for (i = CHRDEV_FIRST; i < CHRDEV_LASTPLUS1; i++) {
		devdata->char_devices[i].platform_device =
		    visordiag_platform_device_template;
		devdata->char_devices[i].platform_device.id =
		    devdata->devno + i;
		devdata->char_devices[i].platform_device.dev.devt =
		    MKDEV(MAJOR(devdata->devt), devdata->devno + i);
		errcode = platform_device_register(&devdata->char_devices[i]
						   .platform_device);
		if (errcode < 0)
				goto away;
		devdata->char_devices[i].platform_device_registered = TRUE;
	}

	rwlock_init(&devdata->lock_files);
	init_rwsem(&devdata->lock_open_file_count);
	init_rwsem(&devdata->lock_visor_dev);
	INIT_LIST_HEAD(&devdata->list_files);
	kref_init(&devdata->kref);
	devdata->last_send_was_good = TRUE;
	spin_lock(&lock_all_devices);
	list_add_tail(&devdata->list_all, &list_all_devices);
	spin_unlock(&lock_all_devices);

	rc = devdata;
away:
	if (rc == NULL) {
		if (devno >= 0) {
			spin_lock(&devnopool_lock);
			clear_bit(devno, dev_no_pool);
			spin_unlock(&devnopool_lock);
		}
		if (devdata != NULL) {
			free_char_devices(devdata);
			kfree(devdata);
		}
	}
	return rc;
}

static void
free_char_devices(struct visordiag_devdata *devdata)
{
	int i;

	for (i = CHRDEV_FIRST; i < CHRDEV_LASTPLUS1; i++) {
		if (devdata->char_devices[i].platform_device_registered) {
			platform_device_unregister
			    (&devdata->char_devices[i].platform_device);
			devdata->char_devices[i].platform_device_registered =
			    FALSE;
		}
	}
	if (devdata->cdev_diag.ops != NULL)
		cdev_del(&devdata->cdev_diag);
	devdata->cdev_diag.ops = NULL;
	if (devdata->char_device_registered) {
		unregister_chrdev_region(devdata->devt, NCHARDEVICES);
		devdata->char_device_registered = FALSE;
	}
}

static void
devdata_release(struct kref *mykref)
{
	struct visordiag_devdata *devdata =
		container_of(mykref, struct visordiag_devdata, kref);
	spin_lock(&devnopool_lock);
	clear_bit(devdata->devno, dev_no_pool);
	spin_unlock(&devnopool_lock);
	spin_lock(&lock_all_devices);
	list_del(&devdata->list_all);
	spin_unlock(&lock_all_devices);
	free_char_devices(devdata);
	kfree(devdata);
}

static void
devdata_put(struct visordiag_devdata *devdata)
{
	kref_put(&devdata->kref, devdata_release);
}

static void
devdata_get(struct visordiag_devdata *devdata)
{
	kref_get(&devdata->kref);
}

static int
visordiag_probe(struct visor_device *dev)
{
	int rc = 0;
	struct visordiag_devdata *devdata = NULL;
	struct memregion *r = NULL;

	struct spar_diag_channel_protocol __iomem *p = NULL;

	devdata = devdata_create(dev);
	if (devdata == NULL) {
		rc = -1;
		goto away;
	}
	if (!SPAR_DIAG_CHANNEL_OK_CLIENT(
			visorchannel_get_header(dev->visorchannel))) {
		rc = -1;
		goto away;
	}

	r = visorchannel_get_memregion(dev->visorchannel);
	if (r)
		p = (struct spar_diag_channel_protocol __iomem *)
			visor_memregion_get_pointer(r);

	if (p) {
		devdata->diag_channel_header = &p->diag_channel_header;

		/* Store the address of the Filter for access via
		 * visordiag_get_severityfilter
		 */
		subsystem_severity_filter_global =
		    devdata->diag_channel_header->subsystem_severity_filter;
	}

	visor_set_drvdata(dev, devdata);
	if (register_device_attributes(dev) < 0) {
		rc = -1;
		goto away;
	}
	visordiag_online(devdata);

away:
	if (rc < 0) {
		if (devdata != NULL)
			devdata_put(devdata);
	}
	return rc;
}

static void
visordiag_remove(struct visor_device *dev)
{
	struct visordiag_devdata *devdata = visor_get_drvdata(dev);

	if (devdata == NULL)
			return;

	visordiag_offline(devdata);
	unregister_device_attributes(dev);
	visor_set_drvdata(dev, NULL);
	host_side_disappeared(devdata);
	kref_put(&devdata->kref, devdata_release);
}

static void
destroy_visor_device(struct visor_device *dev)
{
	if (dev == NULL)
		return;
	if (dev->visorchannel != NULL) {
		visorchannel_destroy(dev->visorchannel);
		dev->visorchannel = NULL;
	}
	kfree(dev);
}

static void
simplebus_release_device(struct device *xdev)
{
	struct visor_device *dev = to_visor_device(xdev);

	destroy_visor_device(dev);
}

static struct visor_device *
create_visor_device(u64 addr)
{
	struct visor_device *rc = NULL;
	struct visorchannel *visorchannel = NULL;
	struct visor_device *dev = NULL;
	BOOL gotten = FALSE;
	uuid_le guid = SPAR_DIAG_CHANNEL_PROTOCOL_UUID;

	/* prepare chan_hdr (abstraction to read/write channel memory) */
	visorchannel = visorchannel_create(addr, DIAG_CH_SIZE, guid);
	if (visorchannel == NULL)
			goto away;

	dev = kmalloc(sizeof(*dev), GFP_KERNEL|__GFP_NORETRY);
	if (dev == NULL)
			goto away;

	memset(dev, 0, sizeof(struct visor_device));
	dev->visorchannel = visorchannel;
	sema_init(&dev->visordriver_callback_lock, 1);	/* unlocked */
	dev->device.bus = &simplebus_type;
	device_initialize(&dev->device);
	dev->device.release = simplebus_release_device;
	/* keep a reference just for us */
	get_visordev(dev, "create", visordiag_debugref);
	gotten = TRUE;

	/* bus_id must be a unique name with respect to this bus TYPE
	 * (NOT bus instance).  That's why we need to include the bus
	 * number within the name.
	 */
	dev_set_name(&dev->device, "visordiag");

	if (device_add(&dev->device) < 0)
			goto away;

	/* note: device_register is simply device_initialize + device_add */
	rc = dev;
away:
	if (rc == NULL) {
		if (gotten)
			put_visordev(dev, "create", visordiag_debugref);
		if (dev != NULL) {
			destroy_visor_device(dev);
			dev = NULL;
		}
	}
	return rc;
}

static void
visordiag_cleanup_guts(void)
{
	if (standalone_device) {
		standalone_device->being_removed = TRUE;
		/* ensure that the being_removed flag is set before
		 * proceeding
		 */
		wmb();
		visordiag_remove(standalone_device);
		put_visordev(standalone_device, "create", visordiag_debugref);
		device_unregister(&standalone_device->device);
		standalone_device = NULL;
	}
	bus_unregister(&simplebus_type);
	if (dev_no_pool != NULL) {
		kfree(dev_no_pool);
		dev_no_pool = NULL;
	}
}

static int __init
visordiag_init(void)
{
	int rc = -1;
	u64 diag_addr = 0;

	spin_lock_init(&devnopool_lock);
	dev_no_pool = kzalloc(BITS_TO_LONGS(MAXDEVICES), GFP_KERNEL);
	if (dev_no_pool == NULL)
			goto away;

	rc = bus_register(&simplebus_type);
	if (rc < 0)
			goto away;

	if (!visordiag_channeladdress) {
		if (!VMCALL_SUCCESSFUL(issue_vmcall_io_diag_addr(&diag_addr))) {
			rc = -1;
			goto away;
		}
		visordiag_channeladdress = diag_addr;
	}
	standalone_device =
	    create_visor_device(visordiag_channeladdress);
	if (standalone_device == NULL) {
		rc = -1;
		goto away;
	}
	if (visordiag_probe(standalone_device) < 0) {
		put_visordev(standalone_device, "create", visordiag_debugref);
		device_unregister(&standalone_device->device);
		standalone_device = NULL;
		rc = -1;
		goto away;
	}

	rc = 0;
away:
	if (rc < 0)
		visordiag_cleanup_guts();
	return rc;
}

static void
visordiag_cleanup(void)
{
	subsystem_severity_filter_global = NULL;
	visordiag_cleanup_guts();
}

/* Send ACTION=online for DEVPATH=/sys/devices/platform/visordiag. */
static void
visordiag_online(struct visordiag_devdata *devdata)
{
	int i;

	for (i = CHRDEV_FIRST; i < CHRDEV_LASTPLUS1; i++)
		kobject_uevent(&devdata->char_devices[i].platform_device.dev.
			       kobj, KOBJ_ONLINE);
}

/* Send ACTION=offline for DEVPATH=/sys/devices/platform/visordiag. */
static void
visordiag_offline(struct visordiag_devdata *devdata)
{
	int i;

	for (i = CHRDEV_FIRST; i < CHRDEV_LASTPLUS1; i++)
		kobject_uevent(&devdata->char_devices[i].platform_device.dev.
			       kobj, KOBJ_OFFLINE);
}

static struct visordiag_filedata *
create_file(struct visordiag_devdata *devdata, int minor)
{
	void *rc = NULL;
	struct visordiag_filedata *filedata = NULL;

	filedata = kmalloc(sizeof(*filedata),
			   GFP_KERNEL|__GFP_NORETRY);
	if (filedata == NULL) {
		rc = NULL;
		goto away;
	}
	memset(filedata, 0, sizeof(struct visordiag_filedata));
	filedata->devdata = devdata;
	filedata->minor = minor;
	devdata_get(devdata);
	rc = filedata;
away:
	if (rc == NULL) {
		if (filedata != NULL) {
			destroy_file(filedata);
			filedata = NULL;
		}
	}
	return rc;
}

static void
destroy_file(struct visordiag_filedata *filedata)
{
	devdata_put(filedata->devdata);
	kfree(filedata);
}

static int
pri_to_subsystem(int pri)
{
	int facility = SYSLOG_GET_FACILITY(pri);
	int subsys = 0;

	if (facility >= SYSLOG_FAC_LOCAL0) {
		subsys = facility - SYSLOG_FAC_LOCAL0;
		if (subsys > 63)
			subsys = 0;
	}
	return subsys;
}

static int
compute_sptype(void)
{
		return SPTYPE_GENERIC;
}

static void
pri_to_module_name(int pri, char *s, int n)
{
	int facility = SYSLOG_GET_FACILITY(pri);

	if (facility >= SYSLOG_FAC_LOCAL0) {
		int subsys = pri_to_subsystem(pri);

		if (subsys <= SUBSYS_APPOS_MAX) {
			subsys_generic_to_s(subsys, s, n);
		} else {
			switch (sp_type) {
			case SPTYPE_UNDECIDED:
				sp_type = compute_sptype();
				if (sp_type != SPTYPE_UNDECIDED)
					pri_to_module_name(pri, s, n);
				else
					subsys_unknown_to_s(subsys, s, n);
				break;
			default:
				subsys_unknown_to_s(subsys, s, n);
				break;
			}
		}
	} else {
		switch (facility) {
		case SYSLOG_FAC_KERN:
			strncpy(s, "KERN", n);
			break;
		case SYSLOG_FAC_USER:
			strncpy(s, "USER", n);
			break;
		case SYSLOG_FAC_MAIL:
			strncpy(s, "MAIL", n);
			break;
		case SYSLOG_FAC_DAEMON:
			strncpy(s, "DAEMON", n);
			break;
		case SYSLOG_FAC_AUTH:
			strncpy(s, "AUTH", n);
			break;
		case SYSLOG_FAC_SYSLOG:
			strncpy(s, "SYSLOG", n);
			break;
		case SYSLOG_FAC_LPR:
			strncpy(s, "LPR", n);
			break;
		case SYSLOG_FAC_NEWS:
			strncpy(s, "NEWS", n);
			break;
		case SYSLOG_FAC_UUCP:
			strncpy(s, "UUCP", n);
			break;
		case SYSLOG_FAC_CRON:
			strncpy(s, "CRON", n);
			break;
		case SYSLOG_FAC_AUTHPRIV:
			strncpy(s, "AUTHPRIV", n);
			break;
		case SYSLOG_FAC_FTP:
			strncpy(s, "FTP", n);
			break;
		default:
			snprintf(s, n, "facility=%d", facility);
			break;
		}
	}
}

/* This shouldn't be here.
 * to_tm() below was adapted from to_tm() in arch/mips/kernel/time.c.
 * The kernel doesn't generally deal with human-readable time values, but
 * this is one of those exceptions.
 */

#define FEBRUARY                2
#define STARTOFTIME             1970
#define SECDAY                  86400L
#define SECYR                   (SECDAY * 365)
#define leapyear(y)             ((!((y) % 4) && ((y) % 100)) || !((y) % 400))
#define days_in_year(y)         (leapyear(y) ? 366 : 365)
#define days_in_month(m)        (month_days[(m) - 1])

static void
to_tm(time_t tim, struct rtc_time *tm)
{
	long hms, day, gday;
	int i;
	int month_days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	gday = tim / SECDAY;
	day = gday;
	hms = tim % SECDAY;

	/* Hours, minutes, seconds are easy */
	tm->tm_hour = hms / 3600;
	tm->tm_min = (hms % 3600) / 60;
	tm->tm_sec = (hms % 3600) % 60;

	/* Number of years in days */
	for (i = STARTOFTIME; day >= days_in_year(i); i++)
		day -= days_in_year(i);
	tm->tm_year = i;

	/* Number of months in days left */
	if (leapyear(tm->tm_year))
		days_in_month(FEBRUARY) = 29;
	for (i = 1; day >= days_in_month(i); i++)
		day -= days_in_month(i);
	tm->tm_mon = i - 1;	/* tm_mon starts from 0 to 11 */

	/* Days are what is left over (+1) from all that. */
	tm->tm_mday = day + 1;

	/* Determine the day of week */
	tm->tm_wday = (gday + 4) % 7;	/* 1970/1/1 was Thursday */
}

/*
 * Search for the occurrence of this pattern (<digit_string> is a
 * string of 1 or more decimal digits, <c> is ']' or ':'):
 *
 *    <c> <space> '<' <digit_string> '>'
 *
 * That will correctly find the pri in messages we get like this:
 *
 *    Apr 27 08:36:50 spar-sles11-initial kernel:[ 14.898115]
 *    <182>INF:virtnic_open():...
 *
 * If we find the indicated pattern, we will:
 *
 * - return the value of the pri value indicated by the <digit_string>
 * - delete the < digit_string > from the string provided, and adjust
 *   <pdest_bytes> to * indicate the new string length
 *
 * If we do NOT find the indicated pattern, we will simply return the
 * value passed as <default_pri>.
 *
 */
static int
parse_for_pri(char *pdest, int *pdest_bytes, int default_pri)
{
	unsigned long pri = default_pri;
	char *p = pdest;
	int dest_bytes = *pdest_bytes;
	int i;

	for (i = 0; i < dest_bytes;) {
		char *pdigits, *pstart;
		int digit_count = 0;

		if (!((*p == ']') || (*p == ':'))) {
			i++;
			p++;	/* skip over char we don't care about */
			continue;
		}

		/* We found an occurrence of ']' or ':', now pointed
		* to by <p>.  dest_bytes-i indicates how much string
		* we have left at <p>.
		*/
		if ((dest_bytes - i) < 5)
			break;	/* need room for <c>, space, '<', digits, '>' */
		i++;
		p++;		/* pass over ']' or ':' */
		if (*p != ' ')
			continue;
		i++;
		p++;		/* pass over ' ' */
		if (*p != '<')
			continue;
		pstart = p;	/* this will be the start of the
				 * substring to remove */
		i++;
		p++;		/* pass over '<' */
		/* we hope we are pointing at a decimal digit string
		* now, so scan over it */
		pdigits = p;
		while (i < dest_bytes) {
			if (!isdigit(*p))
				break;
			digit_count++;
			i++;
			p++;	/* skip over decimal digit */
		}
		if (digit_count == 0)
			continue;	/* '<' with no digits behind it... */
		if ((i < dest_bytes) && (*p == '>')) {
			ptrdiff_t deleted_bytes;

			/* This is the moment we've all been waiting
			* for, as we have just successfully parsed
			* over '<' digit_string '>'
			*/
			i++;
			p++;	/* pass over '>' */
			if (kstrtoul(pdigits, 10, &pri))
				break;
			/* valid <pri> found Now delete '<' digit_string '>'
			 * from the original string, by writing from <p> thru
			 * the end of the string to <pstart>, and specifying
			 * the new adjusted length for the caller.
			 */
			deleted_bytes = (p - pstart);
			/* note that memmove() allows src and dest to
			* overlap.  dest_bytes-i indicates how much
			* string we have left at <p>.
			*/
			memmove(pstart, p, dest_bytes - i);
			(*pdest_bytes) -= deleted_bytes;

			break;
		}
	}
	return (int)pri;
}

/*  Extract an event from a string.
 *
 *  An event within a string is any sequence of ascii characters, terminated
 *  with '\n'.  Optionally:
 *  - for AppOS messages, this string of ascii characters can be prefixed
 *    by a single '<', a series of ascii digits, and a single '>'.  When this
 *    is found, the digits are interpreted as a syslog <pri> value, which is a
 *    SYSLOG_PRI value in the low 3 bits and a SYSLOG_FAC value in the upper
 *    29 bits.  The SYSLOG_PRI value is used to calculate the severity code of
 *    the event, and the SYSLOG_FAC value is used to compute the modulename of
 *    the event.
 *  - for GuestLinux messages, the above also applies, EXCEPT that the "<" +
 *    digit sequence + ">" can be anywhere in the string (see below for more
 *    details).
 *
 *  Returns 0 if no event was extracted, or the number of characters
 *  consumed from the input string if an event was extracted.
 */
static int
visordiag_extract_event_ex(char *s, int n, struct diag_channel_event *event,
			   int default_pri)
{
	int i = 0, source_bytes = 0, dest_bytes = 0;
	char *p = s;
	unsigned long pri = SYSLOG_MAKE_PRI(SYSLOG_FAC_SYSLOG, default_pri);
	struct timeval time;
	struct rtc_time tm;

	for (i = 0; i < n; i++) {
		if (s[i] == '\n') {
			source_bytes = i + 1;
			dest_bytes = source_bytes - 1;	/* don't include '\n' */
			break;
		}
	}
	if (source_bytes == 0)
		return 0;
	/* An off-the-shelf syslogd/klogd is being used, so the
	 * "<pri>" value is NOT going to be at the beginning of the
	 * log message.  So we do a more thorough search within the
	 * message.
	 */
	pri = parse_for_pri(p, &dest_bytes, pri);
	/* string to log is now at <p> for <dest_bytes> */
	memset(event, 0, sizeof(struct diag_channel_event));
	event->severity = pri_to_severity(pri);
	event->subsystem = pri_to_subsystem(pri);
	pri_to_module_name(pri, event->module_name, sizeof(event->module_name));
	if (dest_bytes >= sizeof(event->additional_info))
		dest_bytes = sizeof(event->additional_info) - 1;

	do_gettimeofday(&time);
	to_tm(time.tv_sec, &tm);
	event->timestamp.year = tm.tm_year;
	event->timestamp.month = tm.tm_mon + 1;
	event->timestamp.day = tm.tm_mday;
	event->timestamp.hour = tm.tm_hour;
	event->timestamp.minute = tm.tm_min;
	event->timestamp.second = tm.tm_sec;
	/* HACK - to be consistent with the timestamps being logged in the
	 * EFI environment, we abuse the nanosecond field to contain the
	 * middle 4 bytes of the 8-byte processor TSC value
	 */
	event->timestamp.nanosecond =
	    (get_cycles() & 0x0000FFFFFFFFFFFFULL) >> 16;
	/* INFODRV("Cycles=0x%-16.16Lx\n", get_cycles()); */
	memcpy(event->additional_info, p, dest_bytes);
	event->additional_info[dest_bytes] = '\0';
	return source_bytes;
}

static void
new_message_to_host(void *context, struct diag_channel_event *event)
{
	struct visordiag_devdata *devdata =
	    (struct visordiag_devdata *)(context);
	BOOL sent = FALSE;
	int tries = 0;
	unsigned long long cur_cycles, elapsed_cycles;

	if (devdata->dev == NULL)
			return;

	if (devdata->last_send_was_good) {
		devdata->start_cycles = (unsigned long long)get_cycles();
		tries = MSGRETRYMAX;
	} else {
		tries = 1;
	}
	while ((!sent) && (tries > 0)) {
		if (visorchannel_signalinsert(devdata->dev->visorchannel,
					      devdata->xmitqueue, event))
			sent = TRUE;
		else
			SLEEPJIFFIES(msecs_to_jiffies(40));
		tries--;
	}
	if (sent) {
		devdata->counter.host_messages_out++;
		if (!devdata->last_send_was_good) {
			cur_cycles = (unsigned long long)get_cycles();
			elapsed_cycles = cur_cycles - devdata->start_cycles;
		}
		devdata->dropped_msg_cnt = 0;
		devdata->last_send_was_good = TRUE;
	} else {
		devdata->counter.host_messages_out_failed++;
		devdata->dropped_msg_cnt++;
		devdata->last_send_was_good = FALSE;
	}
}

static void
host_side_disappeared(struct visordiag_devdata *devdata)
{
	down_write(&devdata->lock_visor_dev);
	sprintf(devdata->name, "<dev#%d-history>", devdata->devno);
	devdata->dev = NULL;	/* indicate device destroyed */
	up_write(&devdata->lock_visor_dev);
}

static void
first_file_opened(struct visordiag_filedata *filedata)
{
}

static void
last_file_closed(struct visordiag_filedata *filedata)
{
}

static int
visordiag_file_open(struct inode *inode, struct file *file)
{
	struct visordiag_devdata *devdata = NULL;
	struct visordiag_filedata *filedata = NULL;
	unsigned minor_number = iminor(inode);
	unsigned major_number = imajor(inode);
	int rc;

	list_for_each_entry(devdata, &list_all_devices, list_all) {
		if (MAJOR(devdata->devt) == major_number) {
			if (minor_number >= NCHARDEVICES) {
				rc = -ENODEV;
				goto away;
			}
			filedata = create_file(devdata, minor_number);
			if (filedata == NULL) {
				rc = -ENOMEM;
				goto away;
			}
			file->private_data = filedata;
			write_lock(&devdata->lock_files);
			list_add_tail(&filedata->list_all,
				      &devdata->list_files);
			write_unlock(&devdata->lock_files);
			down_write(&devdata->lock_open_file_count);
			devdata->open_file_count++;
			if (devdata->open_file_count == 1)
				first_file_opened(filedata);
			up_write(&devdata->lock_open_file_count);
			rc = 0;
			goto away;
		}
	}
	rc = -ENODEV;
away:
	if (rc >= 0) {
		if (file->f_mode & FMODE_WRITE)
				SVLOG_ENABLE(0);
	}
	return rc;
}

static int
visordiag_file_release(struct inode *inode, struct file *file)
{
	int rc;
	struct visordiag_filedata *filedata =
	    (struct visordiag_filedata *)(file->private_data);
	struct visordiag_devdata *devdata = NULL;

	if (filedata == NULL)
			goto away;

	devdata = filedata->devdata;
	if (devdata == NULL)
			goto away;

	/* If this is the Platform Diagnostic device then write a zero
	 * length record which will be used as an indication of end-of-file.
	 */
	if (filedata->minor == CHRDEV_PLAT_DIAG)
		visordiag_file_xfer(file, (char __user *)"", 0,
				    "PlatformDiag");
	down_write(&devdata->lock_open_file_count);
	if (devdata->open_file_count == 1)
		last_file_closed(filedata);
	devdata->open_file_count--;
	up_write(&devdata->lock_open_file_count);
	write_lock(&devdata->lock_files);
	list_del(&filedata->list_all);
	write_unlock(&devdata->lock_files);
	destroy_file(filedata);
	file->private_data = NULL;
	rc = 0;
away:
	if (rc >= 0) {
		if (file->f_mode & FMODE_WRITE) {
			SVLOG_ENABLE(1);
		}
	}
	return rc;
}

static ssize_t
visordiag_file_write(struct file *file,
		     const char __user *buf, size_t count, loff_t *ppos)
{
	struct visordiag_filedata *filedata =
	    (struct visordiag_filedata *)(file->private_data);
	int default_pri;

	switch (filedata->minor) {
	case CHRDEV_LOGVER:
		default_pri = SYSLOG_PRI_DEBUG;
		break;
	case CHRDEV_LOGINF:
		default_pri = SYSLOG_PRI_INFO;
		break;
	case CHRDEV_LOGWRN:
		default_pri = SYSLOG_PRI_WARNING;
		break;
	case CHRDEV_LOGERR:
		default_pri = SYSLOG_PRI_ERR;
		break;

	case CHRDEV_PLAT_DIAG:
		return visordiag_file_xfer(file, buf, count, "PlatformDiag");

	case CHRDEV_DUMP:
	default:
		default_pri = SYSLOG_PRI_INFO;
		break;
	}
	return visordiag_file_write_guts(file, buf, count, ppos, default_pri);
}

static ssize_t
visordiag_file_write_guts(struct file *file,
			  const char __user *buf,
			  size_t count, loff_t *ppos, int default_pri)
{
	int i = 0;
	struct visordiag_filedata *filedata =
	    (struct visordiag_filedata *)(file->private_data);
	struct visordiag_devdata *devdata = NULL;
	struct diag_channel_event event;

	if (filedata == NULL)
			return -1;

	devdata = filedata->devdata;
	if (devdata == NULL)
			return -1;

	if (count > (NFILEWRITEBYTESTOBUFFER - filedata->nbuf))
		count = NFILEWRITEBYTESTOBUFFER - filedata->nbuf;
	if (copy_from_user(filedata->buf + filedata->nbuf, buf, count))
		return -EFAULT;

	devdata->counter.umode_bytes_in += count;
	filedata->nbuf += count;
	down_read(&devdata->lock_visor_dev);
	if (devdata->dev == NULL) {	/* host channel is gone */
		up_read(&devdata->lock_visor_dev);
		return 0;
	}
	i = 0;
	while (i < filedata->nbuf) {
		int count = visordiag_extract_event_ex(filedata->buf + i,
						       filedata->nbuf - i,
						       &event,
						       default_pri);
		if (count == 0) {
			/* not a complete event yet, so save remnants for
			 * next time
			 */
			if (i > 0)
				memcpy(filedata->buf,
				       filedata->buf + i, filedata->nbuf - i);
			break;
		}
		if ((event.severity & SEVERITY_MASK) >=
		    VISORDIAG_MIN_SEVERITY_FOR_SUBSYS
		    (devdata->diag_channel_header->subsystem_severity_filter,
		     event.subsystem))
			new_message_to_host(devdata, &event);
		i += count;
	}
	filedata->nbuf -= i;

	up_read(&devdata->lock_visor_dev);
	return count;
}

/*
 * This function was created as part of the work done for s8528.
 * The caller using this device will send data in chunks no larger than the size
 * of event.AdditionalInfo.  This function will send each chunk unaltered to
 * the diag channel after performing a few checks.  Initial uses for this
 * device are to copy the DSET report file from the PDiag partition and the
 * MegaCLI report file from the IOVM partition.  They will end up bundled into
 * an ldump as separate files from the event files.
 *
 * Returns a negative value if a failure occurs, otherwise the number of
 * characters written to the channel.
 */
static ssize_t
visordiag_file_xfer(struct file *file, const char __user *buf,
		    size_t count, u8 *module_name)
{
	int rc = -1;
	struct visordiag_filedata *filedata =
	    (struct visordiag_filedata *)(file->private_data);
	struct visordiag_devdata *devdata = NULL;
	struct diag_channel_event event;
	uint throttled_count = 0;
	int slots_avail, max_slots;
	BOOL timed_out = FALSE;

	if (filedata == NULL)
			goto away;

	devdata = filedata->devdata;
	if (devdata == NULL)
			goto away;

	/* If data exceeds the size of the buffer (should never
	 * happen) return an error.
	 */
	if (count > sizeof(event.additional_info))
			goto away;

	devdata->counter.umode_bytes_in += count;
	down_read(&devdata->lock_visor_dev);
	if (devdata->dev == NULL) {	/* host channel is gone */
		up_read(&devdata->lock_visor_dev);
		rc = 0;	/* eof */
		goto away;
	}

	/* Prepare the event. */
	memset(&event, 0, sizeof(struct diag_channel_event));
	strncpy((char *)(event.module_name), module_name, MAX_MODULE_NAME_SIZE);
	event.severity = CAUSE_FILE_XFER_SEVERITY_PRINT;
	event.event_id = filedata->offset;
	event.line_number = count;	/* Special use for LineNumber on this
					 * device. Zero value marks end-of-file
					 * being transferred. */
	if (copy_from_user(event.additional_info, buf, count)) {
		up_read(&devdata->lock_visor_dev);
		rc = -EFAULT;
		goto away;
	}

	/* Check for the diag channel approaching full and delay until
	* more entries (slots) are available.  Currently if less than
	* 1/3 of the slots are available we enter throttling and stop
	* sending to the diag channel.  Then once at least half of the
	* slots are again available we exit throttling.  Once
	* throttling has been entered, a loop with a delay of 1/4 sec
	* occurs.  The loop limit of MAX_THROTTLE_TRIES is set to
	* create a max total delay of 5 minutes.  Note that the ldump
	* eventually times out after 10 minutes so the delay here
	* should be much less.
	*/
	slots_avail =
	    visorchannel_signalqueue_slots_avail(devdata->dev->visorchannel,
						 devdata->xmitqueue);
	max_slots =
	    visorchannel_signalqueue_max_slots(devdata->dev->visorchannel,
					       devdata->xmitqueue);

	if (slots_avail < (max_slots / 3)) {
		while (slots_avail < (max_slots / 2)) {
			throttled_count++;
			if (throttled_count > MAX_THROTTLE_TRIES) {
				timed_out = TRUE;
				break;
			}
			SLEEPJIFFIES(msecs_to_jiffies(250));
			slots_avail =
			    visorchannel_signalqueue_slots_avail(
					devdata->dev->visorchannel,
					devdata->xmitqueue);
		}
	}

	if (!timed_out) {
		new_message_to_host(devdata, &event);
	} else {
		up_read(&devdata->lock_visor_dev);
		rc = -EAGAIN;
		goto away;
	}
	up_read(&devdata->lock_visor_dev);
	rc = count;		/* Sets rc to count and goes to away label. */
away:
	if (rc > 0)
		filedata->offset += rc;
	return rc;
}

static int
visordiag_mmap(struct file *file, struct vm_area_struct *vma)
{
	ulong offset = vma->vm_pgoff << PAGE_SHIFT;
	ulong phys_addr = 0;
	pgprot_t pgprot;
	struct visordiag_filedata *filedata = (struct visordiag_filedata *)
	    (file->private_data);
	struct visordiag_devdata *devdata = NULL;

	if (filedata == NULL)
			return -1;

	devdata = filedata->devdata;
	if (devdata == NULL)
			return -1;

	if (pgprot_val(vma->vm_page_prot) != pgprot_val(PAGE_READONLY))
			return -EACCES;

	if (offset & (PAGE_SIZE - 1))
			return -ENXIO;

	switch (offset) {
	case VISORDIAG_MMAP_CHANNEL_OFF:

		vma->vm_flags |= VM_IO;
		phys_addr =
		    (ulong)visorchannel_get_physaddr(devdata->dev->
						      visorchannel);

		pgprot = pgprot_noncached(vma->vm_page_prot);

		if (io_remap_pfn_range(vma, vma->vm_start,
				       phys_addr >> PAGE_SHIFT,
				       vma->vm_end - vma->vm_start,
				       vma->vm_page_prot)) {
			return -EAGAIN;
		}
		break;
	default:
		return -ENOSYS;
	}

	return 0;
}

module_param_named(channeladdress, visordiag_channeladdress,
		   ulong, S_IRUGO);
MODULE_PARM_DESC(visordiag_channeladdress, "specify the physical address of the visor diag channel to use for this device");

module_param_named(major, visordiag_major, int, S_IRUGO);
MODULE_PARM_DESC(visordiag_major, "major device number for the visordiag device");

module_init(visordiag_init);
module_exit(visordiag_cleanup);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Unisys");
MODULE_DESCRIPTION("Supervisor visordiag driver for service partition: ver "
		   VERSION);
MODULE_VERSION(VERSION);
