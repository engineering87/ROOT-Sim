/**
* @file mm/checkpoints.c
*
* @brief State saving and restore for model state buffers
*
* State saving and restore for model state buffers
*
* @copyright
* Copyright (C) 2008-2019 HPDCS Group
* https://hpdcs.github.io
*
* This file is part of ROOT-Sim (ROme OpTimistic Simulator).
*
* ROOT-Sim is free software; you can redistribute it and/or modify it under the
* terms of the GNU General Public License as published by the Free Software
* Foundation; only version 3 of the License applies.
*
* ROOT-Sim is distributed in the hope that it will be useful, but WITHOUT ANY
* WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
* A PARTICULAR PURPOSE. See the GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along with
* ROOT-Sim; if not, write to the Free Software Foundation, Inc.,
* 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*
* @author Roberto Toccaceli
* @author Alessandro Pellegrini
* @author Roberto Vitali
* @author Francesco Quaglia
*/
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdint.h>

#include <mm/dymelor.h>
#include <core/timer.h>
#include <core/core.h>
#include <core/init.h>
#include <mm/mm.h>
#include <scheduler/process.h>
#include <scheduler/scheduler.h>
#include <statistics/statistics.h>

void set_force_full(struct lp_struct *lp)
{
	lp->state_log_full_forced = true;
}

#undef spin_lock
#undef spin_unlock
#define spin_lock(...) {}
#define spin_unlock(...) {}


