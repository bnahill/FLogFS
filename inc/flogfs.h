#ifndef __FLOGFS_H_
#define __FLOGFS_H_

#include "stdint.h"

#define FLOG_VSN_MAJOR        (0)
#define FLOG_VSN_MINOR        (1)

#define FLOG_MAX_FILE_LEN     (32)

typedef enum {
	FLOG_FAILURE,
	FLOG_SUCCESS
} flog_result_t;

typedef enum {
	FLOG_MODE_READ = 1,
	FLOG_MODE_WRITE = 2
} flog_mode_t;

typedef struct flog_read_file_s {
	uint32_t read_head; //!< Offset of read head from the start of the file
	uint16_t read_block; //!< Block index of read head
	uint16_t read_chunk; //!< Chunk index of read head
	uint16_t read_index; //!< Index of the read head inside chunk
	
	struct flog_read_file_s * next;
} flog_read_file_t;

typedef struct flog_write_file_s {
	uint32_t write_head; //!< Offset of write head from start of file
	uint16_t write_block; //! Block index of write head
	uint16_t write_chunk; //! Chunk index of write head
	uint16_t write_index; //! Write index of current chunk
	
	struct flog_write_file_s * next;
} flog_write_file_t;

/*!
 @brief Initialize flogfs filesystem structures
 */
flog_result_t flogfs_init();

flog_result_t flogfs_format();

flog_result_t flogfs_mount();

flog_result_t flogfs_open_read(flog_write_file_t * file, char const * filename);

flog_result_t flogfs_open_write(flog_write_file_t * file, char const * filename);

#endif // __FLOGFS_H_
