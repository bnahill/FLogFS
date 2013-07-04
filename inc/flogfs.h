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

#define FLOG_RESULT(x) ((x)?FLOG_SUCCESS:FLOG_FAILURE)

/*!
 @typedef flog_read_file_t
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
	uint8_t  at_eof;
	
	uint32_t id;
	
	struct flog_read_file_t * next;
} flog_read_file_t;

typedef struct flog_write_file_t {
	uint32_t write_head; //!< Offset of write head from start of file
	uint16_t write_block; //! Block index of write head
	uint16_t write_chunk; //! Chunk index of write head
	uint16_t write_index; //! Write index of current chunk
	
	uint32_t id;
	
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

flog_result_t flogfs_open_read(flog_read_file_t * file, char const * filename);

flog_result_t flogfs_open_write(flog_write_file_t * file, char const * filename);

flog_result_t flogfs_close_read(flog_read_file_t * file);

flog_result_t flogfs_close_write(flog_write_file_t * file);

#if !FLOG_BUILD_CPP
#ifdef __cplusplus
};
#endif
#endif

//! @} // Public

#endif // __FLOGFS_H_