/**
* This function creates a full log of the current simulation states and returns a pointer to it.
* The algorithm behind this function is based on packing of the really allocated memory chunks into
* a contiguous memory area, exploiting some threshold-based approach to fasten it up even more.
* The contiguous copy is performed precomputing the size of the full log, and then scanning it
* using a pointer for storing the relevant information.
*
* For further information, please see the paper:
* 	R. Toccaceli, F. Quaglia
* 	DyMeLoR: Dynamic Memory Logger and Restorer Library for Optimistic Simulation Objects
* 	with Generic Memory Layout
*	Proceedings of the 22nd Workshop on Principles of Advanced and Distributed Simulation
*	2008
*
* To fully understand the changes in this function to support the incremental logging as well,
* please point to the paper:
* 	A. Pellegrini, R. Vitali, F. Quaglia
* 	Di-DyMeLoR: Logging only Dirty Chunks for Efficient Management of Dynamic Memory Based
* 	Optimistic Simulation Objects
*	Proceedings of the 23rd Workshop on Principles of Advanced and Distributed Simulation
*	2009
*
* @author Roberto Toccaceli
* @author Francesco Quaglia
* @author Alessandro Pellegrini
* @author Roberto Vitali
*
* @param lp A pointer to the lp_struct of the LP for which we are taking
*           a full log of the buffers keeping the current simulation state.
* @return A pointer to a malloc()'d memory area which contains the full log of the current simulation state,
*         along with the relative meta-data which can be used to perform a restore operation.
*
* @todo must be declared static. This will entail changing the logic in gvt.c to save a state before rebuilding.
*/
void *log_full(struct lp_struct *lp)
{

	void *ptr = NULL, *ckpt = NULL;
	int i;
	size_t size, chunk_size, bitmap_size;
	malloc_area *m_area;
	malloc_state *m_state;

	// Timers for self-tuning of the simulation platform
	timer checkpoint_timer;
	timer_start(checkpoint_timer);

	m_state = lp->mm->m_state;
	m_state->is_incremental = false;
	size = get_log_size(m_state);

	ckpt = __real_malloc(size);

	if (unlikely(ckpt == NULL)) {
		rootsim_error(true, "(%d) Unable to acquire memory for checkpointing the current state (memory exhausted?)", lp->lid.to_int);
	}

	ptr = ckpt;

	// Copy malloc_state in the ckpt
	__real_memcpy(ptr, m_state, sizeof(malloc_state));
	ptr = (void *)((char *)ptr + sizeof(malloc_state));
	((malloc_state *) ckpt)->timestamp = lvt(lp);

	for (i = 0; i < m_state->num_areas; i++) {

		m_area = &m_state->areas[i];

		// Copy the bitmap
		bitmap_size = bitmap_required_size(m_area->num_chunks);


		// Reset Dirty Bitmap, as either we will have a full ckpt in the chain now
		// or there's nothing to log in this area
		m_area->state_changed = 0;
		m_area->dirty_chunks = 0;
		if (likely(m_area->use_bitmap != NULL)) {
			__real_memset(m_area->dirty_bitmap, 0, bitmap_size);
		}

		// Check if there is at least one chunk used in the area
		if (unlikely(m_area->alloc_chunks == 0)) {
			continue;
		}
		// Copy malloc_area into the ckpt
		__real_memcpy(ptr, m_area, sizeof(malloc_area));
		ptr = (void *)((char *)ptr + sizeof(malloc_area));

		__real_memcpy(ptr, m_area->use_bitmap, bitmap_size);
		ptr = (void *)((char *)ptr + bitmap_size);

		chunk_size = UNTAGGED_CHUNK_SIZE(m_area);

		// Check whether the area should be completely copied (not on a per-chunk basis)
		// using a threshold-based heuristic
		if (CHECK_LOG_MODE_BIT(m_area)) {

			// If the malloc_area is almost (over a threshold) full, copy it entirely
			__real_memcpy(ptr, m_area->area, m_area->num_chunks * chunk_size);
			ptr = (void *)((char *)ptr + m_area->num_chunks * chunk_size);

		} else {

#define copy_from_area(x) ({\
			__real_memcpy(ptr, (void*)((char*)m_area->area + ((x) * chunk_size)), chunk_size);\
			ptr = (void*)((char*)ptr + chunk_size);})

			// Copy only the allocated chunks
			bitmap_foreach_set(m_area->use_bitmap, bitmap_size, copy_from_area);

#undef copy_from_area
		}

	} // For each malloc area

	// Sanity check
	if (unlikely((char *)ckpt + size != ptr))
		rootsim_error(true, "Actual (full) ckpt size is wrong by %d bytes!\nlid = %d ckpt = %p size = %#x (%d), ptr = %p, ckpt + size = %p\n",
			      (char *)ckpt + size - (char *)ptr, lp->lid.to_int,
			      ckpt, size, size, ptr, (char *)ckpt + size);

	m_state->total_inc_size = sizeof(malloc_state);

	statistics_post_data(lp, STAT_CKPT_TIME, (double)timer_value_micro(checkpoint_timer));
	statistics_post_data(lp, STAT_CKPT_MEM, (double)size);

	return ckpt;
}

