#include "flogfs_conf.h"
#include "string.h"

#include "flogfs_private.h"

typedef struct {
	flog_read_file_t * read_head;   //!< The head of the list of read files
	flog_write_file_t * write_head; //!< The head of the list of write files
	uint32_t max_file_id;
	uint32_t max_block_sequence;
	flog_state_t state;
	
	// 2 bits per file
	uint8_t block_status[FS_NUM_BLOCKS / 4];
	
	fs_lock_t lock; //!< A lock to serialize some FS operations
} flogfs_t;

typedef struct {
	uint8_t header;
	uint32_t file_id;
	uint16_t first_block;
	char filename[FLOG_MAX_FILE_LEN];
} flog_file_header_t;

/*!
 @brief The header of a block of a file, stored in the first chunk of every
   file block
 */
typedef struct {
	uint32_t file_id;
	uint32_t file_seq;
	uint32_t block_seq;
} flog_file_block_header_t;

static flogfs_t flogfs;

static uint8_t const fs_header_buffer[12] = {
	0x00, 0x00, 0x00, 0x00,
	0xBE, 0xEF, FLOG_VSN_MAJOR, FLOG_VSN_MINOR,
	0x00, 0x00, 0x00, 0x00
};

static uint8_t const number_of_ones_count[256] = {
	// Fill this with a count of the number of ones at each index
};

uint8_t flogfs_count_flips8_default(uint8_t a, uint8_t b){
	return number_of_ones_count[a^b];
}

flog_result_t flogfs_init(){
	flogfs.state = FLOG_STATE_RESET;
	flogfs.read_head = 0;
	flogfs.write_head = 0;
	flogfs.max_file_id = 0;
	flogfs.max_block_sequence = 0;
	flash_init();
}

flog_result_t flogfs_format(){
	uint16_t i;
	
	fs_lock(&flogfs.lock);
	
	for(i = 0; i < FS_NUM_BLOCKS; i++){
		if(!flash_block_is_bad(i)){
			if(FLOG_FAILURE == flash_erase_block(i)){
				fs_unlock();
				flash_unlock();
				return FLOG_FAILURE;
			}
		}
	}
	
	flash_open_page(0, 0);
	
	flash_get_metadata();
	
	flash_write_chunk(fs_header_buffer, 0, 0, 12);
	
	flash_md1(0)[0] = FLOG_BLOCK_TYPE_HEADER;
	flash_md2(0)[0] = FLOG_HEADER_CHUNK_STAT_INUSE;
	flash_write_metadata();
	
	flash_commit();
	
	fs_unlock(&flogfs.lock);
	
	flash_unlock();
	return FLOG_SUCCESS;
}

flog_result_t flogfs_mount(){
	uint8_t buffer[32];
	uint32_t i, j, done_scanning;
	uint16_t block;
	union {
		flog_file_header_t header;
		flog_file_block_header_t block_header;
	};
	fs_lock(&flogfs.lock);
	flash_lock();
	
	if(FLOG_FAILURE == flash_open_page(0, 0)){
		// Can't even read the first page...
		goto failure;
	}
	
	flash_read_chunk(buffer, 0, 0, 12);
	
	if(0 != memcmp(buffer, fs_header_buffer, 12)){
		goto failure;
	}
	
	////////////////////
	// Now scan files to get max ID:
	////////////////////
	
	block = 0;
	done_scanning = 0;
	while(!done_scanning){
		for(i = 0; (i < FS_PAGES_PER_BLOCK) && !done_scanning; i++){
			if(i){
				if(FLOG_FAILURE == flash_open_page()){
					// Error in one of the header blocks
					goto failure;
				}
			}
			flash_get_metadata();
			for(j = 0; j < FS_CHUNKS_PER_PAGE; j++){
				// Don't care about the first chunk
				if((i == 0) && (j == 0)){
					continue;
				}
				if(flogfs_count_flips8(flash_md2(j)[0], FLOG_HEADER_CHUNK_STAT_FREE) < 2){
					// This chunk is free, meaning we're done
					done_scanning = 1;
					break;
				}
				// Otherwise do stuff with that block
				flash_read_chunk((uint8_t *) header, j, 0, sizeof(header) - FLOG_MAX_FILE_LEN);
				if(header.file_id > flogfs.max_file_id){
					flogfs.max_file_id = header.file_id;
				}
			}
		}
	}
	
	// Go check each block status and also get maximum block sequence number
	for(i = 0; i < FS_NUM_BLOCKS; i++){
		flash_open_page(i, 0);
		flash_get_metadata();
		if(i % 4 == 0){
			// Clear the entry for the next 4 blocks
			flogfs.block_status[i / 4] = 0;
		}
		if(flash_block_is_bad()){
			flogfs.block_status[i / 4] |= FLOG_BLOCK_CACHED_BAD << ((i % 4) * 2);
			continue;
		} else if((flogfs_count_flips8(flash_md2(j)[0], FLOG_HEADER_CHUNK_STAT_FREE) < 2)){
			flogfs.block_status[i / 4] |= FLOG_BLOCK_CACHED_UNUSED << ((i % 4) * 2);
			continue;
		} else if(flogfs_count_flips8(flash_md2(j)[0], FLOG_HEADER_CHUNK_STAT_DISCARD) < 2){
			flogfs.block_status[i / 4] |= FLOG_BLOCK_CACHED_DISCARD << ((i % 4) * 2);
			continue;
		} else if(flogfs_count_flips8(flash_md2(j)[0], FLOG_HEADER_CHUNK_STAT_INUSE) < 2){
			flash_read_chunk((uint8_t *) block_header, 0, 0, sizeof(block_header));
			if(block_header.block_seq > flogfs.max_block_sequence){
				flogfs.max_block_sequence = block_header.block_seq;
			}
		}
	}
	
	// Go erase expired blocks for fun
	if(ERASE_EXPIRED){
		for(i = 0; i < FS_NUM_BLOCKS; i++){
			// Check if it is flagged for discarding
			if((flogfs.block_status[i / 4] >> ((i % 4) * 2)) & 3 == FLOG_BLOCK_CACHED_DISCARD){
				flash_open_page(i, 0);
				flash_read_chunk((uint8_t *) block_header, 0, 0, sizeof(block_header));
				// Check if it has expired
				if((flogfs.max_block_sequence - block_header.block_seq) >= ERASE_EXPIRATION_TIME){
					flash_erase_block(i);
					// Go mark them as blank now
					flogfs.block_status[i / 4] =
					    flogfs.block_status[i / 4] & ~(3 << ((i % 4) * 2)) |
					    FLOG_BLOCK_CACHED_UNUSED << ((i % 4) * 2);
				}
			}
		}
	}
	
	flash_unlock();
	fs_unlock(&flogfs.lock);
	return FLOG_SUCCESS;

failure:
	flash_unlock();
	fs_unlock(&flogfs.lock);
	return FLOG_FAILURE;
}

flog_result_t flogfs_open_read(flog_read_file_t * file, char const * filename){
	
}

flog_result_t flogfs_open_write(flog_write_file_t * file, char const * filename){
	
}

