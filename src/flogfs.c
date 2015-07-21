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
 * @file flogfs.c
 * @author Ben Nahill <bnahill@gmail.com>
 * @ingroup FLogFS
 * @brief Core file system implementation
 */

#include "flogfs_private.h"
#include "flogfs.h"

#include <string.h>

#ifndef IS_DOXYGEN
#if !FLOG_BUILD_CPP
#ifdef __cplusplus
extern "C" {
#endif
#endif
#endif

#include "flogfs_conf_implement.h"

//! @addtogroup FLogPrivate
//! @{


typedef struct {
	flog_block_idx_t block;
	flog_block_age_t age;
} flog_block_alloc_t;

typedef struct {
	//! Block indices and ages
	flog_block_alloc_t blocks[FS_PREALLOCATE_SIZE];
	//! The number of entries
	uint16_t n;
	//! The sum of all ages
	flog_block_age_t age_sum;
} flog_prealloc_list_t;

typedef struct {
	flog_block_idx_t block;
	flog_write_file_t * file;
} flog_dirty_block_t;

typedef struct {
	flog_file_id_t file_id;
	flog_block_idx_t first_block;
} flog_file_find_result_t;

/*!
 @brief The complete FLogFS state structure
 */
typedef struct {
	//! The head of the list of read files
	flog_read_file_t * read_head;
	//! The head of the list of write files
	flog_write_file_t * write_head;
	//! The maximum file ID active in the system
	uint32_t max_file_id;

	//! The state of the file system
	flog_state_t state;

	//! The average block age in the file system
	uint32_t mean_block_age;

	//! A list of preallocated blocks for quick access
	flog_prealloc_list_t prealloc;

	//! The most recent timestamp (sequence number)
	//! @note To put a stamp on a new operation, you should preincrement. This
	//! is the timestamp of the most recent operation
	flog_timestamp_t t;

	//! The location of the first inode block
	flog_block_idx_t inode0;
	//! The number of files in the system
	flog_file_id_t   num_files;

	//! @brief Flash cache status
	//! @note This must be protected under @ref flogfs_t::lock !
	struct {
	flog_block_idx_t current_open_block;
	uint16_t         current_open_page;
	uint_fast8_t     page_open;
	flog_result_t    page_open_result;
	} cache_status;
	
	uint8_t free_block_bitmap[FS_NUM_BLOCKS / 8];
	
	flog_block_age_t mean_free_age;
	uint32_t free_block_sum;
	//! The number of free blocks
	flog_block_idx_t num_free_blocks;
	

	//! A lock to serialize some FS operations
	fs_lock_t lock;
	//! A lock to block any allocation-related operations
	fs_lock_t allocate_lock;
	//! A lock to serialize deletion operations
	fs_lock_t delete_lock;

	flog_timestamp_t t_allocation_ceiling;

	//! The one dirty_block
	//! @note This may only be accessed under @ref flogfs_t::allocate_lock
	flog_dirty_block_t dirty_block;
	//! The moving allocator head
	flog_block_idx_t allocate_head;
} flogfs_t;



//! A single static instance
static flogfs_t flogfs;

static inline void flog_lock_fs(){fs_lock(&flogfs.lock);}
static inline void flog_unlock_fs(){fs_unlock(&flogfs.lock);}

static inline void flog_lock_allocate(){fs_lock(&flogfs.allocate_lock);}
static inline void flog_unlock_allocate(){fs_unlock(&flogfs.allocate_lock);}

static inline void flog_lock_delete(){fs_lock(&flogfs.delete_lock);}
static inline void flog_unlock_delete(){fs_unlock(&flogfs.delete_lock);}


/*!
 @brief Go find a suitable free block to use
 @return A block. The index will be FLOG_BLOCK_IDX_INVALID if invalid.

 This attempts to claim a block from the block preallocation list and searches
 for a new one of the list is empty

 @note This requires flogfs_t::allocate_lock
 */
static flog_block_alloc_t flog_allocate_block(int32_t threshold);

/*!
 @brief Iterate the block allocation routine and return the result
 */
static flog_block_alloc_t flog_allocate_block_iterate();

/*!
 @brief Get the next block entry from any valid block
 @param block The previous block
 @returns The next block

 This is fine for both inode and file blocks

 If block == FLOG_BLOCK_IDX_INVALID, it will be returned
 */
static inline flog_block_idx_t
flog_universal_get_next_block(flog_block_idx_t block);


/*!
 @brief Perform an iteration of the preallocator. Basically check one block as
 a candidate.

 @note This requires the allocation lock, \ref flogfs_t::allocate_lock
 */
static inline void flog_prealloc_iterate();

/*!
 @brief Find a file inode entry
 @param[in] filename The filename to check for
 @param[out] iter An inode iterator -- If nothing is found, this will point to
                  the next free inode iterator
 @retval Fileinfo with first_block == FLOG_BLOCK_IDX_INVALID if not found

 @note This requires the FS lock, \ref flogfs_t::lock
 */
static flog_file_find_result_t flog_find_file(char const * filename,
                                              flog_inode_iterator_t * iter);

/*!
 @brief Open a page (read to flash cache) only if necessary
 */
static inline flog_result_t flog_open_page(uint16_t block, uint16_t page);

/*!
 @brief Open the page corresponding to a sector
 @param block The block
 @param sector The sector you wish to access
 */
static inline flog_result_t flog_open_sector(uint16_t block, uint16_t sector);


static void flog_close_sector();

/*!
 @brief Initialize an inode iterator
 @param[in,out] iter The iterator structure
 @param[in] inode0 The first inode block to use
 */
static void flog_inode_iterator_init(flog_inode_iterator_t * iter,
                                     flog_block_idx_t inode0);

/*!
 @brief Advance an inode iterator to the next entry
 @param[in,out] iter The iterator structure

 This doesn't deal with any allocation. That is done with
 flog_inode_prepare_new.

 @warning You MUST check on every iteration for the validity of the entry and
 not iterate past an unallocated entry.
 */
static void flog_inode_iterator_next(flog_inode_iterator_t * iter);

/*!
 @brief Claim a new inode entry as your own.

 @note This requires the @ref flogfs_t::allocate_lock. It might allocate
 something.

 @warning Please be sure that iter points to the first unallocated entry
 */
static flog_result_t flog_inode_prepare_new(flog_inode_iterator_t * iter);

/*!
 @brief Get the value of the next sector in sequence
 @param sector The previous sector
 @return Next sector

 Sectors are written and read out of order so this is used to get the correct
 sequence
 */
static inline uint16_t flog_increment_sector(uint16_t sector);

static flog_result_t flog_flush_write(flog_write_file_t * file);

/*!
 @brief Add a free block candidate to the preallocation list

 @note This requires the allocation lock
 */
static void flog_prealloc_push(flog_block_idx_t block,
                               flog_block_age_t age);

/*!
 @brief Take the youngest block from the preallocation list
 @retval Index The allocated block index
 @retval FLOG_BLOCK_IDX_INVALID if empty

 @note This requires the allocation lock
 */
static flog_block_alloc_t flog_prealloc_pop(int32_t threshold);

/*!
 @brief Invalidate a chain of blocks
 @param base The first block in the chain
 */
static void
flog_invalidate_chain(flog_block_idx_t base, flog_file_id_t file_id);

/*!
 @brief Check for a dirty block and flush it to allow for a new allocation

 @note This requires the allocation lock
 */
static void flog_flush_dirty_block();

/*!
 @brief check to see if a block is sufficiently new against a threshold
 @retval 1 if okay
 @retval 0 if too old
 */
static uint_fast8_t
flog_age_is_sufficient(int32_t threshold, flog_block_age_t age);

static flog_result_t flog_commit_file_sector(flog_write_file_t * file,
                                             uint8_t const * data,
                                             flog_sector_nbytes_t n);

static flog_timestamp_t flog_block_get_init_timestamp(flog_block_idx_t block);

static flog_block_age_t flog_block_get_age(flog_block_idx_t block);

static void flog_get_file_tail_sector(flog_block_idx_t block,
                                      flog_file_tail_sector_header_t * header);

static void flog_get_file_init_sector(flog_block_idx_t block,
                                      flog_file_init_sector_header_t * header);

static void
flog_get_universal_tail_sector(flog_block_idx_t block,
                               flog_universal_tail_sector_t * header);

static flog_block_type_t
flog_get_block_type(flog_block_idx_t block);

static flog_file_id_t
flog_block_get_file_id(flog_block_idx_t block);

static void
flog_write_block_stat(flog_block_idx_t block,
                      flog_block_stat_sector_t const * stat);

static void
flog_get_block_stat(flog_block_idx_t block, flog_block_stat_sector_t * stat);

//! @}


///////////////////////////////////////////////////////////////////////////////
// Public implementations
///////////////////////////////////////////////////////////////////////////////

flog_result_t flogfs_init(){
	// Initialize locks
	fs_lock_init(&flogfs.allocate_lock);
	fs_lock_init(&flogfs.lock);
	fs_lock_init(&flogfs.delete_lock);

	flogfs.state = FLOG_STATE_RESET;
	flogfs.cache_status.page_open = 0;
	flogfs.dirty_block.block = FLOG_BLOCK_IDX_INVALID;
	return flash_init();
}

flog_result_t flogfs_format(){
	flog_block_idx_t i;
	flog_block_idx_t first_valid = FLOG_BLOCK_IDX_INVALID;

	union {
		flog_inode_init_sector_t main_buffer;
		flog_inode_init_sector_spare_t spare_buffer;
        } buffer_union;
	
	struct {
		flog_block_stat_sector_t stat;
		char key[sizeof(flog_block_stat_key)];
	} stat_sector;
	
	flash_lock();
	flog_lock_fs();
	
	if(flogfs.state == FLOG_STATE_MOUNTED){
		flogfs.state = FLOG_STATE_RESET;
	}

	for(i = 0; i < FS_NUM_BLOCKS; i++){
		flog_open_page(i, 0);
		if(FLOG_SUCCESS == flash_block_is_bad()){
			continue;
		}
		flash_read_sector((uint8_t *)&stat_sector, FLOG_BLK_STAT_SECTOR,
		                  0, sizeof(stat_sector));
		if(memcmp(stat_sector.key, flog_block_stat_key,
		   sizeof(flog_block_stat_key)) != 0){
			// Actually need to initialize this block
			stat_sector.stat.age = 0;
			memcpy(stat_sector.key, flog_block_stat_key,
			       sizeof(flog_block_stat_key));
		}
		stat_sector.stat.next_block = FLOG_BLOCK_IDX_INVALID;
		stat_sector.stat.next_age = FLOG_BLOCK_AGE_INVALID;
		stat_sector.stat.timestamp = 0;
		flog_close_sector();
		// Go erase it
		if(FLOG_FAILURE == flash_erase_block(i)){
			flog_unlock_fs();
			flash_unlock();
			flash_debug_error("FLogFS:" LINESTR);
			return FLOG_FAILURE;
		}
		flog_open_sector(i, FLOG_BLK_STAT_SECTOR);
		flash_write_sector((uint8_t *)&stat_sector, FLOG_BLK_STAT_SECTOR,
		                   0, sizeof(stat_sector));
		flash_commit();
		if(first_valid == FLOG_BLOCK_IDX_INVALID){
			first_valid = i;
		}
	}

	// Really just assuming that at least 1 valid block was found

	// Write the first file table
	flog_open_sector(first_valid, FLOG_INIT_SECTOR);
        buffer_union.main_buffer.timestamp = 0;
        buffer_union.main_buffer.previous = FLOG_BLOCK_IDX_INVALID;
        flash_write_sector((const uint8_t *)&buffer_union.main_buffer,
                           FLOG_INIT_SECTOR, 0, sizeof(buffer_union.main_buffer));
        buffer_union.spare_buffer.inode_index = 0;
        buffer_union.spare_buffer.type_id = FLOG_BLOCK_TYPE_INODE;
        flash_write_spare((const uint8_t *)&buffer_union.spare_buffer, FLOG_INIT_SECTOR);
	flash_commit();

	flog_unlock_fs();
	flash_unlock();
	return FLOG_SUCCESS;
}


flog_result_t flogfs_mount(){
	uint32_t i, done_scanning;

	////////////////////////////////////////////////////////////
	// Data structures
	////////////////////////////////////////////////////////////

	// Use in search for highest allocation timestamp
	struct {
		flog_block_idx_t block;
		flog_block_age_t age;
		union {
		flog_file_id_t file_id;
		flog_block_idx_t previous_inode;
		};
		flog_timestamp_t timestamp;
		flog_block_type_t block_type;
	} last_allocation;

	struct {
		flog_block_idx_t first_block, last_block;
		flog_file_id_t   file_id;
		flog_timestamp_t timestamp;
	} last_deletion;

	// Find the freshest block to allocate. Why not?
	struct {
		flog_block_idx_t block;
		flog_block_age_t age;
	} min_age_block;

	flog_block_idx_t inode0_idx, new_inode0_idx;
	flog_timestamp_t inode0_ts;

	// Find the maximum block age
	flog_block_age_t max_block_age;

	flog_inode_iterator_t inode_iter;

	////////////////////////////////////////////////////////////
	// Flexible buffers for flash reads
	////////////////////////////////////////////////////////////

	union {
		uint8_t init_sector_buffer;
		flog_file_init_sector_header_t file_init_sector_header;
		flog_inode_init_sector_t inode_init_sector;
		flog_inode_file_invalidation_t inode_file_invalidation_sector;
        } init_buffer_union;

	union {
		uint8_t sector_buffer;
		flog_file_tail_sector_header_t file_tail_sector_header;
		flog_inode_file_allocation_header_t inode_file_allocation_sector;
		flog_universal_invalidation_header_t universal_invalidation_header;
		flog_block_stat_sector_t stat_sector;
        } sector_buffer_union;

	union {
		uint8_t spare_buffer;
		flog_inode_init_sector_spare_t inode_spare0;
		flog_file_sector_spare_t file_spare0;
        } spare_buffer_union;
	
	flog_universal_tail_sector_t universal_tail_sector;

	flog_block_age_t age;

	////////////////////////////////////////////////////////////
	// Claim the disk and get this show started
	////////////////////////////////////////////////////////////

	flog_lock_fs();

	if(flogfs.state == FLOG_STATE_MOUNTED){
		flog_unlock_fs();
		return FLOG_SUCCESS;
	}

	flash_lock();
	
	for(uint32_t i = 0; i < FS_NUM_BLOCKS/8; i++){
		flogfs.free_block_bitmap[i] = 0;
	}
	
	////////////////////////////////////////////////////////////
	// Initialize data structures
	////////////////////////////////////////////////////////////

	last_allocation.block = FLOG_BLOCK_IDX_INVALID;
	last_allocation.timestamp = 0;
	last_allocation.age = 0;

	last_deletion.timestamp = 0;
	last_deletion.file_id = FLOG_FILE_ID_INVALID;

	flogfs.num_free_blocks = 0;
	flogfs.t_allocation_ceiling = FLOG_TIMESTAMP_INVALID;
	flogfs.max_file_id = 0;
	
	flogfs.cache_status = {0};
	
	flogfs.read_head = nullptr;
	flogfs.write_head = nullptr;
	flogfs.dirty_block.block = FLOG_BLOCK_IDX_INVALID;

	min_age_block.age = 0xFFFFFFFF;
	min_age_block.block = FLOG_BLOCK_IDX_INVALID;

	inode0_idx = FLOG_BLOCK_IDX_INVALID;
	new_inode0_idx = FLOG_BLOCK_IDX_INVALID;
	inode0_ts = FLOG_TIMESTAMP_INVALID;

	max_block_age = 0;

	////////////////////////////////////////////////////////////
	// First, iterate through all blocks to find:
	// - Most recent allocation time in a file block
	// - Number of free blocks
	// - Some free blocks that are fair to use
	// - Oldest block age
	// - Inode table 0
	////////////////////////////////////////////////////////////
	for(i = 0; i < FS_NUM_BLOCKS; i++){
		// Everything can be determined from page 0
		if(FLOG_FAILURE == flash_open_page(i, 0)){
			continue;
		}
		if(FLOG_SUCCESS == flash_block_is_bad()){
			flash_debug_warn("FLogFS:" LINESTR);
			continue;
		}
		// Read the sector 0 spare to identify valid blocks
                flash_read_spare((uint8_t *)&spare_buffer_union.spare_buffer, FLOG_INIT_SECTOR);
		
                switch(spare_buffer_union.inode_spare0.type_id) {
		case FLOG_BLOCK_TYPE_INODE:
			flog_get_universal_tail_sector(i, &universal_tail_sector);
                        if(spare_buffer_union.inode_spare0.inode_index == 0){
				// Found the original gangster!
				if(inode0_idx == FLOG_BLOCK_IDX_INVALID){
					inode0_idx = i;
				} else if(flog_block_get_init_timestamp(inode0_idx) >
				          flog_block_get_init_timestamp(i)){
					// This is the older inode chain
					new_inode0_idx = inode0_idx;
					inode0_idx = i;
				} else {
					// This is the NEW inode chain!
					new_inode0_idx = i;
				}
			} else {
				// Not the first, but valid!
			}
	
			if((universal_tail_sector.timestamp != FLOG_TIMESTAMP_INVALID) &&
			   (universal_tail_sector.timestamp > last_allocation.timestamp)){
				// This is now the most recent allocation timestamp!
				last_allocation.previous_inode = i;
				last_allocation.block_type = FLOG_BLOCK_TYPE_INODE;
				goto update_last_allocation;
			}
			break;
		case FLOG_BLOCK_TYPE_FILE:
			flog_get_universal_tail_sector(i, &universal_tail_sector);
                        flog_get_file_init_sector(i, &init_buffer_union.file_init_sector_header);
			if((universal_tail_sector.timestamp != FLOG_TIMESTAMP_INVALID) &&
			   (universal_tail_sector.timestamp > last_allocation.timestamp)){
				// This is now the most recent allocation timestamp!
                                last_allocation.file_id = init_buffer_union.file_init_sector_header.file_id;
				last_allocation.block_type = FLOG_BLOCK_TYPE_FILE;
				goto update_last_allocation;
			}
			
                        if(init_buffer_union.file_init_sector_header.file_id > flogfs.max_file_id){
                                flogfs.max_file_id = init_buffer_union.file_init_sector_header.file_id;
			}

			break;
		case FLOG_BLOCK_TYPE_UNALLOCATED:
                        flog_get_block_stat(i, &sector_buffer_union.stat_sector);
			flogfs.num_free_blocks += 1;
			flogfs.free_block_bitmap[i / 8] |= (1 << (i % 8));
                        flogfs.free_block_sum += sector_buffer_union.stat_sector.age;
			break;
		default:
			flash_debug_error("FLogFS:" LINESTR);
			goto failure;
		}
		
		age = flog_block_get_age(i);
		// Check if this is a really old block
		if((age != FLOG_BLOCK_AGE_INVALID) && (age > max_block_age)){
			max_block_age = age;
		}
		
		continue;
update_last_allocation:
		last_allocation.timestamp = universal_tail_sector.timestamp;
		last_allocation.block = universal_tail_sector.next_block;
		last_allocation.age = universal_tail_sector.next_age;
	}
	
	flogfs.mean_free_age = flogfs.free_block_sum / flogfs.num_free_blocks;
	
	if(inode0_idx == FLOG_BLOCK_IDX_INVALID){
		flash_debug_error("FLogFS:" LINESTR);
		goto failure;
	}

	////////////////////////////////////////////////////////////
	// Now iterate through the inode chain, finding:
	// - Most recent file deletion
	// - Most recent file allocation
	// - Max file ID
	////////////////////////////////////////////////////////////

	done_scanning = 0;
	// THE OLD INODE CHAIN
	for(flog_inode_iterator_init(&inode_iter, inode0_idx);;
		flog_inode_iterator_next(&inode_iter)){
		
		flog_open_sector(inode_iter.block, inode_iter.sector);
                flash_read_sector(&sector_buffer_union.sector_buffer, inode_iter.sector, 0,
		                  sizeof(flog_inode_file_allocation_header_t));
                if(sector_buffer_union.inode_file_allocation_sector.file_id == FLOG_FILE_ID_INVALID){
			// Passed the last file
			// When iterating across an incomplete inode table deletion, this
			// will also catch and finish the routine
			break;
		}
		flog_open_sector(inode_iter.block, inode_iter.sector + 1);
                flash_read_sector(&init_buffer_union.init_sector_buffer, inode_iter.sector + 1, 0,
		                  sizeof(flog_inode_file_invalidation_t));

		// Keep track of the maximum file ID
                if(sector_buffer_union.inode_file_allocation_sector.file_id > flogfs.max_file_id){
                        flogfs.max_file_id = sector_buffer_union.inode_file_allocation_sector.file_id;
		}
		
		
		// Was it deleted?
                if(init_buffer_union.inode_file_invalidation_sector.timestamp ==
			FLOG_TIMESTAMP_INVALID){
			// This is still valid

			// Check if this is now the most recent allocation
                        if(sector_buffer_union.inode_file_allocation_sector.timestamp >
			   last_allocation.timestamp){
				// This isn't really always true becase we also consider
				// allocations in the file chain itself, which are not
				// reflected
				last_allocation.block =
                                  sector_buffer_union.inode_file_allocation_sector.first_block;
				last_allocation.file_id =
                                  sector_buffer_union.inode_file_allocation_sector.file_id;
				last_allocation.age =
                                  sector_buffer_union.inode_file_allocation_sector.first_block_age;
				last_allocation.timestamp =
                                  sector_buffer_union.inode_file_allocation_sector.timestamp;
				last_allocation.block_type = FLOG_BLOCK_TYPE_FILE;
			}
		} else {
			// Check if this was the most recent deletion
                        if(init_buffer_union.inode_file_invalidation_sector.timestamp >
			   last_deletion.timestamp){
				last_deletion.first_block =
                                  sector_buffer_union.inode_file_allocation_sector.first_block;
				last_deletion.last_block =
                                  init_buffer_union.inode_file_invalidation_sector.last_block;
				last_deletion.file_id =
                                  sector_buffer_union.inode_file_allocation_sector.file_id;
				last_deletion.timestamp =
                                  init_buffer_union.inode_file_invalidation_sector.timestamp;
			}
		}
	}

	// Go check and (maybe) clean the last allocation
	if(last_allocation.timestamp > 0){
		switch(last_allocation.block_type){
		case FLOG_BLOCK_TYPE_FILE:
			flog_open_sector(last_allocation.block, FLOG_INIT_SECTOR);
                        flash_read_sector(&init_buffer_union.init_sector_buffer, FLOG_INIT_SECTOR, 0,
			                  sizeof(flog_file_init_sector_header_t));
                        if(init_buffer_union.file_init_sector_header.file_id != last_allocation.file_id){
				// This block never got claimed
				// Initialize it!
				flog_open_sector(last_allocation.block, FLOG_INIT_SECTOR);
                                init_buffer_union.file_init_sector_header.timestamp = last_allocation.timestamp;
                                init_buffer_union.file_init_sector_header.age = last_allocation.age;
                                init_buffer_union.file_init_sector_header.file_id = last_allocation.file_id;
                                flash_write_sector(&init_buffer_union.init_sector_buffer, FLOG_INIT_SECTOR, 0,
				                   sizeof(flog_file_init_sector_header_t));
                                spare_buffer_union.file_spare0.nbytes = 0;
                                spare_buffer_union.file_spare0.nothing = 0;
                                spare_buffer_union.file_spare0.type_id = FLOG_BLOCK_TYPE_FILE;
                                flash_write_spare(&spare_buffer_union.spare_buffer, FLOG_INIT_SECTOR);
				flash_commit();
				
				// BOOOOOO
				flogfs.num_free_blocks -= 1;
				flogfs.free_block_sum -= last_allocation.age;
				flogfs.mean_free_age = 
				   flogfs.free_block_sum / flogfs.num_free_blocks;

				flogfs.t = last_allocation.timestamp + 1;
			}
			break;
		case FLOG_BLOCK_TYPE_INODE:
			flog_inode_init_sector_spare_t inode_init_spare;
			flog_inode_init_sector_t inode_init;
			if(flog_get_block_type(last_allocation.block) ==
			   FLOG_BLOCK_TYPE_INODE)
				break;
			// Well, it seems the allocation was incomplete
			flog_open_sector(last_allocation.previous_inode, FLOG_INIT_SECTOR);
			flash_read_spare((uint8_t *)&inode_init_spare, FLOG_INIT_SECTOR);
			inode_init.previous = last_allocation.previous_inode;
			inode_init.timestamp = last_allocation.timestamp;
			inode_init_spare.inode_index += 1;
			// Other fields should be valid...
			flog_open_sector(last_allocation.block, FLOG_INIT_SECTOR);
			flash_write_sector((uint8_t *)&inode_init, FLOG_INIT_SECTOR, 0,
			                   sizeof(inode_init));
                        flash_write_spare((uint8_t *)&init_buffer_union.inode_init_sector, FLOG_INIT_SECTOR);
			flash_commit();
			
			// BOOOOOO
			flogfs.num_free_blocks -= 1;
			flogfs.free_block_sum -= last_allocation.age;
			flogfs.mean_free_age = 
			   flogfs.free_block_sum / flogfs.num_free_blocks;
			break;
		default:
			// Huh?
			break;
		}
	}

	// Verify the completion of the most recent deletion operation
	if((last_deletion.timestamp > 0) &&
	   (flog_get_block_type(last_deletion.last_block) ==
	      FLOG_BLOCK_TYPE_FILE)){

		flog_open_sector(last_deletion.last_block, FLOG_INIT_SECTOR);
                flash_read_sector(&init_buffer_union.init_sector_buffer, FLOG_INIT_SECTOR, 0,
		                  sizeof(flog_file_init_sector_header_t));
                if(init_buffer_union.file_init_sector_header.file_id == last_deletion.file_id){
			// This is the same file still, see if it's been invalidated
			flog_open_sector(last_deletion.last_block,
			                 FLOG_BLK_STAT_SECTOR);
                        flash_read_sector(&sector_buffer_union.sector_buffer, FLOG_BLK_STAT_SECTOR, 0,
			                  sizeof(flog_universal_invalidation_header_t));
                        if(sector_buffer_union.universal_invalidation_header.timestamp != FLOG_TIMESTAMP_INVALID){
				// Crap, this never got invalidated correctly
				flog_invalidate_chain(last_deletion.first_block,
				                      last_deletion.file_id);
			}
		}
	}

	flogfs.state = FLOG_STATE_MOUNTED;

	flash_unlock();
	flog_unlock_fs();
	return FLOG_SUCCESS;

failure:
	flash_unlock();
	flog_unlock_fs();
	return FLOG_FAILURE;
}



flog_result_t flogfs_open_read(flog_read_file_t * file, char const * filename){
	flog_inode_iterator_t inode_iter;
	flog_read_file_t * file_iter;
	flog_file_find_result_t find_result;

	union {
		uint8_t spare_buffer;
		flog_file_sector_spare_t file_sector_spare;
        } spare_buffer_union;

	if(strlen(filename) >= FLOG_MAX_FNAME_LEN){
		return FLOG_FAILURE;
	}

	flog_lock_fs();
	flash_lock();

	find_result = flog_find_file(filename, &inode_iter);
	if(find_result.first_block == FLOG_BLOCK_IDX_INVALID){
		// File doesn't exist
		goto failure;
	}

	file->block = find_result.first_block;
	file->id = find_result.file_id;
	/////////////
	// Actual file search
	/////////////


	// Now go find the start of file data (either first or second sector)
	// and adjust some settings
	flog_open_sector(file->block, FLOG_INIT_SECTOR);
        flash_read_spare(&spare_buffer_union.spare_buffer, FLOG_INIT_SECTOR);

        if(spare_buffer_union.file_sector_spare.nbytes != 0){
		// The first sector has some stuff in it!
		file->sector = FLOG_INIT_SECTOR;
		file->offset = sizeof(flog_file_init_sector_header_t);
	} else {
		flog_open_sector(file->block, 1);
                flash_read_spare(&spare_buffer_union.spare_buffer, 1);
		file->sector = flog_increment_sector(FLOG_INIT_SECTOR);
		file->offset = 0;
	}

        file->sector_remaining_bytes = spare_buffer_union.file_sector_spare.nbytes;

	// If we got this far...

	// Add to list of read files
	file->next = 0;
	if(flogfs.read_head){
		// Iterate to end of list
		for(file_iter = flogfs.read_head; file_iter->next;
			file_iter = file_iter->next);
		file_iter->next = file;
	} else {
		flogfs.read_head = file;
	}

	flash_unlock();
	flog_unlock_fs();
	return FLOG_SUCCESS;


failure:
	flash_unlock();
	flog_unlock_fs();
	return FLOG_FAILURE;
}

flog_result_t flogfs_close_read(flog_read_file_t * file){
	flog_read_file_t * iter;
	flog_lock_fs();
	if(flogfs.read_head == file){
		flogfs.read_head = file->next;
	} else {
		iter = flogfs.read_head;
		while(iter->next){
			if(iter->next == file){
				iter->next = file->next;
				break;
			}
			iter = iter->next;
		}
		goto failure;
	}
	flog_unlock_fs();
	return FLOG_SUCCESS;

failure:
	flog_unlock_fs();
	return FLOG_FAILURE;
}

flog_result_t flogfs_check_exists(char const * filename){
	flog_inode_iterator_t inode_iter;
	flog_file_find_result_t find_result;
	flog_result_t result;
	
	flog_lock_fs();
	flash_lock();
	find_result = flog_find_file(filename, &inode_iter);
	flash_unlock();
	flog_unlock_fs();

	result = (find_result.first_block == FLOG_BLOCK_IDX_INVALID) ?
	   FLOG_FAILURE : FLOG_SUCCESS;

	return result;
}

uint32_t flogfs_read(flog_read_file_t * file, uint8_t * dst, uint32_t nbytes){
        uint32_t count = 0;
        uint16_t to_read;

	flog_block_idx_t block;
	uint16_t sector;

	union {
		uint8_t sector_header;
		flog_file_tail_sector_header_t file_tail_sector_header;
		flog_file_init_sector_header_t file_init_sector_header;
        } buffer_union;

	union {
		uint8_t sector_spare;
		flog_file_sector_spare_t file_sector_spare;
        } spare_buffer_union;

	flog_lock_fs();
	flash_lock();

	while(nbytes){
		if(file->sector_remaining_bytes == 0){
			// We are/were at the end of file, look into the existence of new data
			// This block is responsible for setting:
			// -- file->sector_remaining_bytes
			// -- file->offset
			// -- file->sector
			// -- file->block
			// and bailing on the loop if EOF is encountered
			if(file->sector == FLOG_TAIL_SECTOR){
				// This was the last sector in the block, check the next
				flog_open_sector(file->block, FLOG_TAIL_SECTOR);
                                flash_read_sector(&buffer_union.sector_header, FLOG_TAIL_SECTOR, 0,
								sizeof(flog_file_tail_sector_header_t));
                                block = buffer_union.file_tail_sector_header.next_block;
				// Now check out that new block and make sure it's legit
				flog_open_sector(block, FLOG_INIT_SECTOR);
                                flash_read_sector(&buffer_union.sector_header, FLOG_INIT_SECTOR, 0,
								sizeof(flog_file_init_sector_header_t));
                                if(buffer_union.file_init_sector_header.file_id != file->id){
					// This next block hasn't been written. EOF for now
					goto done;
				}

				file->block = block;

                                flash_read_spare(&spare_buffer_union.sector_spare, FLOG_INIT_SECTOR);
                                if(spare_buffer_union.file_sector_spare.nbytes == 0){
					// It's possible for the first sector to have 0 bytes
					// Data is in next sector
					file->sector = flog_increment_sector(FLOG_INIT_SECTOR);
				} else {
					file->sector = FLOG_INIT_SECTOR;
				}
			} else {
				// Increment to next sector but don't necessarily update file
				// state
				sector = flog_increment_sector(file->sector);

				flog_open_sector(file->block, sector);
                                flash_read_spare(&spare_buffer_union.sector_spare, sector);

                                if(spare_buffer_union.file_sector_spare.nbytes == FLOG_SECTOR_NBYTES_INVALID){
					// We're looking at an empty sector, GTFO
					goto done;
				} else {
					file->sector = sector;
				}
			}

                        file->sector_remaining_bytes = spare_buffer_union.file_sector_spare.nbytes;
			switch(file->sector){
			case FLOG_TAIL_SECTOR:
				file->offset = sizeof(flog_file_tail_sector_header_t);
				break;
			case FLOG_INIT_SECTOR:
				file->offset = sizeof(flog_file_init_sector_header_t);
				break;
			default:
				file->offset = 0;
			}
		}

		// Figure out how many to read
		to_read = MIN(nbytes, file->sector_remaining_bytes);

		if(to_read){
			// Read this sector now
			flog_open_sector(file->block, file->sector);
			flash_read_sector(dst, file->sector, file->offset, to_read);
			count += to_read;
			nbytes -= to_read;
			dst += to_read;
			// Update file stats
			file->offset += to_read;
			file->sector_remaining_bytes -= to_read;
			file->read_head += to_read;
		}
	}


done:
	flash_unlock();
	flog_unlock_fs();

	return count;
}

uint32_t flogfs_write(flog_write_file_t * file, uint8_t const * src,
                      uint32_t nbytes){
	uint32_t count = 0;
	flog_sector_nbytes_t bytes_written;

	flog_lock_fs();
	flash_lock();

	while(nbytes){
		if(nbytes >= file->sector_remaining_bytes){
			bytes_written = file->sector_remaining_bytes;
			if(flog_commit_file_sector(file, src,
				file->sector_remaining_bytes) == FLOG_FAILURE){
				// Couldn't allocate or something
				goto done;
			}

			// Now that sector is completely written
			src += bytes_written;
			nbytes -= bytes_written;
			count += bytes_written;
		} else {
			// This is smaller than a sector; cache it
			memcpy(file->sector_buffer + file->offset, src, nbytes);
			count += nbytes;
			file->sector_remaining_bytes -= nbytes;
			file->offset += nbytes;
			file->bytes_in_block += nbytes;
			file->write_head += nbytes;
			nbytes = 0;
		}
	}

done:
	flash_unlock();
	flog_unlock_fs();

	return count;
}

flog_result_t flogfs_seek(flog_read_file_t * file, uint32_t index){
	return FLOG_FAILURE;
}

flog_result_t flogfs_open_write(flog_write_file_t * file, char const * filename){
	flog_inode_iterator_t inode_iter;
	flog_block_alloc_t alloc_block;
	flog_file_find_result_t find_result;

	union {
                uint8_t sector_buffer;
		flog_inode_file_allocation_t inode_file_allocation_sector;
		flog_file_init_sector_header_t file_init_sector_header;
		flog_file_tail_sector_header_t file_tail_sector_header;
        } buffer_union;
	union {
		uint8_t spare_buffer;
		flog_file_sector_spare_t file_sector_spare;
        } spare_buffer_union;

	flog_lock_fs();
	flash_lock();

	find_result = flog_find_file(filename, &inode_iter);
	
	file->base_threshold = 0;

	if(find_result.first_block != FLOG_BLOCK_IDX_INVALID){
		// TODO: Make sure file isn't already open for writing

		// File already exists
		file->block = find_result.first_block;
		file->id = find_result.file_id;
		file->sector = FLOG_INIT_SECTOR;
		// Count bytes from 0
		file->write_head = 0;
		// Iterate to the end of the file
		// First check each terminated block
		while(1){
			flog_open_sector(file->block, FLOG_TAIL_SECTOR);
                        flash_read_sector(&buffer_union.sector_buffer, FLOG_TAIL_SECTOR, 0,
			                  sizeof(flog_file_tail_sector_header_t));
                        if(buffer_union.file_tail_sector_header.timestamp == FLOG_TIMESTAMP_INVALID){
				// This block is incomplete
				break;
			}
                        file->block = buffer_union.file_tail_sector_header.next_block;
                        file->write_head += buffer_union.file_tail_sector_header.bytes_in_block;
		}
		// Now file->block is the first incomplete block
		// Scan it sector-by-sector

		// Check out init sector no matter what and move on.
		// It might have no data
		flog_open_sector(file->block, FLOG_INIT_SECTOR);
                flash_read_spare(&spare_buffer_union.spare_buffer, FLOG_INIT_SECTOR);
                file->write_head += spare_buffer_union.file_sector_spare.nbytes;
		file->sector = flog_increment_sector(file->sector);
		while(1){
			// For each block in the file
			flog_open_sector(file->block, file->sector);
                        flash_read_spare(&spare_buffer_union.spare_buffer, file->sector);
                        if(spare_buffer_union.file_sector_spare.nbytes == FLOG_SECTOR_NBYTES_INVALID){
				// No data
				// We will write here!
				if(file->sector == FLOG_TAIL_SECTOR){
					file->offset = sizeof(flog_file_tail_sector_header_t);
				} else {
					file->offset = 0;
				}
				file->sector_remaining_bytes = FS_SECTOR_SIZE - file->offset;
				break;
			}
                        file->write_head += spare_buffer_union.file_sector_spare.nbytes;
			file->sector = flog_increment_sector(file->sector);
		}
	} else {
		// File doesn't exist

		// Get a new inode entry
		if(flog_inode_prepare_new(&inode_iter) != FLOG_SUCCESS){
			// Somehow couldn't allocate an inode entry
			goto failure;
		}

		// Configure inode to write
                strcpy(buffer_union.inode_file_allocation_sector.filename, filename);
                buffer_union.inode_file_allocation_sector.filename[FLOG_MAX_FNAME_LEN-1] = '\0';

		flog_lock_allocate();

		flog_flush_dirty_block();

		alloc_block = flog_allocate_block(file->base_threshold);
		if(alloc_block.block == FLOG_BLOCK_IDX_INVALID){
			flog_unlock_allocate();
			// Couldn't allocate a block
			goto failure;
		}

		flogfs.dirty_block.block = alloc_block.block;
		flogfs.dirty_block.file = file;

		flog_unlock_allocate();

                buffer_union.inode_file_allocation_sector.header.file_id = ++flogfs.max_file_id;
                buffer_union.inode_file_allocation_sector.header.first_block = alloc_block.block;
                buffer_union.inode_file_allocation_sector.header.first_block_age = ++alloc_block.age;
                buffer_union.inode_file_allocation_sector.header.timestamp = ++flogfs.t;

		// Write the new inode entry
		flog_open_sector(inode_iter.block,inode_iter.sector);
                flash_write_sector(&buffer_union.sector_buffer, inode_iter.sector, 0,
		                   sizeof(flog_inode_file_allocation_t));
		flash_commit();

		file->block = alloc_block.block;
		file->block_age = alloc_block.age;
		file->id = flogfs.max_file_id;
		file->bytes_in_block = 0;
		file->write_head = 0;
		file->sector = FLOG_INIT_SECTOR;
		file->offset = sizeof(flog_file_init_sector_header_t);
		file->sector_remaining_bytes = FS_SECTOR_SIZE -
		                               sizeof(flog_file_init_sector_header_t);
	}

	// Add it to that list
	file->next = 0;
	if(flogfs.write_head == 0){
		flogfs.write_head = file;
	} else {
		flog_write_file_t * file_iter;
		for(file_iter = flogfs.write_head;
		    file_iter->next;
		    file_iter = file_iter->next);
		file_iter->next = file;
	}

	flash_unlock();
	flog_unlock_fs();

	return FLOG_SUCCESS;

failure:
	flash_unlock();
	flog_unlock_fs();

	return FLOG_FAILURE;
}

/*!
 @details
 ### Internals
 To close a write, all outstanding data must simply be flushed to flash. If any
 blocks are newly-allocated, they must be committed.

 TODO: Deal with files that can't be flushed due to no space for allocation
 */
flog_result_t flogfs_close_write(flog_write_file_t * file){
	flog_write_file_t * iter;
	flog_result_t result;


	flog_lock_fs();
	flash_lock();

	if(flogfs.write_head == file){
		flogfs.write_head = file->next;
	} else {
		iter = flogfs.write_head;
		while(iter->next){
			if(iter->next == file){
				iter->next = file->next;
				break;
			}
			iter = iter->next;
		}
		goto failure;
	}

	result = flog_flush_write(file);

	flash_unlock();
	flog_unlock_fs();

	return result;

failure:

	flash_unlock();
	flog_unlock_fs();
	return FLOG_FAILURE;
}

flog_result_t flogfs_rm(char const * filename){
	flog_file_find_result_t find_result;
	flog_inode_iterator_t inode_iter;
	flog_block_idx_t block, next_block;	
	
	union {
		uint8_t sector_buffer;
		flog_inode_file_invalidation_t invalidation_buffer;
        } buffer_union;

	flog_lock_fs();
	flash_lock();

	find_result = flog_find_file(filename, &inode_iter);

	if(find_result.first_block == FLOG_BLOCK_IDX_INVALID){
		// Cool! The file already doesn't exist.
		// No work to be done here.
		goto failure;
	}

	// Navigate to the end to find the last block
	block = find_result.first_block;
	while(1){
		next_block = flog_universal_get_next_block(block);
		if(next_block == FLOG_BLOCK_IDX_INVALID){
			// THIS IS THE LAST BLOCK
			break;
		}
		block = next_block;
	}

	// Invalidate the inode entry
        buffer_union.invalidation_buffer.last_block = block;
        buffer_union.invalidation_buffer.timestamp = ++flogfs.t;
	flog_open_sector(inode_iter.block, inode_iter.sector + 1);
        flash_write_sector(&buffer_union.sector_buffer, inode_iter.sector + 1, 0,
	                   sizeof(flog_inode_file_invalidation_t));
	flash_commit();
	// A disk failure here can be recovered in mounting

	// Invalidate the file block chain
	flog_invalidate_chain(find_result.first_block, find_result.file_id);

	flash_unlock();
	flog_unlock_fs();
	return FLOG_SUCCESS;

failure:
	flash_unlock();
	flog_unlock_fs();
	return FLOG_FAILURE;
}





void flogfs_start_ls(flogfs_ls_iterator_t * iter){
	// TODO: Lock something?

	flog_inode_iterator_init(iter, flogfs.inode0);
}

uint_fast8_t flogfs_ls_iterate(flogfs_ls_iterator_t * iter, char * fname_dst){
	union {
		uint8_t sector_buffer;
		flog_file_id_t file_id;
		flog_timestamp_t timestamp;
        } buffer_union;
	while(1){
		flog_open_sector(iter->block, iter->sector);
                flash_read_sector(&buffer_union.sector_buffer, iter->sector,
		                  0, sizeof(flog_file_id_t));
                if(buffer_union.file_id == FLOG_FILE_ID_INVALID){
			// Nothing here. Done.
			return 0;
		}
		// Now check to see if it's valid
		flog_open_sector(iter->block, iter->sector + 1);
                flash_read_sector(&buffer_union.sector_buffer, iter->sector+1,
		                  0, sizeof(flog_timestamp_t));
                if(buffer_union.timestamp == FLOG_TIMESTAMP_INVALID){
			// This file's good
			// Now check to see if it's valid
			// Go read the filename
			flog_open_sector(iter->block, iter->sector);
			flash_read_sector((uint8_t *)fname_dst, iter->sector,
			                  sizeof(flog_inode_file_allocation_header_t),
							  FLOG_MAX_FNAME_LEN);
			fname_dst[FLOG_MAX_FNAME_LEN-1] = '\0';
			flog_inode_iterator_next(iter);
			return 1;
		} else {
			flog_inode_iterator_next(iter);
		}
	}
}

void flogfs_stop_ls(flogfs_ls_iterator_t * iter){
	// TODO: Unlock something?
}



///////////////////////////////////////////////////////////////////////////////
// Static implementations
///////////////////////////////////////////////////////////////////////////////

flog_result_t flog_commit_file_sector(flog_write_file_t * file,
                                      uint8_t const * data,
                                      flog_sector_nbytes_t n){
	flog_file_sector_spare_t file_sector_spare;
	if(file->sector == FLOG_TAIL_SECTOR){
		// We need a new block
		flog_block_alloc_t next_block;
		flog_file_tail_sector_header_t * const file_tail_sector_header =
		   (flog_file_tail_sector_header_t *) file->sector_buffer;

		flog_lock_allocate();

		flog_flush_dirty_block();

		next_block = flog_allocate_block(file->base_threshold);
		if(next_block.block == FLOG_BLOCK_IDX_INVALID){
			// Can't write the last sector without sealing the file.
			// Bailing
			flog_unlock_allocate();
			return FLOG_FAILURE;
		}

		flogfs.dirty_block.block = next_block.block;
		flogfs.dirty_block.file = file;

		flogfs.num_free_blocks -= 1;

		flog_unlock_allocate();

		// Prepare the header
		file_tail_sector_header->next_age = next_block.age + 1;
		file_tail_sector_header->next_block = next_block.block;
		file_tail_sector_header->timestamp = ++flogfs.t;
		file->bytes_in_block +=
			FS_SECTOR_SIZE - sizeof(flog_file_tail_sector_header_t);
		file_sector_spare.nbytes =
			FS_SECTOR_SIZE - sizeof(flog_file_tail_sector_header_t);
		file_tail_sector_header->bytes_in_block = file->bytes_in_block;

		flog_open_sector(file->block, FLOG_TAIL_SECTOR);
		// First write what was already buffered (and the header)
		flash_write_sector((uint8_t const *)file_tail_sector_header,
						FLOG_TAIL_SECTOR, 0, file->offset);
		// Now write the rest of the data
		if(n){
			flash_write_sector(data, FLOG_TAIL_SECTOR, file->offset, n);
		}
		flash_write_spare((uint8_t const *)&file_sector_spare,
		                  FLOG_TAIL_SECTOR);
		flash_commit();

		// Ready the file structure for the next block/sector
		file->block = next_block.block;
		file->block_age = next_block.age;
		file->sector = FLOG_INIT_SECTOR;
		file->sector_remaining_bytes =
		   FS_SECTOR_SIZE - sizeof(flog_file_init_sector_header_t);
		file->bytes_in_block = 0;
		file->offset = sizeof(flog_file_init_sector_header_t);
		file->write_head += n;
		return FLOG_SUCCESS;
	} else {
		flog_file_init_sector_header_t * const file_init_sector_header =
			(flog_file_init_sector_header_t *) file->sector_buffer;

		flog_lock_allocate();
		// So if this block is the dirty block...
		if(flogfs.dirty_block.file == file){
			flogfs.dirty_block.block = FLOG_BLOCK_IDX_INVALID;
		}
		flog_unlock_allocate();
		file_sector_spare.type_id = FLOG_BLOCK_TYPE_FILE;
		file_sector_spare.nbytes = file->offset + n;

		// We need to just write the data and advance
		if(file->sector == FLOG_INIT_SECTOR){
			// Need to prepare sector 0 header
			file_init_sector_header->file_id = file->id;
			file_init_sector_header->age = file->block_age;
			file_sector_spare.nbytes -= sizeof(flog_file_init_sector_header_t);
		}

		flog_open_sector(file->block, file->sector);
		if(file->offset){
			// This is either sector 0 or there was data already
			// First write prior data/header
			flash_write_sector(file->sector_buffer, file->sector, 0, file->offset);
		}
		if(n){
			flash_write_sector(data, file->sector, file->offset, n);
		}
		flash_write_spare((uint8_t const *)&file_sector_spare, file->sector);
		flash_commit();

		// Now update stuff for the new sector
		file->sector = flog_increment_sector(file->sector);
		if(file->sector == FLOG_TAIL_SECTOR){
			file->offset = sizeof(flog_file_tail_sector_header_t);
		} else {
			file->offset = 0;
		}
		file->bytes_in_block += n;
		file->sector_remaining_bytes = FS_SECTOR_SIZE - file->offset;
		file->write_head += n;
		return FLOG_SUCCESS;
	}
}

flog_result_t flog_flush_write (flog_write_file_t * file ){
	return flog_commit_file_sector(file, 0, 0);
}


void flog_prealloc_iterate() {
	flog_block_alloc_t block;
	block = flog_allocate_block_iterate();
	flog_prealloc_push(block.block, block.age);
}


flog_block_alloc_t flog_allocate_block_iterate(){
        flog_block_alloc_t block;

	flog_block_stat_sector_t block_stat_sector;

	block.block = FLOG_BLOCK_IDX_INVALID;
	
	if(flogfs.free_block_bitmap[flogfs.allocate_head] &
	   (1 << (flogfs.allocate_head % 8))){
		// This block is okay to look at
		flog_get_block_stat(flogfs.allocate_head, &block_stat_sector);
		block.age = block_stat_sector.age;
		block.block = flogfs.allocate_head;
	}

	// Move the head
	flogfs.allocate_head = (flogfs.allocate_head + 1) % FS_NUM_BLOCKS;

	return block;
}

static void flog_prealloc_push(flog_block_idx_t block,
                               flog_block_age_t age){
	if(flogfs.prealloc.n == 0){
		flogfs.prealloc.blocks[0].block = block;
		flogfs.prealloc.blocks[0].age = age;
		flogfs.prealloc.n = 1;
		flogfs.prealloc.age_sum += age;
		return;
	}

	if((flogfs.prealloc.n == FS_PREALLOCATE_SIZE) &&
	   (flogfs.prealloc.blocks[flogfs.prealloc.n - 1].age < age)){
		// This block sucks
		return;
	}
	for(flog_block_idx_t i = flogfs.prealloc.n - 1; i; i--){
		// Search for the right place (start at end)
		if(age <= flogfs.prealloc.blocks[i].age){
			if(flogfs.prealloc.n == FS_PREALLOCATE_SIZE){
				// Last block will be discarded
				flogfs.prealloc.age_sum -=
				  flogfs.prealloc.blocks[FS_PREALLOCATE_SIZE - 1].age;
			}
			for(flog_block_idx_t j = MIN(flogfs.prealloc.n,
			                             FS_PREALLOCATE_SIZE - 1);
			    j > i; j--){
				// Shift all older blocks
				flogfs.prealloc.blocks[j].age =
				   flogfs.prealloc.blocks[j - 1].age;
				flogfs.prealloc.blocks[j].block =
				   flogfs.prealloc.blocks[j - 1].block;
			}
			if(flogfs.prealloc.n < FS_PREALLOCATE_SIZE){
				flogfs.prealloc.n += 1;
			}
			flogfs.prealloc.blocks[i].age = age;
			flogfs.prealloc.blocks[i].block = block;

			flogfs.prealloc.age_sum += age;
			return;
		}
	}
}

uint_fast8_t flog_age_is_sufficient(int32_t threshold,
                                    flog_block_age_t age){
	if((int32_t)flogfs.mean_free_age - (int32_t)age >= threshold)
		return 1;
	return 0;
}

static flog_block_alloc_t flog_prealloc_pop(int32_t threshold) {
	flog_block_alloc_t block;
	if((flogfs.prealloc.n == 0) ||
	   !flog_age_is_sufficient(threshold, flogfs.prealloc.blocks[0].age)){
		block.block = FLOG_BLOCK_IDX_INVALID;
		return block;
	}

	block.block = flogfs.prealloc.blocks[0].block;
	block.age = flogfs.prealloc.blocks[0].age;

	// Shift all of the other entries forward
	flogfs.prealloc.n -= 1;
	for(flog_block_idx_t i = 0; i < flogfs.prealloc.n; i++){
		flogfs.prealloc.blocks[i].age = flogfs.prealloc.blocks[i + 1].age;
		flogfs.prealloc.blocks[i].block = flogfs.prealloc.blocks[i + 1].block;
	}
	return block;
}

static flog_result_t flog_open_page(uint16_t block, uint16_t page){
	if(flogfs.cache_status.page_open &&
	   (flogfs.cache_status.current_open_block == block) &&
	   (flogfs.cache_status.current_open_page == page)){
		return flogfs.cache_status.page_open_result;
	}
	flogfs.cache_status.page_open_result = flash_open_page(block, page);
	flogfs.cache_status.page_open = 1;
	flogfs.cache_status.current_open_block = block;
	flogfs.cache_status.current_open_page = page;

	return flogfs.cache_status.page_open_result;
}

flog_result_t flog_open_sector(uint16_t block, uint16_t sector){
	return flog_open_page(block, sector / FS_SECTORS_PER_PAGE);
}

void flog_close_sector(){
	flogfs.cache_status.page_open = 0;
}

flog_block_idx_t
flog_universal_get_next_block(flog_block_idx_t block){
	if(block == FLOG_BLOCK_IDX_INVALID)
		return block;
	flog_open_sector(block, FLOG_TAIL_SECTOR);
	flash_read_sector((uint8_t*)&block, FLOG_TAIL_SECTOR,
	                  0, sizeof(block));
	return block;
}


void flog_inode_iterator_init(flog_inode_iterator_t * iter,
                              flog_block_idx_t inode0){
        union {
		uint8_t spare_buffer;
		flog_inode_init_sector_spare_t inode_init_sector_spare;
        } buffer_union;
	iter->block = inode0;
	flog_open_sector(inode0, FLOG_TAIL_SECTOR);
	flash_read_sector((uint8_t *)&iter->next_block, FLOG_TAIL_SECTOR, 0,
	                  sizeof(flog_block_idx_t));
	// Get the current inode block index
	flog_open_sector(inode0, FLOG_INIT_SECTOR);
        flash_read_spare(&buffer_union.spare_buffer, FLOG_INIT_SECTOR);
        iter->inode_block_idx = buffer_union.inode_init_sector_spare.inode_index;

	// This is zero anyways
	iter->inode_idx = 0;
	iter->sector = FLOG_INODE_FIRST_ENTRY_SECTOR;
}

/*!
 @details
 ### Internals
 Inode entries are organized sequentially in pairs of sectors following the
 first page. The first page contains simple header information. To iterate to
 the next entry, we simply advance by two sectors. If this goes past the end of
 the block, the next block is checked. If the next block hasn't yet been
 allocated, the sector is set to invalid to indicate that the next block has to
 be allocated using flog_inode_prepare_new().
 */
void flog_inode_iterator_next(flog_inode_iterator_t * iter){
	iter->sector += 2;
	iter->inode_idx += 1;
	if(iter->sector >= FS_SECTORS_PER_BLOCK){
		// The next sector is in ANOTHER BLOCK!!!
		if(iter->next_block != FLOG_BLOCK_IDX_INVALID){
			// The next block actually already exists
			iter->block = iter->next_block;
			// Check the next block
			iter->next_block = flog_universal_get_next_block(iter->block);

			// Point to the first inode sector of the next block
			iter->sector = FLOG_INODE_FIRST_ENTRY_SECTOR;
		} else {
			// The next doesn't exist
			// WTF?
			flash_debug_warn("FLogFS:" LINESTR);
			// Don't do anything; this is dumb
			iter->sector -= 2;
			iter->inode_idx -= 1;
		}
	}
}


flog_block_idx_t
flog_inode_get_prev_block(flog_block_idx_t block){
	if(block == FLOG_BLOCK_IDX_INVALID)
		return block;
	flog_open_sector(block, FLOG_INIT_SECTOR);
	flash_read_sector((uint8_t*)&block, FLOG_INIT_SECTOR,
	                  sizeof(flog_timestamp_t), sizeof(block));
	return block;
}

void flog_inode_iterator_prev(flog_inode_iterator_t * iter){
	flog_block_idx_t previous;
	if((iter->sector - 2) < FLOG_INODE_FIRST_ENTRY_SECTOR){
		// Need to go to previous block
		previous = flog_inode_get_prev_block(iter->block);
		if(previous == FLOG_BLOCK_IDX_INVALID){
			// Already at beginning of chain
			return;
		}

		iter->next_block = iter->block;
		iter->block = previous;
		iter->sector = FS_SECTORS_PER_BLOCK - 2;
	} else {
		iter->sector -= 2;
	}
	
	iter->inode_idx -= 1;
}

flog_result_t flog_inode_prepare_new (flog_inode_iterator_t * iter) {
	flog_block_alloc_t block_alloc;
        union {
		uint8_t sector_buffer;
		flog_universal_tail_sector_t inode_tail_sector;
		flog_inode_init_sector_t inode_init_sector;
		flog_inode_init_sector_spare_t inode_init_sector_spare;
        } buffer_union;
	if(iter->sector == FS_SECTORS_PER_BLOCK - 2){
		if(iter->next_block != FLOG_BLOCK_IDX_INVALID){
			flash_debug_warn("FLogFS:" LINESTR);
		}
		// TODO: In init, set previous block
		// We are at the last entry of the inode block
		// This entry is valid and will be used but now is the time to allocate
		// the next block

		flog_lock_allocate();

		flog_flush_dirty_block();

		block_alloc = flog_allocate_block(0);
		if(block_alloc.block == FLOG_BLOCK_IDX_INVALID){
			// Couldn't allocate a new block!
			flog_unlock_allocate();
			return FLOG_FAILURE;
		}

		flogfs.num_free_blocks -= 1;

		flog_unlock_allocate();

		// Go write the tail sector
		flog_open_sector(iter->block, FLOG_TAIL_SECTOR);
                buffer_union.inode_tail_sector.next_age = block_alloc.age + 1;
                buffer_union.inode_tail_sector.next_block = block_alloc.block;
                buffer_union.inode_tail_sector.timestamp = ++flogfs.t;
                flash_write_sector(&buffer_union.sector_buffer, FLOG_TAIL_SECTOR, 0,
		                   sizeof(flog_universal_tail_sector_t));
		flash_commit();

		// And prepare the header
		flog_open_sector(block_alloc.block, FLOG_INIT_SECTOR);
                buffer_union.inode_init_sector.timestamp = flogfs.t;
                flash_write_sector(&buffer_union.sector_buffer, FLOG_INIT_SECTOR, 0,
		                   sizeof(flog_inode_init_sector_t));
                buffer_union.inode_init_sector_spare.type_id = FLOG_BLOCK_TYPE_INODE;
                buffer_union.inode_init_sector_spare.inode_index = ++iter->inode_block_idx;
                flash_write_spare(&buffer_union.sector_buffer, 0);
		flash_commit();

		iter->next_block = block_alloc.block;
	}
	// Otherwise this is a completely okay sector to use

	return FLOG_SUCCESS;
}

flog_file_id_t flog_block_get_file_id(flog_block_idx_t block){
	flog_file_id_t id;
	flog_open_sector(block, FLOG_INIT_SECTOR);
	flash_read_sector((uint8_t *)&id, FLOG_INIT_SECTOR,
	                  sizeof(flog_block_age_t), sizeof(flog_file_id_t));
	return id;
}

void flog_write_block_stat(flog_block_idx_t block,
                           flog_block_stat_sector_t const * stat){
	flog_open_sector(block, FLOG_BLK_STAT_SECTOR);
	flash_write_sector((uint8_t const *)stat,
	                   FLOG_BLK_STAT_SECTOR, 0,
	                   sizeof(flog_block_stat_sector_t));
	flash_commit();
}

void flog_get_block_stat(flog_block_idx_t block,
                         flog_block_stat_sector_t * stat){
	flog_open_sector(block, FLOG_BLK_STAT_SECTOR);
	flash_read_sector((uint8_t *)stat,
	                   FLOG_BLK_STAT_SECTOR, 0,
	                   sizeof(flog_block_stat_sector_t));
}

void flog_invalidate_chain (flog_block_idx_t base, flog_file_id_t file_id) {
	union {
		uint8_t invalidation_sector_buffer;
		flog_file_invalidation_sector_t file_invalidation_sector;
		flog_file_init_sector_header_t init_sector;
        } init_buffer_union;
	union {
		uint8_t tail_sector_buffer;
		flog_file_tail_sector_header_t file_tail_sector;
        } tail_buffer_union;
	flog_block_stat_sector_t block_stat;
	
	flog_block_idx_t num_freed = 0;
	
	flog_lock_delete();
	
	flogfs.t_allocation_ceiling = flogfs.t;

	while(1){
		// Stop loop after encoutering a block with no next block
		// ...or a block assigned to a different file
		//    since that could only happen if the operation had completed

		// First check if the block is free
		// If so, check the next block
		
		if(base == FLOG_BLOCK_IDX_INVALID){
			break;
		}

		switch(flog_get_block_type(base)){
		case FLOG_BLOCK_TYPE_UNALLOCATED:
			flog_get_block_stat(base, &block_stat);
			base = block_stat.next_block;
			goto done;
			break;
// 		case FLOG_BLOCK_TYPE_ERROR:
// 			break;
// 		case FLOG_BLOCK_TYPE_INODE:
// 			// It's already been allocated to something else, quit
// 			return;
		case FLOG_BLOCK_TYPE_FILE:
			// Check if this is indeed still the correct file
			flog_open_sector(base, FLOG_INIT_SECTOR);
                        flash_read_sector((uint8_t *)&init_buffer_union.init_sector, FLOG_INIT_SECTOR, 0,
			                  sizeof(flog_file_init_sector_header_t));
			if((file_id != FLOG_FILE_ID_INVALID) &&
                           (init_buffer_union.init_sector.file_id == file_id)){
				// Well it's time to invalidate this
				// Get the age of this block
				// The the age and index of the next block
                                block_stat.age = init_buffer_union.init_sector.age;
				flog_open_sector(base, FLOG_TAIL_SECTOR);
                                flash_read_sector((uint8_t *)&tail_buffer_union.file_tail_sector,
				                  FLOG_TAIL_SECTOR, 0,
				                  sizeof(flog_file_tail_sector_header_t));
                                block_stat.next_block = tail_buffer_union.file_tail_sector.next_block;
                                block_stat.next_age = tail_buffer_union.file_tail_sector.next_age;
				block_stat.timestamp = ++flogfs.t;
				// Need to clear cache
				flog_close_sector();
				
				flash_erase_block(base);
				
				flog_write_block_stat(base, &block_stat);
				flogfs.free_block_bitmap[base / 8] |= 1 << (base % 8);
				
				num_freed += 1;
				
				flogfs.free_block_sum += block_stat.age;
				
				base = block_stat.next_block;
			}
			break;
		default:
			while(1);
			break;
		}

	}
done:
	flogfs.num_free_blocks += num_freed;
	flogfs.mean_free_age = flogfs.free_block_sum / flogfs.num_free_blocks;
	flogfs.t_allocation_ceiling = FLOG_TIMESTAMP_INVALID;
	flog_unlock_delete();
}

flog_block_type_t flog_get_block_type(flog_block_idx_t block){
	uint8_t type_id[4];
	if(flog_open_sector(block, FLOG_INIT_SECTOR) != FLOG_SUCCESS){
		return FLOG_BLOCK_TYPE_ERROR;
	}
	flash_read_spare(type_id, FLOG_INIT_SECTOR);
	return (flog_block_type_t)type_id[0];
}

flog_block_alloc_t flog_allocate_block(int32_t threshold){
	flog_block_alloc_t block;

	// Don't lock because that should be done at higher level

	//flog_lock_allocate();
	if(flogfs.num_free_blocks == 0){
		// No free blocks in the system. GTFO.
		block.block = FLOG_BLOCK_IDX_INVALID;
		//flog_unlock_allocate();
		return block;
	}

	// Preallocate is empty
	// Go search for another
	// TODO: Make this efficient
	for(flog_block_idx_t i = FS_NUM_BLOCKS; i; i--){
		block = flog_prealloc_pop(threshold);
		if(block.block != FLOG_BLOCK_IDX_INVALID){
			// Got a block! Yahtzee!
			//flog_unlock_allocate();
			flogfs.num_free_blocks -= 1;
			flogfs.free_block_sum -= block.age;
			flogfs.mean_free_age = 
			   flogfs.free_block_sum / flogfs.num_free_blocks;
			return block;
		}
		
		block = flog_allocate_block_iterate();
		if(block.block != FLOG_BLOCK_IDX_INVALID){
			// Found a block
			if(flog_age_is_sufficient(threshold, block.age)){
				flogfs.free_block_bitmap[block.age / 8] &=
				   ~(1 << (block.age % 8));
				// BOOOOOO
				flogfs.num_free_blocks -= 1;
				flogfs.free_block_sum -= block.age;
				flogfs.mean_free_age = 
				   flogfs.free_block_sum / flogfs.num_free_blocks;
				// It's actually okay!
				break;
			} else {
				// Leave it for someone else
				flog_prealloc_push(block.block, block.age);
			}
		}
		threshold -= 1;
	}
	//flog_unlock_allocate();

	return block;
}

uint16_t flog_increment_sector(uint16_t sector){
	switch(sector){
	case FLOG_TAIL_SECTOR - 1:
		return FS_SECTORS_PER_PAGE;
	case FS_SECTORS_PER_BLOCK - 1:
		return FLOG_TAIL_SECTOR;
	default:
		return sector + 1;
	}
}

flog_file_find_result_t flog_find_file(char const * filename,
                                       flog_inode_iterator_t * iter){
	union {
                uint8_t sector_buffer;
		flog_inode_file_allocation_t inode_file_allocation_sector;
		flog_inode_file_invalidation_t inode_file_invalidation_sector;
        } buffer_union;

	flog_file_find_result_t result;

	for(flog_inode_iterator_init(iter, flogfs.inode0);;
	    flog_inode_iterator_next(iter)){

		/////////////
		// Inode search
		/////////////

		// Check if the entry is valid
		flog_open_sector(iter->block, iter->sector);
                flash_read_sector(&buffer_union.sector_buffer, iter->sector, 0,
		                  sizeof(flog_inode_file_allocation_t));

                if(buffer_union.inode_file_allocation_sector.header.file_id ==
		   FLOG_FILE_ID_INVALID){
			// This file is the end.
			// Do a quick check to make sure there are no foolish errors
			if(iter->next_block != FLOG_BLOCK_IDX_INVALID){
				flash_debug_warn("FLogFS:" LINESTR);
			}
			goto failure;
		}

		// Check if the name matches
                if(strncmp(filename, buffer_union.inode_file_allocation_sector.filename,
		   FLOG_MAX_FNAME_LEN) != 0){
			continue;
		}

                result.first_block = buffer_union.inode_file_allocation_sector.header.first_block;
                result.file_id = buffer_union.inode_file_allocation_sector.header.file_id;

		// Now check if it's been deleted
		flog_open_sector(iter->block, iter->sector+1);
                flash_read_sector(&buffer_union.sector_buffer, iter->sector+1, 0,
		                  sizeof(flog_timestamp_t));

                if(buffer_union.inode_file_invalidation_sector.timestamp != FLOG_TIMESTAMP_INVALID){
			// This one is invalid
			continue;
		}

		// This seems to be fine
		return result;
	}

failure:
	result.first_block = FLOG_BLOCK_IDX_INVALID;
	return result;
}

void flog_flush_dirty_block(){
	if(flogfs.dirty_block.block != FLOG_BLOCK_IDX_INVALID){
		flog_flush_write(flogfs.dirty_block.file);
		flogfs.dirty_block.block = FLOG_BLOCK_IDX_INVALID;
	}
}


flog_timestamp_t flog_block_get_init_timestamp(flog_block_idx_t block){
	flog_timestamp_t ts;
	flog_open_sector(block, FLOG_INIT_SECTOR);
	flash_read_sector((uint8_t *)&ts, FLOG_INIT_SECTOR,
	                  0, sizeof(flog_timestamp_t));
	return ts;
}

flog_block_age_t flog_block_get_age(flog_block_idx_t block){
	flog_block_age_t age;
	flog_open_sector(block, FLOG_BLK_STAT_SECTOR);
	flash_read_sector((uint8_t *)&age, FLOG_BLK_STAT_SECTOR,
	                  0, sizeof(flog_block_age_t));
	return age;
}

void flog_get_file_tail_sector(flog_block_idx_t block,
                               flog_file_tail_sector_header_t * header){
	flog_open_sector(block, FLOG_TAIL_SECTOR);
	flash_read_sector((uint8_t *)header, FLOG_TAIL_SECTOR, 0,
	                  sizeof(flog_file_tail_sector_header_t));
}

void flog_get_file_init_sector(flog_block_idx_t block,
                               flog_file_init_sector_header_t * header){
	flog_open_sector(block, FLOG_INIT_SECTOR);
	flash_read_sector((uint8_t *)header, FLOG_INIT_SECTOR, 0,
	                  sizeof(flog_file_init_sector_header_t));
}

void flog_get_universal_tail_sector(flog_block_idx_t block,
                                    flog_universal_tail_sector_t * header){
	flog_open_sector(block, FLOG_TAIL_SECTOR);
	flash_read_sector((uint8_t *)header, FLOG_TAIL_SECTOR, 0,
	                  sizeof(flog_universal_tail_sector_t));
}


#ifndef IS_DOXYGEN
#if !FLOG_BUILD_CPP
#ifdef __cplusplus
};
#endif
#endif
#endif