/**
* This function creates a partial (incremental) log of the current simulation states and returns
* a pointer to it.
* The algorithm behind this function is based on the information retrieved from the dirty bitmap,
* which is set accordingly to memory-write access patterns through the subroutine calls injected
* at compile time by the intrumentor.
* Similarly to full logs, a partial log is a contiguous memory area, holding metadata for a subsequent
* restore. Differently from it, a partial log holds only malloc_areas and chunks which have been
* modified since the last log operation.
* The contiguous copy is performed precomputing the size of the full log, and then scanning it
* using a pointer for storing the relevant information.
*
* For further information, please see the paper:
* 	A. Pellegrini, R. Vitali, F. Quaglia
* 	Di-DyMeLoR: Logging only Dirty Chunks for Efficient Management of Dynamic Memory Based
* 	Optimistic Simulation Objects
*	Proceedings of the 23rd Workshop on Principles of Advanced and Distributed Simulation
*	2009
*
* @author Alessandro Pellegrini
* @author Roberto Vitali
*
* @param lp A pointer to the lp_struct of the LP for which we are taking
*           a full log of the buffers keeping the current simulation state.
* @return A pointer to a malloc()'d memory area which contains the full log of the current simulation state,
*         along with the relative meta-data which can be used to perform a restore operation.
*/
void *log_incremental(struct lp_struct *lp) {
	unsigned char *ptr;
	malloc_state *ckpt;
	int i;
	size_t size, chunk_size, bitmap_size;
	malloc_area *m_area;
	malloc_state *m_state = lp->mm->m_state;

	timer checkpoint_timer;
	timer_start(checkpoint_timer);

	size = m_state->total_inc_size;
	ckpt = __real_malloc(size);
	ptr = (unsigned char *)ckpt;

	// Copy malloc_state into the log
	__real_memcpy(ptr, m_state, sizeof(malloc_state));
	ptr += sizeof(malloc_state);
	ckpt->timestamp = lvt(lp);
	ckpt->is_incremental = true;

	for(i = 0; i < m_state->num_areas; i++){

		m_area = &m_state->areas[i];

		bitmap_size = bitmap_required_size(m_area->num_chunks);

		// Check if there is at least one chunk used in the area
		if(m_area->state_changed == 0){
			if(m_area->dirty_chunks)
				rootsim_error(true, "State unchanged and dirty chunks");
			continue;
		}

		// Copy malloc_area into the log
		__real_memcpy(ptr, m_area, sizeof(malloc_area));
		ptr += sizeof(malloc_area);

		// The area has at least one allocated chunk. Copy the bitmap.
                __real_memcpy(ptr, m_area->use_bitmap, bitmap_size);
                ptr += bitmap_size;

                if (m_area->dirty_chunks == 0){
                	m_area->state_changed = 0;
                	continue;
                }

                __real_memcpy(ptr, m_area->dirty_bitmap, bitmap_size);
                ptr += bitmap_size;

		chunk_size = UNTAGGED_CHUNK_SIZE(m_area);

#define copy_from_area(x) ({\
			__real_memcpy(ptr, (char*)m_area->area + ((x) * chunk_size), chunk_size);\
			ptr = ptr + chunk_size;\
		})

		// Copy only the dirtied chunks
		bitmap_foreach_set(m_area->dirty_bitmap, bitmap_size, copy_from_area);
#undef copy_from_area

		m_area->state_changed = 0;
		m_area->dirty_chunks = 0;
		__real_memset(m_area->dirty_bitmap, 0, bitmap_size);
	} // for each dirty m_area in m_state

	// Sanity check
	if ((uintptr_t)ptr != (uintptr_t)ckpt + size){
		rootsim_error(true, "Actual (inc) log size different from the estimated one! Aborting...\n\tlog = %x expected size = %d, actual size = %d, ptr = %x\n", ckpt, size, (uintptr_t)ptr-(uintptr_t)ckpt, ptr);
	}

	m_state->total_inc_size = sizeof(malloc_state);

	statistics_post_data(lp, STAT_CKPT_TIME, (double)timer_value_micro(checkpoint_timer));
	statistics_post_data(lp, STAT_CKPT_MEM, (double)size);

	return ckpt;
}

/**
* This function is the only log function which should be called from the simulation platform. Actually,
* it is a demultiplexer which calls the correct function depending on the current configuration of the
* platform. Note that this function only returns a pointer to a malloc'd area which contains the
* state buffers. This means that this memory area cannot be used as-is, but should be wrapped
* into a state_t structure, which gives information about the simulation state pointer (defined
* via SetState() by the application-level code and the lvt associated with the log.
* This is done implicitly by the LogState() function, which in turn connects the newly taken
* snapshot with the currencly-scheduled LP.
* Therefore, any point of the simulator which wants to take a (real) log, shouldn't call directly this
* function, rather LogState() should be used, after having correctly set current and current_lvt.
*
* @author Alessandro Pellegrini
*
* @param lp A pointer to the lp_struct of the LP for which we want to take
*           a snapshot of the buffers used by the model to keep state variables.
* @return A pointer to a malloc()'d memory area which contains the log of the current simulation state,
*         along with the relative meta-data which can be used to perform a restore operation.
*/
void *log_state(struct lp_struct *lp)
{
	statistics_post_data(lp, STAT_CKPT, 1.0);
	if(rootsim_config.snapshot != SNAPSHOT_FULL && !lp->state_log_full_forced) {
		return log_incremental(lp);
	}
	
	lp->state_log_full_forced = false;
	return log_full(lp);
}

