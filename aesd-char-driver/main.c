/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesd-circular-buffer.h"
#include "aesdchar.h"

#include <linux/slab.h> // kmalloc
#include <linux/mutex.h> // mutex
#include <linux/uaccess.h>
#include <linux/string.h>

int aesd_major = 0; // use dynamic major
int aesd_minor = 0;

MODULE_AUTHOR("Krishnendu Marathe"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filep)
{
    PDEBUG("open");
    /**
     * TODO: handle open
     */

	// save private data
	struct aesd_dev *dev;
	dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
	filep->private_data = dev;	
	
    return 0;
}

int aesd_release(struct inode* inode, struct file *filep)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */

	// release private_data
	if (filep->private_data != NULL) {
		filep->private_data = NULL;
	}
	
    return 0;
}

ssize_t aesd_read(struct file *filep, char __user *buf, size_t count, loff_t *f_pos)
{
	// partial read flag assumes the fd is open while reading the entire team

    ssize_t retval = 0;
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle read
     */

	// get pointer to custom driver struct
	struct aesd_dev *dev = (struct aesd_dev *) filep->private_data;

	// lock mutex
	retval = mutex_lock_interruptible(&dev->mut);
	if (retval != 0) {
		PDEBUG("failed to acquire lock due to interruption");
		retval = -ERESTARTSYS;
		goto read_exit;
	}

	// read from circular buffer
	char *dbuffer = NULL;
	size_t offset_ret = 0;

	struct aesd_buffer_entry* data = aesd_circular_buffer_find_entry_offset_for_fpos(dev->buffer, *f_pos, &offset_ret);
	if (data == NULL) {
		// EOF
		PDEBUG("-> at EOF");
		goto read_exit;
	}

	// copy over expected data
	if (count > KMALLOC_MAX_SIZE) {
		count = KMALLOC_MAX_SIZE;
	}
	
	if (count > data->size) {
		count = data->size;
	}

	retval = count;

	// dynamic buffer
	dbuffer = (char *) kmalloc(count, GFP_KERNEL);
	if (dbuffer == NULL) {
		PDEBUG("Failed to allocate dynamic buffer for read with size %u", count);
		retval = -ENOMEM;
		goto read_exit;
	}
	memset(dbuffer, 0, count);

	// copy data to buffer
	memcpy(dbuffer, data->buffptr, count);

	// check if data is partially needed
	/*
	 * UNNCESSARY HANDLING FOR A DRIVER
	 *
	 if (count < data->size) {
		// partial send
		memcpy(dbuffer, data->buffptr, count-1);
		dbuffer[count-1] = '\0';
	}
	else {
		 // more than 1 element
		 unsigned long int new_pos = *f_pos + data->size;
		 unsigned long int counter = data->size - 1; // skip null character
		 memcpy(dbuffer, data->buffptr, counter);
		 while (counter < count) {
			 data = aesd_circular_buffer_find_entry_offset_for_fpos(dev->buffer, new_pos, &offset_ret);

			 // EOF
			 if (data == NULL) {
				 retval = 0;
				 break;
			 }

			 unsigned long int rem = count - counter;
			 if (rem < data->size) {
				 memcpy(dbuffer+counter, data->buffptr, rem-1);
				 dbuffer[rem-1] = '\0';
				 counter += rem;
			 } else {
				 memcpy(dbuffer+counter, data->buffptr, data->size-1);
				 counter += data->size-1;
			 }

			 new_pos += data->size;
		 }
	 }*/

	// maybe need to handle EOF here. we are just assuming blank data in dbuffer
	unsigned long int not_copied = copy_to_user(buf, dbuffer, count);
	PDEBUG("-> copied %u bytes to user space with %u not copied", count, not_copied);

	if (not_copied != 0) {
		PDEBUG("Failed to copy data to user space");
		retval = -EFAULT;
		goto read_exit;
	}

read_exit:
	// unlock mutex
	if (mutex_is_locked(&dev->mut)) {
		mutex_unlock(&dev->mut);
	}
	
	if (dbuffer != NULL) kfree(dbuffer);
    return retval;
}

