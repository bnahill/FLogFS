#ifndef __FLOGFS_PRIVATE_H_
#define __FLOGFS_PRIVATE_H_

#include "flogfs.h"
#include "flogfs_conf.h"

#define MAX(a,b) ((a > b) ? a : b)

typedef enum {
	FLOG_STATE_RESET,
	FLOG_STATE_MOUNTED
} flog_state_t;

/*!
 @brief A block type stored in the first byte of the first sector spare

 Those values not present represent an error
 */
typedef enum {
	FLOG_BLOCK_TYPE_UNALLOCATED = 0xFF,
	FLOG_BLOCK_TYPE_INODE = 1,
	FLOG_BLOCK_TYPE_FILE = 2
} flog_block_type_t;


typedef uint32_t flog_timestamp_t;
typedef uint16_t flog_block_idx_t;
typedef uint32_t flog_block_age_t;
typedef uint32_t flog_file_id_t;
typedef uint16_t flog_sector_nbytes_t;
typedef uint16_t inode_index_t;

#define FLOG_BLOCK_IDX_INVALID ((flog_block_idx_t)(-1))
#define FLOG_FILE_ID_INVALID   ((flog_file_id_t)(-1))
#define FLOG_TIMESTAMP_INVALID ((flog_timestamp_t)(-1))

static uint8_t const flog_copy_complete_marker = 0x55;

/*!
 @name Inode block structures
 @{
 */

typedef struct {
	flog_block_age_t age;
	flog_timestamp_t timestamp;
} flog_inode_sector0_t;

typedef struct {
	uint8_t type_id;
	uint8_t nothing;
	inode_index_t inode_index;
} flog_inode_sector0_spare_t;

typedef struct {
	flog_block_idx_t next_block;
	flog_block_age_t next_age;
	flog_timestamp_t timestamp;
} flog_inode_tail_sector_t;

typedef struct {
	flog_timestamp_t timestamp;
	flog_block_idx_t replacement_id;
	flog_block_age_t replacement_age;
} flog_inode_invalidation_sector_t;

typedef struct {
	flog_file_id_t file_id;
	flog_block_idx_t first_block;
	flog_block_age_t first_block_age;
	flog_timestamp_t timestamp;
} flog_inode_file_allocation_header_t;

typedef struct {
	flog_inode_file_allocation_header_t header;
	char filename[FLOG_MAX_FNAME_LEN];
} flog_inode_file_allocation_t;

typedef struct {
	uint8_t copy_complete_marker;
} flog_inode_file_allocation_spare_t;

typedef struct {
	flog_timestamp_t timestamp;
	flog_block_idx_t last_block;
} flog_inode_file_invalidation_t;

/*!
 @}
 */



/*!
 @name File block structures
 @{
 */

typedef struct {
	flog_block_age_t age;
	flog_file_id_t file_id;
} flog_file_sector0_header_t;

typedef struct {
	flog_file_sector0_header_t header;
	uint8_t data[FS_SECTOR_SIZE - sizeof(flog_file_sector0_header_t)];
} flog_file_sector0_t;

typedef struct {
	flog_block_idx_t next_block;
	flog_block_age_t next_age;
	flog_timestamp_t timestamp;
} flog_file_tail_sector_header_t;

typedef struct {
	flog_file_tail_sector_header_t header;
	uint8_t data[FS_SECTOR_SIZE - sizeof(flog_file_tail_sector_header_t)];
} flog_file_tail_sector_t;

typedef struct {
	uint8_t type_id;
	uint8_t nothing;
	flog_sector_nbytes_t nbytes;
} flog_file_sector_spare_t;

typedef struct {
	flog_timestamp_t timestamp;
	flog_block_age_t next_age;
} flog_file_invalidation_sector_t;

/*!
 @}
 */

#define FLOG_FILE_TAIL_SECTOR           (FS_SECTORS_PER_PAGE - 2)
#define FLOG_FILE_INVALIDATION_SECTOR   (FS_SECTORS_PER_PAGE - 1)
#define FLOG_INODE_TAIL_SECTOR          (FS_SECTORS_PER_PAGE - 2)
#define FLOG_INODE_INVALIDATION_SECTOR  (FS_SECTORS_PER_PAGE - 1)

/*!
 @brief 
 
 @note This is in an unprotected area and some error checking must be done
 */
typedef enum {
	FLOG_HEADER_CHUNK_STAT_FREE = 0xFF,
	FLOG_HEADER_CHUNK_STAT_INUSE = 0x0F,
	FLOG_HEADER_CHUNK_STAT_DISCARD = 0x00
} flog_header_chunk_status_t;

#endif // __FLOGFS_PRIVATE_H_