/**
* This function restores a full log in the address space where the logical process will be
* able to use it as the current state.
* Operations performed by the algorithm are mostly the opposite of the corresponding log_full
* function.
*
* For further information, please see the paper:
* 	R. Toccaceli, F. Quaglia
* 	DyMeLoR: Dynamic Memory Logger and Restorer Library for Optimistic Simulation Objects
* 	with Generic Memory Layout
*	Proceedings of the 22nd Workshop on Principles of Advanced and Distributed Simulation
*	2008

*
* @author Roberto Toccaceli
* @author Francesco Quaglia
* @author Roberto Vitali
* @author Alessandro Pellegrini
*
* @param lp A pointer to the lp_struct of the LP for which we are restoring
*           the content of simulation state buffers, taking it from the checkpoint
* @param ckpt A pointer to the checkpoint to take the previous content of
*             the buffers to be restored
*/
void restore_full(struct lp_struct *lp, void *raw_ckpt)
{
	unsigned char *ptr, *target_ptr;
	int i, original_num_areas;
	size_t chunk_size, bitmap_size;
	malloc_area *m_area;
	malloc_state *m_state;

	// Timers for simulation platform self-tuning
	timer recovery_timer;
	timer_start(recovery_timer);
	ptr = raw_ckpt;
	target_ptr = ptr + ((malloc_state *)raw_ckpt)->total_log_size;
	m_state = lp->mm->m_state;
	original_num_areas = m_state->num_areas;

	// restore the old malloc state except for the malloc areas array
	m_area = m_state->areas;
	__real_memcpy(m_state, ptr, sizeof(malloc_state));
	m_state->areas = m_area;
	ptr += sizeof(malloc_state);

	// Scan areas and chunk to restore them
	for (i = 0; i < m_state->num_areas; i++) {

		m_area = &m_state->areas[i];

		bitmap_size = bitmap_required_size(m_area->num_chunks);

		// Reset incremental checkpoint related stuff
		m_area->state_changed = 0;
		m_area->dirty_chunks = 0;
		if (likely(m_area->use_bitmap != NULL)) {
			__real_memset(m_area->dirty_bitmap, 0, bitmap_size);
		}

		if (ptr >= target_ptr || memcmp(&m_area->idx, ptr + offsetof(malloc_area, idx), sizeof(m_area->idx))) {

			m_area->alloc_chunks = 0;
			m_area->next_chunk = 0;
			RESET_LOG_MODE_BIT(m_area);
			RESET_AREA_LOCK_BIT(m_area);

			if (likely(m_area->use_bitmap != NULL)) {
				__real_memset(m_area->use_bitmap, 0, bitmap_size);
			}
			m_area->last_access = m_state->timestamp;

			continue;
		}
		// Restore the malloc_area
		__real_memcpy(m_area, ptr, sizeof(malloc_area));
		ptr += sizeof(malloc_area);

		// Restore use bitmap
		__real_memcpy(m_area->use_bitmap, ptr, bitmap_size);
		ptr += bitmap_size;

		chunk_size = UNTAGGED_CHUNK_SIZE(m_area);

		// Check how the area has been logged
		if (CHECK_LOG_MODE_BIT(m_area)) {
			// The area has been entirely logged
			__real_memcpy(m_area->area, ptr, m_area->num_chunks * chunk_size);
			ptr += m_area->num_chunks * chunk_size;

		} else {
			// The area was partially logged.
			// Logged chunks are the ones associated with a used bit whose value is 1
			// Their number is in the alloc_chunks counter

#define copy_to_area(x) ({\
			__real_memcpy((void*)((char*)m_area->area + ((x) * chunk_size)), ptr, chunk_size);\
			ptr += chunk_size;\
		})

			bitmap_foreach_set(m_area->use_bitmap, bitmap_size, copy_to_area);
#undef copy_to_area
		}

	}

	// Check whether there are more allocated areas which are not present in the log
	if (original_num_areas > m_state->num_areas) {

		for (i = m_state->num_areas; i < original_num_areas; i++) {

			m_area = &m_state->areas[i];
			m_area->alloc_chunks = 0;
			m_area->state_changed = 0;
			m_area->dirty_chunks = 0;
			m_area->next_chunk = 0;
			m_area->last_access = m_state->timestamp;
			m_state->areas[m_area->prev].next = m_area->idx;

			RESET_LOG_MODE_BIT(m_area);
			RESET_AREA_LOCK_BIT(m_area);

			if (likely(m_area->use_bitmap != NULL)) {
				bitmap_size = bitmap_required_size(m_area->num_chunks);

				__real_memset(m_area->use_bitmap, 0, bitmap_size);
				__real_memset(m_area->dirty_bitmap, 0, bitmap_size);
			}
		}
		m_state->num_areas = original_num_areas;
	}

	m_state->timestamp = -1;
	m_state->is_incremental = false;
	m_state->total_inc_size = sizeof(malloc_state);

	statistics_post_data(lp, STAT_RECOVERY_TIME, (double)timer_value_micro(recovery_timer));
}


