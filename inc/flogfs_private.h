/*
Copyright (c) 2013, Ben Nahill <bnahill@gmail.com>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The views and conclusions contained in the software and documentation are those
of the authors and should not be interpreted as representing official policies,
either expressed or implied, of the FLogFS Project.
*/

/*!
 * @file flogfs_private.h
 * @author Ben Nahill <bnahill@gmail.com>
 *
 * @ingroup FLogFS
 *
 * @brief Private definitions for FLogFS internals
 *
 */

#ifndef __FLOGFS_PRIVATE_H_
#define __FLOGFS_PRIVATE_H_

#include "flogfs.h"
#include "flogfs_conf.h"

// KDevelop wants to be a jerk about the __restrict__ keyword
// #ifdef USE_RESTRICT
// 	#if defined(__GNUG__)
// 	#define restrict __restrict__
// 	#elif defined(__GNUC__)
// 	#define restrict restrict
// 	#else
// 	#define restrict
// 	#endif
// #else
// 	#if not defined(__cplusplus)
// 	#define restrict restrict
// 	#else
// 	#define restrict
// 	#endif
// #endif


#define MAX(a,b) ((a > b) ? a : b)
#define MIN(a,b) ((a > b) ? b : a)


//! @addtogroup FLogPrivate
//! @{

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

//! @name Type size definitions
//! @{
typedef uint32_t flog_timestamp_t;
typedef uint16_t flog_block_idx_t;
typedef uint32_t flog_block_age_t;
typedef uint32_t flog_file_id_t;
typedef uint16_t flog_sector_nbytes_t;
typedef uint16_t inode_index_t;
//! @}

//! @name Invalid values
//! @{
#define FLOG_BLOCK_IDX_INVALID ((flog_block_idx_t)(-1))
#define FLOG_BLOCK_AGE_INVALID ((flog_block_age_t)(-1))
#define FLOG_FILE_ID_INVALID   ((flog_file_id_t)(-1))
#define FLOG_TIMESTAMP_INVALID ((flog_timestamp_t)(-1))
//! @}

//! A marker value to identify a completed inode copy
static uint8_t const flog_copy_complete_marker = 0x55;


//! @defgroup FLogInodeBlockStructs Inode block structures
//! @brief Descriptions of the data in inode blocks
//! @{
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

//! @} // Inode structures

typedef struct {
	flog_block_age_t age;
} flog_universal_sector0_header_t;

typedef struct {
	flog_timestamp_t timestamp;
} flog_universal_invalidation_header_t;

//! @defgroup FLogFileBlockStructs File block structures
//! @brief Descriptions of the data in file blocks
//! @{

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
	uint16_t bytes_in_block;
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

//! @}


//! @name Special sector indices
//! @{
#define FLOG_FILE_TAIL_SECTOR           (FS_SECTORS_PER_PAGE - 2)
#define FLOG_FILE_INVALIDATION_SECTOR   (FS_SECTORS_PER_PAGE - 1)
#define FLOG_INODE_TAIL_SECTOR          (FS_SECTORS_PER_PAGE - 2)
#define FLOG_INODE_INVALIDATION_SECTOR  (FS_SECTORS_PER_PAGE - 1)
//! @}

//! @} // FLogPrivate


#endif // __FLOGFS_PRIVATE_H_
