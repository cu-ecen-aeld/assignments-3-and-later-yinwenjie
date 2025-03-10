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
#else
#include <string.h>
#endif

#include "aesd-circular-buffer.h"

/**
 * @param buffer the buffer to search for corresponding offset.  Any necessary locking must be performed by caller.
 * @param char_offset the position to search for in the buffer list, describing the zero referenced
 *      character buffer_idx if all buffer strings were concatenated end to end
 * @param entry_offset_byte_rtn is a pointer specifying a location to store the byte of the returned aesd_buffer_entry
 *      buffptr member corresponding to char_offset.  This value is only set when a matching char_offset is found
 *      in aesd_buffer.
 * @return the struct aesd_buffer_entry structure representing the position described by char_offset, or
 * NULL if this position is not available in the buffer (not enough data is written).
 */
struct aesd_buffer_entry *aesd_circular_buffer_find_entry_offset_for_fpos(struct aesd_circular_buffer *buffer,
            size_t char_offset, size_t *entry_offset_byte_rtn )
{
    uint32_t buffer_total_sz = 0;
    uint8_t isfull = buffer->full;
    uint8_t buffer_idx = buffer->out_offs;
    const char *buffer_ptr = NULL;

    while(buffer_idx != buffer->in_offs || isfull) {
        buffer_total_sz += (buffer->entry[buffer_idx].size);
        if(buffer_total_sz > char_offset) {
            uint16_t start_index = 0;
            uint16_t last_index = char_offset - (buffer_total_sz - buffer->entry[buffer_idx].size);            
            buffer_ptr = buffer->entry[buffer_idx].buffptr;
            while(start_index != last_index) {
                start_index++;
                buffer_ptr++;
            }            
            *entry_offset_byte_rtn  = last_index;
            return &buffer->entry[buffer_idx];
        }
        buffer_idx = (buffer_idx + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        isfull = 0;
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
const char* aesd_circular_buffer_add_entry(struct aesd_circular_buffer *buffer, const struct aesd_buffer_entry *add_entry)
{
    const char *pointer_to_free = NULL;
    if(buffer->full) {
        buffer->out_offs = (buffer->out_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        pointer_to_free = buffer->entry[buffer->in_offs].buffptr;
    }
    buffer->entry[buffer->in_offs] = *add_entry;
    buffer->in_offs = (buffer->in_offs+1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    if(buffer->in_offs == buffer->out_offs) {
        buffer->full = 1;
    }
    return pointer_to_free;
}

/**
* Initializes the circular buffer described by @param buffer to an empty struct
*/
void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer)
{
    memset(buffer,0,sizeof(struct aesd_circular_buffer));
}

size_t aesd_circular_buffer_size(struct aesd_circular_buffer *buffer) {
    uint8_t index;
    size_t total_bytes = 0;
    struct aesd_buffer_entry* entry = NULL; 
    AESD_CIRCULAR_BUFFER_FOREACH(entry, buffer, index)
    {
        if(entry->buffptr == NULL) break;
        total_bytes += entry->size;
    }
    return total_bytes;
}