/**
* This function restores an incremental log in the address space where the logical process will be
* able to use it as the current state.
* Operations performed by the algorithm are mostly the opposite of the corresponding log_full
* function.
*
* For further information, please see the paper:
* 	A. Pellegrini, R. Vitali, F. Quaglia
* 	Di-DyMeLoR: Logging only Dirty Chunks for Efficient Management of Dynamic Memory Based
* 	Optimistic Simulation Objects
*	Proceedings of the 23rd Workshop on Principles of Advanced and Distributed Simulation
*	2009
*
* @author Alessandro Pellegrini
* @author Roberto Vitali
*
* @param lid The logical process' local identifier
* @param queue_node a pointer to the node in the checkpoint queue
* 		     from which to restore in the state in the
* 		     logical process.
*/
void restore_incremental(struct lp_struct *lp, state_t *queue_node) {
	unsigned char *ptr;
	malloc_state *ckpt;
	rootsim_bitmap *bitmap_pointer, **to_be_restored;
	size_t chunk_size, bitmap_size, log_size;
	malloc_area *m_area, *logged_m_area;
	state_t *curr_node;
	malloc_area *new_area;
	int i;
	malloc_state *m_state = lp->mm->m_state;

	timer recovery_timer;
	timer_start(recovery_timer);

	int original_num_areas = m_state->num_areas;

	new_area = m_state->areas;

	// Restore malloc_state
	__real_memcpy(m_state, queue_node->log, sizeof(malloc_state));

	m_state->areas = new_area;

	// max_areas: it's possible, scanning backwards, to find a greater number of active areas than the one
	// in the state we're rolling back to. Hence, max_areas keeps the greater number of areas ever reached
	// during the simulation
	to_be_restored = __real_malloc(m_state->max_num_areas * sizeof(rootsim_bitmap *));
	__real_memset(to_be_restored, 0, m_state->max_num_areas * sizeof(rootsim_bitmap *));

	curr_node = queue_node;

	// Handle incremental logs
	while(is_incremental(curr_node->log)) {

		ckpt = curr_node->log;
		ptr = curr_node->log;

		log_size = ckpt->total_inc_size;

		// Skip malloc_state
		ptr += sizeof(malloc_state);

		// Handle areas in current incremental log
		for(i = 0; i < m_state->num_areas; i++) {

		        if ((uintptr_t)ptr >= (uintptr_t)ckpt + log_size) {
                            break;
		        }

			// Get current malloc_area
		        __real_memcpy(&logged_m_area, &ptr, sizeof(ptr));
			m_area = &m_state->areas[logged_m_area->idx];

			ptr += sizeof(malloc_area);

			chunk_size = UNTAGGED_CHUNK_SIZE(logged_m_area);

			bitmap_size = bitmap_required_size(logged_m_area->num_chunks);

			// Check whether the malloc_area has not already been restored
			if(to_be_restored[logged_m_area->idx] == NULL) {

				// No chunks restored so far
				to_be_restored[logged_m_area->idx] = __real_malloc(bitmap_size);
				__real_memcpy(to_be_restored[logged_m_area->idx], ptr, bitmap_size);

				// Restore m_area
				__real_memcpy(m_area, logged_m_area, sizeof(malloc_area));
				// Restore use bitmap
				__real_memcpy(m_area->use_bitmap, ptr, bitmap_size);

				// Reset incremental checkpoint related stuff
				m_area->state_changed = 0;
				m_area->dirty_chunks = 0;
				__real_memset(m_area->dirty_bitmap, 0, bitmap_size);
			}

			// skip use bitmap
			ptr += bitmap_size;

			if (logged_m_area->dirty_chunks == 0)
				continue;

			// bitmap_pointer now points to the dirty bitmap
			// which has been previously saved in the checkpoint
			bitmap_pointer = ptr;

			// make ptr point to chunks
			ptr += bitmap_size;

#define copy_to_area_check_already(x) ({\
			if(bitmap_check(to_be_restored[logged_m_area->idx], (x))){\
				__real_memcpy((char *)m_area->area + ((x) * chunk_size), ptr, chunk_size);\
				bitmap_reset(to_be_restored[logged_m_area->idx], x);\
			}\
			ptr += chunk_size;\
		})

			bitmap_foreach_set(bitmap_pointer, bitmap_size, copy_to_area_check_already);
#undef copy_to_area_check_already

		}

		// Sanity check
		if ((uintptr_t)ptr - (uintptr_t)ckpt != log_size){
			rootsim_error(true, "The incremental log size does not match. Aborting...\n");
		}

		// Handle previous log
		curr_node = list_prev(curr_node);

		if(curr_node == NULL) {
			rootsim_error(true, "Unable to scan through the incremental log chain. The state queue has %d total checkpoints. Aborting...\n", list_size(lp->queue_states));
		}
	}


	/* Full log reached */

	// Handle areas in the full log reached
	ckpt = curr_node->log;
	ptr = curr_node->log;

        log_size = ckpt->total_log_size;

    // Skip malloc_state
	ptr += sizeof(malloc_state);

	for(i = 0; i < m_state->num_areas; i++) {

		if ((uintptr_t)ptr >= (uintptr_t)ckpt + log_size) {
		    break;
		}

		// Get current malloc_area
		__real_memcpy(&logged_m_area, &ptr, sizeof(ptr));

		m_area = (malloc_area *)&m_state->areas[logged_m_area->idx];
		ptr += sizeof(malloc_area);

		chunk_size = UNTAGGED_CHUNK_SIZE(logged_m_area);

		bitmap_size = bitmap_required_size(logged_m_area->num_chunks);

		// Check whether the malloc_area has not already been restored
		if(to_be_restored[logged_m_area->idx] == NULL) {

			// No chunks restored so far
			to_be_restored[logged_m_area->idx] = __real_malloc(bitmap_size);
			__real_memcpy(to_be_restored[logged_m_area->idx], ptr, bitmap_size);

			// Restore m_area
			__real_memcpy(m_area, logged_m_area, sizeof(malloc_area));
			// Restore use bitmap
			__real_memcpy(m_area->use_bitmap, ptr, bitmap_size);

			// Reset incremental checkpoint related stuff
			m_area->state_changed = 0;
			m_area->dirty_chunks = 0;
			__real_memset(m_area->dirty_bitmap, 0, bitmap_size);
		}

		// ptr points to use bitmap
		bitmap_pointer = ptr;

		// ptr points to chunks
		ptr += bitmap_size;


		if(CHECK_LOG_MODE_BIT(logged_m_area)){
			// The area has been logged completely.
			// So far, we have restored just the needed chunks. Anyway,
			// There are still some chunks to be skipped, at the tail of
			// the log.
			// There's no need to set the to_be_restored bitmap here, since this is the last read of it
#define copy_to_area_log_mode(x) ({\
			__real_memcpy((void*)((char*)m_area->area + ((x) * chunk_size)), ptr + ((x) * chunk_size), chunk_size);\
			bitmap_reset(to_be_restored[logged_m_area->idx], x);\
		})

			bitmap_foreach_set(to_be_restored[logged_m_area->idx], bitmap_size, copy_to_area_log_mode);
