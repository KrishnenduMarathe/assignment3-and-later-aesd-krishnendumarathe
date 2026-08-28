/**
 * @file aesd-circular-buffer.c
 * @brief Functions and data related to a circular buffer imlementation
 *
 * @author Dan Walkes
 * @date 2020-03-01
 * @copyright Copyright (c) 2020
 *
 */

#ifdef __KERNEL__
#include <linux/string.h>
#include <linux/slab.h>

#define FREEM(ptr) kfree(ptr)

#else
#include <string.h>
#include <stdlib.h>

#define FREEM(ptr) free(ptr)

#endif

#include "aesd-circular-buffer.h"

/**
 * @param buffer the buffer to search for corresponding offset.  Any necessary locking must be performed by caller.
 * @param char_offset the position to search for in the buffer list, describing the zero referenced
 *      character index if all buffer strings were concatenated end to end
 * @param entry_offset_byte_rtn is a pointer specifying a location to store the byte of the returned aesd_buffer_entry
 *      buffptr member corresponding to char_offset.  This value is only set when a matching char_offset is found
 *      in aesd_buffer.
 * @return the struct aesd_buffer_entry structure representing the position described by char_offset, or
 * NULL if this position is not available in the buffer (not enough data is written).
 */
struct aesd_buffer_entry *aesd_circular_buffer_find_entry_offset_for_fpos(struct aesd_circular_buffer *buffer,
            size_t char_offset, size_t *entry_offset_byte_rtn )
{
    /**
    * TODO: implement per description
    */

    int totalcount = 0;
    if (buffer->full) {
        totalcount = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }
    else if (buffer->in_offs > buffer->out_offs) {
        totalcount = buffer->in_offs - buffer->out_offs;
    }
    else {
        // in case of first element
        totalcount = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED + buffer->in_offs - buffer->out_offs;
    }

    unsigned int idx = buffer->out_offs;
    unsigned int total_bytes = 0;

    for (int i = 0; i < totalcount; i++) {
        struct aesd_buffer_entry* elm = &buffer->entry[idx];
        if (char_offset < total_bytes + elm->size) {
            *entry_offset_byte_rtn = char_offset - total_bytes;
            return elm;
        }

        total_bytes += elm->size;
        idx = (idx + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    return NULL;
}

/**
* Adds entry @param add_entry to @param buffer in the location specified in buffer->in_offs.
* If the buffer was already full, overwrites the oldest entry and advances buffer->out_offs to the
* new start location.
* Any necessary locking must be handled by the caller
* Any memory referenced in @param add_entry must be allocated by and/or must have a lifetime managed by the caller.
*/
void aesd_circular_buffer_add_entry(struct aesd_circular_buffer *buffer, const struct aesd_buffer_entry *add_entry)
{
    /**
    * TODO: implement per description
    */

    // check if the current offset is at the end
    bool loop_over = false;
    if (buffer->in_offs >= AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED - 1) loop_over = true;

    // assign buffer to offset
	struct aesd_buffer_entry *rm_entry;
	rm_entry = &buffer->entry[buffer->in_offs];

	// free old entry
	if (rm_entry != NULL) {
		FREEM(rm_entry->buffptr);
	}
	
    buffer->entry[buffer->in_offs] = *add_entry;

    if (loop_over) {
        buffer->in_offs = 0;
        if (buffer->full) {
            buffer->out_offs = 0;
        }
        else {
            buffer->full = true;
        }
    }
    else {
        buffer->in_offs++;
        if (buffer->full) {
            buffer->out_offs++;
        }
    }
}

/**
* Initializes the circular buffer described by @param buffer to an empty struct
*/
void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer)
{
    memset(buffer,0,sizeof(struct aesd_circular_buffer));
}
