#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <asm/uaccess.h>
#include <linux/slab.h>		/** kmalloc() */

#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/init.h>

#include <linux/list.h>

struct scull_dev
{
  int quantum;			/** The size of each quantum */
  int qset;			/** The number of quantum in a qset */
  unsigned long size;		/** Amount of data stored here */
  struct cdev cdev;		/** Char device structure      */
  struct list_head data;
};

struct scull_qset
{
  void **data;
  struct list_head list;
};

#define SCULL_QUANTUM   4000
#define SCULL_QSET      1000

static int scull_major;
static int scull_minor = 0;
static int scull_quantum = SCULL_QUANTUM;
static int scull_qset = SCULL_QSET;

static struct scull_dev *scull_device;
static struct class *cl;

/** Buffer which is stored in "/sys/kernel/stat" */
static ssize_t
scull_obj_show (struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
  int number_of_qset = 0;
  struct scull_dev *dev = scull_device;
  struct list_head *dptr;

  /** Allocate first qset explicitly if need be */
  if (list_empty (&dev->data))
    INIT_LIST_HEAD (&dev->data);

  list_for_each (dptr, &dev->data)
  {
    ++number_of_qset;
  }

  return sprintf (buf, "The size is %lu and the number of qset is %d.\n",
		  scull_device->size, number_of_qset);
}

/** \brief  Set an attribute of kobject

    Sysfs attributes cannot be world-writable.  Object name is "stat".
*/
static struct kobj_attribute scull_stat_attr =
__ATTR (stat, 0444, scull_obj_show, NULL);

/** \brief  Create a group of attributes

    Create a group of attributes so that we can create and destroy them all
    at once.
*/
static struct attribute *scull_attrs[] = {
  &scull_stat_attr.attr,
  NULL,				/** Need to NULL terminate the list of attributes */
};

/** An unnamed attribute group will put all of the attributes directly in
    the kobject directory. If we specify a name, a subdirectory will be
    created for the attributes with the directory being the name of the
    attribute group.
*/
static struct attribute_group scull_attr_group = {
  .attrs = scull_attrs,
};

static struct kobject *scull_kobj;

/** Clean up all devices */
static int
scull_trim (struct scull_dev *dev)
{
  struct list_head *dptr;
  int i;

  list_for_each (dptr, &dev->data)
  {
    struct scull_qset *d = list_entry (dptr, struct scull_qset, list);
    for (i = 0; i < dev->qset; ++i) /** TODO: ERROR data[i] (i < dev->qset) can be not allocated data */
      kfree (d->data[i]);
    kfree (d->data);
  }

  list_del_init (&dev->data);

  /** Initialise items here */
  dev->size = 0;
  dev->quantum = scull_quantum;
  dev->qset = scull_qset;

  return 0;
}

/** Get a set of quantum

    \return     Pointer of a set of quantum (make a set if not available)
    \param[in]  dev     Device structure
    \param[in]  n       Count, from the first

*/
static struct scull_qset *
scull_follow (struct scull_dev *dev, int n)
{

  struct list_head *dptr;
  struct scull_qset *d;

  /** Allocate first qset explicitly if need be */
  if (list_empty (&dev->data))
    INIT_LIST_HEAD (&dev->data);

  /** Then follow the list */
  list_for_each (dptr, &dev->data)
  {
    if (n-- == 0)
      return list_entry (dptr, struct scull_qset, list);
  }

  d = kmalloc (sizeof (struct scull_qset), GFP_KERNEL);

  if (d == NULL)
    return NULL;		/** Never mind */

  memset (d, 0, sizeof (struct scull_qset));
  list_add (&d->list, &dev->data);

  return d;
}

static int
scull_open (struct inode *inode, struct file *filp)
{
  struct scull_dev *dev;	/** device information */
  dev = container_of (inode->i_cdev, struct scull_dev, cdev);
  filp->private_data = dev;	/** For other methods */

  /** Trim to 0 the length of the device if open was write-only */
  if ((filp->f_flags & O_ACCMODE) == O_WRONLY)
    {
      scull_trim (dev);
    }
  return 0;			/** Success */
}

static int
scull_release (struct inode *inode, struct file *filp)
{
  return 0;
}

static char *
scull_devnode (struct device *dev, umode_t * mode)
{
  if (!mode)
    return NULL;

  *mode = 0666;
  return NULL;
}

static ssize_t
scull_read (struct file *filp, char __user * buf,
	    size_t count, loff_t * f_pos)
{
  struct scull_dev *dev = filp->private_data;
  struct scull_qset *dptr;	/** The first listitem */
  int quantum = dev->quantum, qset = dev->qset;
  int itemsize = quantum * qset;	/** How many bytes in the listitem */
  int item, s_pos, q_pos, rest;
  ssize_t retval = 0;
  if (*f_pos >= dev->size)
    goto out;
  if (*f_pos + count > dev->size)
    count = dev->size - *f_pos;

  /** Find listitem, qset index, and offset in the quantum */
  item = (long) *f_pos / itemsize;
  rest = (long) *f_pos % itemsize;
  s_pos = rest / quantum;
  q_pos = rest % quantum;
  /** Follow the list up to the right position (defined elsewhere) */
  dptr = scull_follow (dev, item);

  if (dptr == NULL || !dptr->data || !dptr->data[s_pos])
    goto out;			/** Don't fill holes */

  /** Read only up to the end of this quantum */
  if (count > quantum - q_pos)
    count = quantum - q_pos;

  if (copy_to_user (buf, dptr->data[s_pos] + q_pos, count))
    {
      retval = -EFAULT;
      goto out;
    }
  *f_pos += count;
  retval = count;

out:
  return retval;
}