#undef copy_to_area_log_mode

			ptr += chunk_size * logged_m_area->num_chunks;
		} else {
#define copy_to_area_check_already(x) ({\
			if(bitmap_check(to_be_restored[logged_m_area->idx], x)){\
				__real_memcpy((void*)((char*)m_area->area + ((x) * chunk_size)), ptr, chunk_size);\
				bitmap_reset(to_be_restored[logged_m_area->idx], x);\
			}\
			ptr = (void*)((char*)ptr + chunk_size);\
		})

			bitmap_foreach_set(bitmap_pointer, bitmap_size, copy_to_area_check_already);
#undef copy_to_area_check_already
		}
	}

	// Sanity check
	if ((uintptr_t)ptr - (uintptr_t)ckpt != log_size){
		rootsim_error(true, "The incremental log size does not match. Aborting...\n");
	}

	// Check whether there are more allocated areas which are not present in the log
	if(original_num_areas > m_state->num_areas){

		for(i = m_state->num_areas; i < original_num_areas; i++){
			m_area = &m_state->areas[i];
			m_area->alloc_chunks = 0;
			m_area->dirty_chunks = 0;
			m_area->state_changed = 0;
			m_area->next_chunk = 0;
			m_area->last_access = m_state->timestamp;
			m_state->areas[m_area->prev].next = m_area->idx;

			RESET_LOG_MODE_BIT(m_area);
			RESET_AREA_LOCK_BIT(m_area);

			if (m_area->use_bitmap != NULL) {
				bitmap_size = bitmap_required_size(m_area->num_chunks);
				__real_memset(m_area->use_bitmap, 0, bitmap_size);
				__real_memset(m_area->dirty_bitmap, 0, bitmap_size);// Take dirty_bitmap into account as well
			}
		}
		m_state->num_areas = original_num_areas;
	}

	// Release data structures
	for(i = 0; i < m_state->max_num_areas ; i++) {
		if(to_be_restored[i] != NULL) {
			__real_free(to_be_restored[i]);
		}
	}
	__real_free(to_be_restored);

	m_state->timestamp = -1;
	m_state->is_incremental = false;
	m_state->total_inc_size = sizeof(malloc_state);

	statistics_post_data(lp, STAT_RECOVERY_TIME, (double)timer_value_micro(recovery_timer));
}


/**
* Upon the decision of performing a rollback operation, this function is invoked by the simulation
* kernel to perform a restore operation.
* This function checks the mark in the malloc_state telling whether we're dealing with a full or
* partial log, and calls the proper function accordingly
*
* @author Alessandro Pellegrini
* @author Roberto Vitali
*
* @param lp A pointer to the lp_struct of the LP for which we are restoring
*           model-specific buffers keeping the simulation state
* @param state_queue_node a pointer to a node in the state queue keeping the state
*        which must be restored in the logical process live image
*/
void log_restore(struct lp_struct *lp, state_t *state_queue_node)
{
	statistics_post_data(lp, STAT_RECOVERY, 1.0);
	if (((malloc_state *)(state_queue_node->log))->is_incremental)
		restore_incremental(lp, state_queue_node);
	else
		restore_full(lp, state_queue_node->log);
}

/**
* This function is called directly from the simulation platform kernel to delete a certain log
* during the fossil collection.
*
* @author Alessandro Pellegrini
* @author Roberto Vitali
*
* @param ckpt a pointer to the simulation state which must be deleted
*
*/
void log_delete(void *ckpt)
{
	if (likely(ckpt != NULL)) {
		__real_free(ckpt);
	}
}
