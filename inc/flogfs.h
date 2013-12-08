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
 * @file flogfs.h
 * @author Ben Nahill <bnahill@gmail.com>
 *
 * @ingroup FLogFS
 *
 * @brief Public interface for FLogFS
 */

#ifndef __FLOGFS_H_
#define __FLOGFS_H_

#include "stdint.h"

//! @addtogroup FLogPublic
//! @{

//! Compile as C++
#define FLOG_BUILD_CPP        (0)

//! @name Version Number
//! @{
#define FLOG_VSN_MAJOR        (0)
#define FLOG_VSN_MINOR        (1)
//! @}


//! @name Configuration Options
//! Some compilation options to customize the build
//! @{
//! The maximum file name length allowed
#define FLOG_MAX_FNAME_LEN     (32)
//! @}

#include "flogfs_conf.h"


#if !FLOG_BUILD_CPP
#ifdef __cplusplus
extern "C" {
#endif
#endif


typedef enum {
	FLOG_FAILURE,
	FLOG_SUCCESS
} flog_result_t;

typedef enum {
	FLOG_FLASH_SUCCESS = 0,
	FLOG_FLASH_ERR_CORRECT = 1,
	FLOG_FLASH_ERR_DETECT = -1
} flog_flash_read_result_t;

//! @name Type size definitions
//! @{
typedef uint32_t flog_timestamp_t;
typedef uint16_t flog_block_idx_t;
typedef uint32_t flog_block_age_t;
typedef uint32_t flog_file_id_t;
typedef uint16_t flog_sector_nbytes_t;
typedef uint16_t inode_index_t;
//! @}

/*!
 * @brief A structure for iterating through inode table elements
 */
typedef struct {
	//! The current block
	flog_block_idx_t block;
	//! The next block so as to avoid re-reading the header
	flog_block_idx_t next_block;
	//! The previous block
	flog_block_idx_t previous_block;
	//! The index of the current inode entry -- relative to start point
	uint16_t inode_idx;
	//! The index of the current inode block -- absolute
	uint16_t inode_block_idx;
	//! The current sector -- If this is
	//! FS_SECTORS_PER_PAGE * FS_PAGES_PER_BLOCK, at end of block
	uint16_t sector;
} flog_inode_iterator_t;

typedef flog_inode_iterator_t flogfs_ls_iterator_t;

#define FLOG_RESULT(x) ((x)?FLOG_SUCCESS:FLOG_FAILURE)

/*!
 @brief The state of a currently-open file

 This should be allocated by the application and provided to the file system
 for access to a file
 */
typedef struct flog_read_file_t {
	//! Offset of read head from the start of the file
	uint32_t read_head;
	//! Block index of read head
	uint16_t block;
	//! Sector index of read head
	uint16_t sector;
	//! Index of the read head inside sector
	uint16_t offset;
	//! Number of bytes remaining in current sector
	uint16_t sector_remaining_bytes;
	
	uint32_t id;
	
	struct flog_read_file_t * next;
} flog_read_file_t;

/*!
 @brief An instance of a file opened for writing.

 This should be allocated by the application and provided to the file system
 for access to a file. It contains the bulk of the required memory for most
 operations.
 */
typedef struct flog_write_file_t {
	//! Offset of write head from start of file
	uint32_t write_head;
	//! Block index of write head
	uint16_t block;
	//! Sector index of write head
	uint16_t sector;
	//! Index of write header insider current sector
	uint16_t offset;
	//! The number of bytes remaining in the sector before forcing a cache flush
	uint16_t sector_remaining_bytes;
	//! Bytes in block (so far)
	uint16_t bytes_in_block;
	uint32_t block_age;
	uint32_t id;
	
	int32_t base_threshold;

	uint8_t sector_buffer[FS_SECTOR_SIZE];
	
	struct flog_write_file_t * next;
} flog_write_file_t;

/*!
 @brief Initialize flogfs filesystem structures
 */
flog_result_t flogfs_init();

/*!
 @brief Format the flash memory for FLogFS
 */
flog_result_t flogfs_format();

/*!
 @brief Mount the FLogFS filesystem and prepare it for use
 */
flog_result_t flogfs_mount();

/*!
 @brief Open a file to read
 @param file The file structure to use
 @param filename The name of the file to use
 @retval FLOG_SUCCESS if successful
 @retval FLOG_FAILURE otherwise (doesn't exist or corruption)
 */
flog_result_t flogfs_open_read(flog_read_file_t * file, char const * filename);

/*!
 @brief Open a file to write
 @param file The file structure to use
 @param filename The name of the file to use
 @retval FLOG_SUCCESS if successful
 @retval FLOG_FAILURE otherwise

 Since the system is append-only, it automatically seeks to the end of the file
 if it exists. Check the flog_write_file_t::write_head value to see where you
 are writing.
 */
flog_result_t flogfs_open_write(flog_write_file_t * file, char const * filename);

/*!
 @brief Close a file which has been opened for reading
 @param file The currently-open read file
 @retval FLOG_SUCCESS if successful
 @retval FLOG_FAILURE otherwise
 */
flog_result_t flogfs_close_read(flog_read_file_t * file);

/*!
 @brief Close a file which has been opened for writing
 @param file The currently-open write file
 @retval FLOG_SUCCESS if successful
 @retval FLOG_FAILURE otherwise
 */
flog_result_t flogfs_close_write(flog_write_file_t * file);

/*!
 @brief Remove a file from the filesystem
 @param filename The name of the file
 */
flog_result_t flogfs_rm(char const * filename);

/*!
 @brief Read data from an open file
 @param file The file structure to read from
 @param dst The destination for the data
 @param nbytes The number of bytes to try to read
 @returns The number of bytes read
 */
uint32_t flogfs_read(flog_read_file_t * file, uint8_t * dst, uint32_t nbytes);

/*!
 @brief Write data to an open file
 @param file The file structure to write to
 @param src The data source
 @param nbytes The number of bytes to try to write
 @returns The number of bytes written
 */
uint32_t flogfs_write(flog_write_file_t * file, uint8_t const * src,
                      uint32_t nbytes);

/*!
 @brief Check if a file exists in the filesystem
 @param filename The 0-terminated filename to check for
 @retval FLOG_SUCCESS If the file exists
 @retval FLOG_FAILURE If the file doesn't exist
 */
flog_result_t flogfs_check_exists(char const * filename);

/*!
 @brief Start listing files (lock inode table)
 */
void flogfs_start_ls(flogfs_ls_iterator_t * iter);

/*!
 @brief Read another filename
 @param[out] fname_dst A destination to copy the filename
 @retval 1 Successful
 @retval 0 This is the end of the data
 */
uint_fast8_t flogfs_ls_iterate(flogfs_ls_iterator_t * iter, char * fname_dst);

/*!
 @brief Unlock the inode table when done listing
 */
void flogfs_stop_ls(flogfs_ls_iterator_t * iter);

#if !FLOG_BUILD_CPP
#ifdef __cplusplus
};
#endif
#endif

//! @} // Public

#endif // __FLOGFS_H_