static ssize_t
scull_write (struct file *filp, const char __user * buf,
	     size_t count, loff_t * f_pos)
{
  struct scull_dev *dev = filp->private_data;
  struct scull_qset *dptr;
  int quantum = dev->quantum, qset = dev->qset;
  int itemsize = quantum * qset;	/** Size of each qset */
  int item, s_pos, q_pos, rest;
  ssize_t retval = -ENOMEM;	/** Value used in "goto out" statements */
  /** Find listitem, qset index and offset in the quantum */
  item = (long) *f_pos / itemsize;	/** item-th qset */
  rest = (long) *f_pos % itemsize;	/** rest-th byte in the item’s qset */
  s_pos = rest / quantum;	/** s_pos-th quantum */
  q_pos = rest % quantum;	/** q_pos-th byte in the quantum */

  /** Follow the list up to the right position */
  dptr = scull_follow (dev, item);
  if (dptr == NULL)
    goto out;
  if (!dptr->data)
    {
      dptr->data = kmalloc (qset * sizeof (char *), GFP_KERNEL);
      if (!dptr->data)
	goto out;
      memset (dptr->data, 0, qset * sizeof (char *));
    }
  if (!dptr->data[s_pos])
    {
      dptr->data[s_pos] = kmalloc (quantum, GFP_KERNEL);
      if (!dptr->data[s_pos])
	goto out;
    }
  /** Write only up to the end of this quantum */
  if (count > quantum - q_pos)
    count = quantum - q_pos;

  if (copy_from_user (dptr->data[s_pos] + q_pos, buf, count))
    {
      retval = -EFAULT;
      goto out;
    }

  *f_pos += count;
  retval = count;

  /** Update the size */
  if (dev->size < *f_pos)
    dev->size = *f_pos;

out:
  return retval;
}

static struct file_operations scull_fops = {
  .owner = THIS_MODULE,
  .read = scull_read,
  .write = scull_write,
  .open = scull_open,
  .release = scull_release,
};

static void
scull_cleanup_module (void)
{
  dev_t devno = MKDEV (scull_major, scull_minor);

  if (scull_kobj)
    {
      kobject_put (scull_kobj);
    }

  /** Get rid of our char dev entries */
  if (scull_device)
    {
      scull_trim (scull_device);
      cdev_del (&(scull_device->cdev));
      kfree (scull_device);
    }
  device_destroy (cl, devno);
  class_destroy (cl);
  unregister_chrdev_region (devno, 1);
}

static int
scull_init_module (void)
{

  int retval;
  dev_t dev = 0;
  if (alloc_chrdev_region (&dev, 0, 1, "scull") < 0)
    {
      printk (KERN_WARNING "scull: can't get major.\n");
      return -1;
    }
  scull_major = MAJOR (dev);

  if ((cl = class_create (THIS_MODULE, "scullchardrv")) == NULL)
    {
      unregister_chrdev_region (dev, 1);
      return -1;
    }
  cl->devnode = scull_devnode;
  if (device_create (cl, NULL, dev, NULL, "myscull") == NULL)
    {
      class_destroy (cl);
      unregister_chrdev_region (dev, 1);
      return -1;
    }
  scull_device = kmalloc (sizeof (struct scull_dev), GFP_KERNEL);
  if (!scull_device)
    {
      class_destroy (cl);
      unregister_chrdev_region (dev, 1);
      return -ENOMEM;
    }
  memset (scull_device, 0, sizeof (struct scull_dev));

  /** Initialise scull_device here */
  scull_device->quantum = scull_quantum;
  scull_device->qset = scull_qset;
  scull_device->size = 0;
  INIT_LIST_HEAD (&scull_device->data);

  cdev_init (&(scull_device->cdev), &scull_fops);
  scull_device->cdev.owner = THIS_MODULE;
  if (cdev_add (&(scull_device->cdev), dev, 1) == -1)
    {
      device_destroy (cl, dev);
      class_destroy (cl);
      unregister_chrdev_region (dev, 1);
      return -1;
    }

  /** Create a simple kobject with the name of "scull",
      located under /sys/kernel/

      As this is a simple directory, no uevent will be sent to
      userspace.  That is why this function should not be used for
      any type of dynamic kobjects, where the name and number are
      not known ahead of time.
  */
  scull_kobj = kobject_create_and_add ("scull", kernel_kobj);
  if (!scull_kobj)
    {
      device_destroy (cl, dev);
      class_destroy (cl);
      unregister_chrdev_region (dev, 1);
      return -ENOMEM;
    }

  /** Create the files associated with this kobject */
  retval = sysfs_create_group (scull_kobj, &scull_attr_group);
  if (retval)
    {
      kobject_put (scull_kobj);
      device_destroy (cl, dev);
      class_destroy (cl);
      unregister_chrdev_region (dev, 1);
      return retval;
    }

  return retval;		/** Succeess */
}

module_init (scull_init_module);
module_exit (scull_cleanup_module);

MODULE_LICENSE ("GPL");
MODULE_DESCRIPTION ("Our First Character Driver");
MODULE_AUTHOR ("Jongmin Kim <jmkim@pukyong.ac.kr>");
