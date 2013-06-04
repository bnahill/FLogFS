#ifndef __FLOGFS_PRIVATE_H_
#define __FLOGFS_PRIVATE_H_

#include "flogfs_conf.h"

typedef enum {
	FLOG_STATE_RESET,
	FLOG_STATE_MOUNTED
} flog_state_t;

/*!
 @brief A block type stored in the first byte of the protected MD1 in the
   first chunk of a block

 Those values not present represent either a free block of an error
 */
typedef enum {
	FLOG_BLOCK_TYPE_HEADER = 1,
	FLOG_BLOCK_TYPE_FILE = 2
} flog_block_type_t;

/*!
 @brief The status of the block as stored in the first byte of the unprotected
   MD2 of the first chunk of a block.

 @note This is in an unprotected area and some error checking must be done
 */
typedef enum {
	FLOG_BLOCK_UNUSED = 0xFF,
	FLOG_BLOCK_INUSE = 0x0F,
	FLOG_BLOCK_DISCARD = 0x00
} flog_block_status_t;

/*!
 @brief The 2-bit status of the block as stored in in SRAM for quick reference
 */
typedef enum {
	FLOG_BLOCK_CACHED_UNUSED = 0,
	FLOG_BLOCK_CACHED_INUSE = 1,
	FLOG_BLOCK_CACHED_DISCARD = 2,
	FLOG_BLOCK_CACHED_BAD = 3
} flog_block_cached_status_t;

/*!
 @brief The chunk status in a header, stored in the first byt of the
   unprotected MD2
 
 @note This is in an unprotected area and some error checking must be done
 */
typedef enum {
	FLOG_HEADER_CHUNK_STAT_FREE = 0xFF,
	FLOG_HEADER_CHUNK_STAT_INUSE = 0x0F,
	FLOG_HEADER_CHUNK_STAT_DISCARD = 0x00
} flog_header_chunk_status_t;

#endif // __FLOGFS_PRIVATE_H_