ssize_t aesd_write(struct file *filep, const char __user *buf, size_t count, loff_t *f_pos)
{
	PDEBUG("write %zu bytes with offset %lld",count,*f_pos);

	/**
     * TODO: handle write
     */

	// get pointer for custom driver struct
	struct aesd_dev *dev = (struct aesd_dev *) filep->private_data;
	
	// create write buffer
	if (count > KMALLOC_MAX_SIZE) {
		count = KMALLOC_MAX_SIZE;
	}

	ssize_t retval = count;

	char *writebuf = (char *) kmalloc(count, GFP_KERNEL);
	if (writebuf == NULL) {
		PDEBUG("failed to allocate memory for write buffer");
		retval = -ENOMEM;
		goto grace_exit;
	}
	unsigned long int not_copied = copy_from_user(writebuf, buf, count);
	PDEBUG("-> copied %u bytes from user space with %u not copied", count, not_copied);

	if (not_copied != 0) {
		PDEBUG("Failed to copy data from user space");
		retval = -EFAULT;
		goto grace_exit;
	}

	// realloc dynamic buffer
	char *ptr = (char *) kmalloc(dev->dynbuffersize+count, GFP_KERNEL);
	if (ptr == NULL) {
		PDEBUG("failed to allocate dynamic memory with size %u", dev->dynbuffersize+count+1);
		retval = -ENOMEM;
		goto grace_exit;
	}

	memcpy(ptr, dev->dynbuffer, dev->dynbuffersize);
	kfree(dev->dynbuffer);
	dev->dynbuffer = ptr;
	memcpy(dev->dynbuffer + dev->dynbuffersize, writebuf, count);
	dev->dynbuffersize += count;

	// check for newline
	char *retptr;
	int newlinefound = 0;

	while (dev->dynbuffer != NULL && (retptr = strchr(dev->dynbuffer, '\n')) != NULL) {
		// new line found
		if (!newlinefound) newlinefound = 1;

		unsigned int length = (retptr - dev->dynbuffer) + 1;

		// write data to circular_buffer
		struct aesd_buffer_entry *entry;
		entry = (struct aesd_buffer_entry *) kmalloc(sizeof(struct aesd_buffer_entry), GFP_KERNEL);
		if (entry == NULL) {
			PDEBUG("Failed to allocate memory for entry struct");
			goto grace_exit;
		}

		entry->size = length;

		entry->buffptr = (char *) kmalloc(length, GFP_KERNEL);
		if (entry->buffptr == NULL) {
			PDEBUG("Failed to allocate memorry to buffer for entry");
			kfree(entry);
			goto grace_exit;
		}

		// lock mutex
		retval = mutex_lock_interruptible(&dev->mut);
		if (retval != 0) {
			PDEBUG("failed to acquire lock due to interruption");
			goto grace_exit;
		}

		memcpy(entry->buffptr, dev->dynbuffer, length);
		aesd_circular_buffer_add_entry(dev->buffer, entry);

		// unlock mutex
		if (mutex_is_locked(&dev->mut)) {
			mutex_unlock(&dev->mut);
		}

		// move remaining data forward
		unsigned int rem_length = dev->dynbuffersize - length;
		if (rem_length > 0) {
			memmove(dev->dynbuffer, dev->dynbuffer+length, rem_length);
			dev->dynbuffersize = rem_length;
			dev->dynbuffer[dev->dynbuffersize] = '\0';
		}
		else {
			if (dev->dynbuffer != NULL) kfree(dev->dynbuffer);
			dev->dynbuffer = NULL;
			dev->dynbuffersize = 0;
		}
	}

	
grace_exit:
	if (writebuf != NULL) kfree(writebuf);
    return retval;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}

int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));
	aesd_device.dynbuffer = NULL;
	aesd_device.dynbuffersize = 0;
	aesd_device.partial_read = 0;

    /**
     * TODO: initialize the AESD specific portion of the device
     */

	// initialize mutex
	mutex_init(&aesd_device.mut);

	// set up circular buffer
	aesd_device.buffer = (struct aesd_circular_buffer *) kmalloc(sizeof(struct aesd_circular_buffer), GFP_KERNEL);
	if (aesd_device.buffer == NULL) {
		PDEBUG("Failed to allocate memory for circular buffer");
		unregister_chrdev_region(dev, 1);
		
		return -1;
	}	
	aesd_circular_buffer_init(aesd_device.buffer);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */

	// clean up circular buffer
	int index;
	struct aesd_buffer_entry *entry;
	AESD_CIRCULAR_BUFFER_FOREACH(entry, aesd_device.buffer, index) {
		if (entry->buffptr != NULL) {
			kfree(entry->buffptr);
		}
	}

	if (aesd_device.buffer != NULL) {
		kfree(aesd_device.buffer);
	}

	if (aesd_device.dynbuffer != NULL) {
		kfree(aesd_device.dynbuffer);
	}

    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
