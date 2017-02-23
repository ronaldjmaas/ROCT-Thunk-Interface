/*
 * Copyright © 2014 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including
 * the next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include "libhsakmt.h"
#include "pmc_table.h"
#include "linux/kfd_ioctl.h"
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

#define BITS_PER_BYTE		CHAR_BIT

#define HSA_PERF_MAGIC4CC	0x54415348

enum perf_trace_state {
	PERF_TRACE_STATE__STOPPED = 0,
	PERF_TRACE_STATE__STARTED
};

struct perf_trace_block {
	enum perf_block_id block_id;
	uint32_t num_counters;
	uint64_t *counter_id;
	int *perf_event_fd;
};

struct perf_trace {
	uint32_t magic4cc;
	uint32_t gpu_id;
	enum perf_trace_state state;
	uint32_t num_blocks;
	void *buf;
	uint64_t buf_size;
	struct perf_trace_block blocks[0];
};

enum perf_trace_action {
	PERF_TRACE_ACTION__ACQUIRE = 0,
	PERF_TRACE_ACTION__RELEASE
};

struct perf_lockf_tbl {
	uint32_t magic4cc;
	uint32_t iommu_slots_left;
};

struct perf_counts_values {
	union {
		struct {
			u64 val;
			u64 ena;
			u64 run;
		};
		u64 values[3];
	};
};

extern int amd_hsa_thunk_lock_fd;

static HsaCounterProperties **counter_props;
static unsigned int counter_props_count;

static ssize_t readn(int fd, void *buf, size_t n)
{
	size_t left = n;
	ssize_t bytes;

	while (left) {
		bytes = read(fd, buf, left);
		if (!bytes) /* reach EOF */
			return (n - left);
		if (bytes < 0 ) {
			if (errno == EINTR) /* read got interrupted */
				continue;
			else
				return -errno;
		}
		left -= bytes;
		buf = VOID_PTR_ADD(buf, bytes);
	}
	return n;
}

static HSAKMT_STATUS init_lockf_perf_section(void)
{
	struct perf_lockf_tbl lockf_tbl;
	HSAKMT_STATUS ret = HSAKMT_STATUS_SUCCESS;

	if (amd_hsa_thunk_lock_fd <= 0)
		return HSAKMT_STATUS_UNAVAILABLE;

	if (lockf(amd_hsa_thunk_lock_fd, F_TLOCK, sizeof(lockf_tbl)))
		return HSAKMT_STATUS_ERROR;

	memset(&lockf_tbl, 0, sizeof(lockf_tbl));
	if (readn(amd_hsa_thunk_lock_fd, &lockf_tbl, sizeof(lockf_tbl)) < 0) {
		ret = HSAKMT_STATUS_ERROR;
		goto out;
	}
	/* If the magic number exists, the lock file table has been
	 * initialized by another process and is in use. Don't overwrite it.
	 */
	if (lockf_tbl.magic4cc == HSA_PERF_MAGIC4CC)
		goto out;
	/* write the perf content */
	lockf_tbl.magic4cc = HSA_PERF_MAGIC4CC;
	lockf_tbl.iommu_slots_left =
		pmc_table_get_max_concurrent(PERFCOUNTER_BLOCKID__IOMMUV2);
	if (write(amd_hsa_thunk_lock_fd, &lockf_tbl, sizeof(lockf_tbl)) < 0)
		ret = HSAKMT_STATUS_ERROR;
out:
	/* unlock the perf section */
	if (lockf(amd_hsa_thunk_lock_fd, F_ULOCK, sizeof(lockf_tbl)))
		ret = HSAKMT_STATUS_ERROR;

	return ret;
}

HSAKMT_STATUS init_counter_props(unsigned int NumNodes)
{
	counter_props = calloc(NumNodes, sizeof(struct HsaCounterProperties*));
	if (counter_props == NULL)
		return HSAKMT_STATUS_NO_MEMORY;

	counter_props_count = NumNodes;
	alloc_pmc_blocks();

	return init_lockf_perf_section();
}

void destroy_counter_props(void)
{
	unsigned int i;

	if (counter_props == NULL)
		return;

	for (i = 0; i<counter_props_count; i++)
		if (counter_props[i] != NULL) {
			free(counter_props[i]);
			counter_props[i] = NULL;
		}

	free(counter_props);
	free_pmc_blocks();
}

static int blockid2uuid(enum perf_block_id block_id, HSA_UUID *uuid)
{
	int rc = 0;

	switch (block_id) {
	case PERFCOUNTER_BLOCKID__SQ:
		*uuid = HSA_PROFILEBLOCK_AMD_SQ;
		break;
	case PERFCOUNTER_BLOCKID__IOMMUV2:
		*uuid = HSA_PROFILEBLOCK_AMD_IOMMUV2;
		break;
	default:
		/* If we reach this point, it's a bug */
		rc = -1;
		break;
	}

	return rc;
}

static HSAuint32 get_block_concurrent_limit(uint32_t node_id,
						HSAuint32 block_id)
{
	uint32_t i;

	for (i = 0; i < PERFCOUNTER_BLOCKID__MAX; i++)
		if (counter_props[node_id]->Blocks[i].Counters[0].BlockIndex ==
								block_id)
			return counter_props[node_id]->Blocks[i].NumConcurrent;

	return 0;
}

static HSAKMT_STATUS update_block_slots(enum perf_trace_action action,
					uint32_t block_id, uint32_t num_slots)
{
	struct perf_lockf_tbl lockf_tbl;
	uint32_t *slots_left;
	HSAKMT_STATUS ret = HSAKMT_STATUS_SUCCESS;

	if (amd_hsa_thunk_lock_fd <= 0)
		return HSAKMT_STATUS_UNAVAILABLE;

	if (lockf(amd_hsa_thunk_lock_fd, F_TLOCK, sizeof(lockf_tbl)) < 0)
		return HSAKMT_STATUS_ERROR;

	if (lseek(amd_hsa_thunk_lock_fd, 0, SEEK_SET)) {
		ret = HSAKMT_STATUS_ERROR;
				goto out;
	}

	if (readn(amd_hsa_thunk_lock_fd, &lockf_tbl, sizeof(lockf_tbl))
			!= sizeof(lockf_tbl)) {
		ret = HSAKMT_STATUS_ERROR;
		goto out;
	}

	if (block_id == PERFCOUNTER_BLOCKID__IOMMUV2)
		slots_left = &lockf_tbl.iommu_slots_left;
	else {
		ret = HSAKMT_STATUS_UNAVAILABLE;
		goto out;
	}

	switch (action) {
	case PERF_TRACE_ACTION__ACQUIRE:
		if (*slots_left >= num_slots)
			*slots_left -= num_slots;
		else
			ret = HSAKMT_STATUS_UNAVAILABLE;
		break;
	case PERF_TRACE_ACTION__RELEASE:
		if ((*slots_left + num_slots) <=
				pmc_table_get_max_concurrent(block_id))
			*slots_left += num_slots;
		else
			ret = HSAKMT_STATUS_ERROR;
		break;
	default:
		ret = HSAKMT_STATUS_INVALID_PARAMETER;
		break;
	}

	if (ret == HSAKMT_STATUS_SUCCESS) {
		if (write(amd_hsa_thunk_lock_fd, &lockf_tbl, sizeof(lockf_tbl)) < 0)
			ret = HSAKMT_STATUS_ERROR;
	}
out:
	/* unlock the perf section */
	if (lockf(amd_hsa_thunk_lock_fd, F_ULOCK, sizeof(lockf_tbl)))
		ret = HSAKMT_STATUS_ERROR;

	return ret;
}

static unsigned int get_perf_event_type(enum perf_block_id block_id)
{
	FILE *file = NULL;
	unsigned int type = 0;

	if (block_id == PERFCOUNTER_BLOCKID__IOMMUV2)
		file = fopen("/sys/bus/event_source/devices/amd_iommu/type",
			 "r");
	if (!file)
		return 0;

	if (fscanf(file, "%d", &type) != 1)
		type = 0;
	fclose(file);

	return type;
}

/* close_perf_event_fd - Close all FDs opened for this block.
 * 		When RT acquires the trace access, RT has no ideas about each
 *		individual FD opened for this block. We should treat the whole
 *		block as one and close all of them.
 */
static void close_perf_event_fd(struct perf_trace_block *block)
{
	uint32_t i;

	if (!block || !block->perf_event_fd)
		return;

	for (i = 0; i < block->num_counters; i++)
		if (block->perf_event_fd[i] > 0) {
			close(block->perf_event_fd[i]);
			block->perf_event_fd[i] = 0;
		}
}

/* open_perf_event_fd - Open FDs required for this block.
 * 		If one of them fails, we should close all FDs that have been
 *		opened because RT has no ideas about those FDs successfully
 *		opened and it won't send anything to close them.
 */
static HSAKMT_STATUS open_perf_event_fd(struct perf_trace_block *block)
{
	struct perf_event_attr attr;
	uint32_t i;
	HSAKMT_STATUS ret = HSAKMT_STATUS_SUCCESS;

	if (!block || !block->perf_event_fd)
		return HSAKMT_STATUS_INVALID_HANDLE;

	if (getuid()) {
		fprintf(stderr,
			"Error. Must be root to open perf_event.\n");
		return HSAKMT_STATUS_ERROR;
	}

	memset(&attr, 0, sizeof(struct perf_event_attr));
	attr.type = get_perf_event_type(block->block_id);
	if (!attr.type)
		return HSAKMT_STATUS_ERROR;

	for (i = 0; i < block->num_counters; i++) {
		attr.size = sizeof(struct perf_event_attr);
		attr.config = block->counter_id[i];
		attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED |
					PERF_FORMAT_TOTAL_TIME_RUNNING;
		attr.disabled = 1;
		attr.inherit = 1;

		/* We are profiling system wide, not per cpu, so no threads,
		 * no groups -> pid=-1 and group_fd=-1. cpu = 0
		 * flags=PERF_FLAG_FD_NO_GROUP
		 */
		block->perf_event_fd[i] = syscall(__NR_perf_event_open, &attr,
					-1, 0, -1, PERF_FLAG_FD_NO_GROUP);

		if (block->perf_event_fd[i] < 0) {
			ret = HSAKMT_STATUS_ERROR;
			close_perf_event_fd(block);
			break;
		}
	}

	return ret;
}

static HSAKMT_STATUS perf_trace_ioctl(struct perf_trace_block *block,
				uint32_t cmd)
{
	uint32_t i;

	for (i = 0; i < block->num_counters; i++) {
		if (block->perf_event_fd[i] < 0)
			return HSAKMT_STATUS_UNAVAILABLE;
		if (ioctl(block->perf_event_fd[i], cmd, NULL))
			return HSAKMT_STATUS_ERROR;
	}

	return HSAKMT_STATUS_SUCCESS;
}

static HSAKMT_STATUS query_trace(int fd, uint64_t *buf)
{
	struct perf_counts_values content;

	if (fd < 0)
		return HSAKMT_STATUS_ERROR;
	if(readn(fd, &content, sizeof(content)) != sizeof(content))
		return HSAKMT_STATUS_ERROR;

	*buf = content.val;
	return HSAKMT_STATUS_SUCCESS;
}

HSAKMT_STATUS
HSAKMTAPI
hsaKmtPmcGetCounterProperties(
	HSAuint32                   NodeId,             //IN
	HsaCounterProperties**      CounterProperties   //OUT
	)
{
	HSAKMT_STATUS rc = HSAKMT_STATUS_SUCCESS;
	uint32_t gpu_id, i, block_id;
	uint32_t counter_props_size = 0;
	uint32_t total_counters = 0;
	uint32_t total_concurrent = 0;
	struct perf_counter_block block = {0};
	uint32_t total_blocks = 0;
	uint32_t entry;

	if (counter_props == NULL)
		return HSAKMT_STATUS_NO_MEMORY;

	if (CounterProperties == NULL)
		return HSAKMT_STATUS_INVALID_PARAMETER;

	if (validate_nodeid(NodeId, &gpu_id) != 0)
		return HSAKMT_STATUS_INVALID_NODE_UNIT;

	if (counter_props[NodeId] != NULL) {
		*CounterProperties = counter_props[NodeId];
		return HSAKMT_STATUS_SUCCESS;
	}

	for (i = 0; i < PERFCOUNTER_BLOCKID__MAX; i++) {
		rc = get_block_properties(NodeId, i, &block);
		if (rc != HSAKMT_STATUS_SUCCESS)
			return rc;
		total_concurrent += block.num_of_slots;
		total_counters += block.num_of_counters;
		/* If num_of_slots=0, this block doesn't exist */
		if (block.num_of_slots)
			total_blocks++;
	}

	counter_props_size = sizeof(HsaCounterProperties) +
			sizeof(HsaCounterBlockProperties)*(total_blocks-1) +
			sizeof(HsaCounter)*(total_counters-1);

	counter_props[NodeId] = malloc(counter_props_size);
	if (counter_props[NodeId] == NULL)
		return HSAKMT_STATUS_NO_MEMORY;

	counter_props[NodeId]->NumBlocks = total_blocks;
	counter_props[NodeId]->NumConcurrent = total_concurrent;

	entry = 0;
	for (block_id = 0; block_id < PERFCOUNTER_BLOCKID__MAX; block_id++) {
		rc = get_block_properties(NodeId, block_id, &block);
		if (rc != HSAKMT_STATUS_SUCCESS) {
			free(counter_props[NodeId]);
			return rc;
		}

		if (!block.num_of_slots) /* not a valid block */
			continue;

		blockid2uuid(block_id,
			&counter_props[NodeId]->Blocks[entry].BlockId);
		counter_props[NodeId]->Blocks[entry].NumCounters =
					block.num_of_counters;
		counter_props[NodeId]->Blocks[entry].NumConcurrent =
					block.num_of_slots;

		for (i = 0; i < block.num_of_counters; i++) {
			counter_props[NodeId]->Blocks[entry].Counters[i].BlockIndex = block_id;
			counter_props[NodeId]->Blocks[entry].Counters[i].CounterId = block.counter_ids[i];
			counter_props[NodeId]->Blocks[entry].Counters[i].CounterSizeInBits = block.counter_size_in_bits;
			counter_props[NodeId]->Blocks[entry].Counters[i].CounterMask = block.counter_mask;
			counter_props[NodeId]->Blocks[entry].Counters[i].Flags.ui32.Global = 1;
			if (block_id == PERFCOUNTER_BLOCKID__IOMMUV2)
				counter_props[NodeId]->Blocks[entry].Counters[i].Type = HSA_PROFILE_TYPE_PRIVILEGED_IMMEDIATE;
			else
				counter_props[NodeId]->Blocks[entry].Counters[i].Type = HSA_PROFILE_TYPE_NONPRIV_IMMEDIATE;
		}
		entry++;
	}

	*CounterProperties = counter_props[NodeId];

	return HSAKMT_STATUS_SUCCESS;
}

/**
  Registers a set of (HW) counters to be used for tracing/profiling
*/

HSAKMT_STATUS
HSAKMTAPI
hsaKmtPmcRegisterTrace(
	HSAuint32           NodeId,             //IN
	HSAuint32           NumberOfCounters,   //IN
	HsaCounter*         Counters,           //IN
	HsaPmcTraceRoot*    TraceRoot           //OUT
	)
{
	uint32_t gpu_id, i, j;
	uint64_t min_buf_size = 0;
	struct perf_trace *trace = NULL;
	uint32_t concurrent_limit;
	const uint32_t MAX_COUNTERS = 512;
	uint64_t counter_id[PERFCOUNTER_BLOCKID__MAX][MAX_COUNTERS];
	uint32_t num_counters[PERFCOUNTER_BLOCKID__MAX] = {0};
	uint32_t block, num_blocks = 0, total_counters = 0;
	uint64_t *counter_id_ptr;
	int *fd_ptr;

	if (counter_props == NULL)
		return HSAKMT_STATUS_NO_MEMORY;

	if (Counters == NULL || TraceRoot == NULL || NumberOfCounters == 0)
		return HSAKMT_STATUS_INVALID_PARAMETER;

	if (validate_nodeid(NodeId, &gpu_id) != HSAKMT_STATUS_SUCCESS)
		return HSAKMT_STATUS_INVALID_NODE_UNIT;

	if (NumberOfCounters > MAX_COUNTERS) {
		fprintf(stderr, "Error: MAX_COUNTERS is too small for %d.\n",
			NumberOfCounters);
		return HSAKMT_STATUS_NO_MEMORY;
	}

	/* Calculating the minimum buffer size */
	for (i = 0; i < NumberOfCounters; i++) {
		if (Counters[i].BlockIndex >= PERFCOUNTER_BLOCKID__MAX)
			return HSAKMT_STATUS_INVALID_PARAMETER;
		/* Only privileged counters need to register */
		if (Counters[i].Type > HSA_PROFILE_TYPE_PRIVILEGED_STREAMING)
			continue;
		min_buf_size += Counters[i].CounterSizeInBits/BITS_PER_BYTE;
		/* j: the first blank entry in the block to record counter_id */
		j = num_counters[Counters[i].BlockIndex];
		counter_id[Counters[i].BlockIndex][j] = Counters[i].CounterId;
		num_counters[Counters[i].BlockIndex]++;
		total_counters++;
	}

	/* Verify that the number of counters per block is not larger than the
	 * number of slots.
	 */
	for (i = 0; i < PERFCOUNTER_BLOCKID__MAX; i++) {
		if (!num_counters[i])
			continue;
		concurrent_limit = get_block_concurrent_limit(NodeId, i);
		if (!concurrent_limit) {
			fprintf(stderr, "Invalid block ID: %d\n", i);
			return HSAKMT_STATUS_INVALID_PARAMETER;
		}
		if (num_counters[i] > concurrent_limit) {
			fprintf(stderr, "Counters exceed the limit.\n");
			return HSAKMT_STATUS_INVALID_PARAMETER;
		}
		num_blocks++;
	}

	if (!num_blocks)
		return HSAKMT_STATUS_INVALID_PARAMETER;

	/* Now we have sorted blocks/counters information in
	 * num_counters[block_id] and counter_id[block_id][]. Allocate trace
	 * and record the information.
	 */
	trace = (struct perf_trace *)calloc(sizeof(struct perf_trace)
			+ sizeof(struct perf_trace_block) * num_blocks
			+ sizeof(uint64_t) * total_counters
			+ sizeof(int) * total_counters,
			1);
	if (trace == NULL)
		return HSAKMT_STATUS_NO_MEMORY;

	/* Allocated area is partitioned as:
	 * +---------------------------------+ trace
	 * |    perf_trace                   |
	 * |---------------------------------| trace->blocks[0]
	 * | perf_trace_block 0              |
	 * | ....                            |
	 * | perf_trace_block N-1            | trace->blocks[N-1]
	 * |---------------------------------| <-- counter_id_ptr starts here
	 * | block 0's counter IDs(uint64_t) |
	 * | ......                          |
	 * | block N-1's counter IDs         |
	 * |---------------------------------| <-- perf_event_fd starts here
	 * | block 0's perf_event_fds(int)   |
	 * | ......                          |
	 * | block N-1's perf_event_fds      |
	 * +---------------------------------+
	 */
	block = 0;
	counter_id_ptr = (uint64_t *)((char *)
			trace + sizeof(struct perf_trace)
			+ sizeof(struct perf_trace_block) * num_blocks);
	fd_ptr = (int *)(counter_id_ptr + total_counters);
	/* Fill in each block's information to the TraceId */
	for (i = 0; i < PERFCOUNTER_BLOCKID__MAX; i++) {
		if (!num_counters[i]) /* not a block to trace */
			continue;
		/* Following perf_trace + perf_trace_block x N are those
		 * counter_id arrays. Assign the counter_id array belonging to
		 * this block.
		 */
		trace->blocks[block].counter_id = counter_id_ptr;
		/* Fill in counter IDs to the counter_id array. */
		for (j = 0; j < num_counters[i]; j++)
			trace->blocks[block].counter_id[j] = counter_id[i][j];
		trace->blocks[block].perf_event_fd = fd_ptr;
		/* how many counters to trace */
		trace->blocks[block].num_counters = num_counters[i];
		/* block index in "enum perf_block_id" */
		trace->blocks[block].block_id = i;
		block++; /* move to next */
		counter_id_ptr += num_counters[i];
		fd_ptr += num_counters[i];
	}

	trace->magic4cc = HSA_PERF_MAGIC4CC;
	trace->gpu_id = gpu_id;
	trace->state = PERF_TRACE_STATE__STOPPED;
	trace->num_blocks = num_blocks;

	TraceRoot->NumberOfPasses = 1;
	TraceRoot->TraceBufferMinSizeBytes = PAGE_ALIGN_UP(min_buf_size);
	TraceRoot->TraceId = PORT_VPTR_TO_UINT64(trace);

	return HSAKMT_STATUS_SUCCESS;
}

/**
  Unregisters a set of (HW) counters used for tracing/profiling
*/

HSAKMT_STATUS
HSAKMTAPI
hsaKmtPmcUnregisterTrace(
	HSAuint32   NodeId,     //IN
	HSATraceId  TraceId     //IN
	)
{
	uint32_t gpu_id;
	struct perf_trace *trace;

	if (TraceId == 0)
		return HSAKMT_STATUS_INVALID_PARAMETER;

	if (validate_nodeid(NodeId, &gpu_id) != 0)
		return HSAKMT_STATUS_INVALID_NODE_UNIT;

	trace = (struct perf_trace *)PORT_UINT64_TO_VPTR(TraceId);

	if (trace->magic4cc != HSA_PERF_MAGIC4CC)
		return HSAKMT_STATUS_INVALID_HANDLE;

	if (trace->gpu_id != gpu_id)
		return HSAKMT_STATUS_INVALID_NODE_UNIT;

	/* If the trace is in the running state, stop it */
	if (trace->state == PERF_TRACE_STATE__STARTED) {
		HSAKMT_STATUS status = hsaKmtPmcStopTrace(TraceId);
		if (status != HSAKMT_STATUS_SUCCESS)
			return status;
	}

	free(trace);

	return HSAKMT_STATUS_SUCCESS;
}

HSAKMT_STATUS
HSAKMTAPI
hsaKmtPmcAcquireTraceAccess(
	HSAuint32   NodeId,     //IN
	HSATraceId  TraceId     //IN
	)
{
	struct perf_trace *trace;
	HSAKMT_STATUS ret = HSAKMT_STATUS_SUCCESS;
	uint32_t gpu_id, i;
	int j;

	if (TraceId == 0)
		return HSAKMT_STATUS_INVALID_PARAMETER;

	trace = (struct perf_trace *)PORT_UINT64_TO_VPTR(TraceId);

	if (trace->magic4cc != HSA_PERF_MAGIC4CC)
		return HSAKMT_STATUS_INVALID_HANDLE;

	if (validate_nodeid(NodeId, &gpu_id) != HSAKMT_STATUS_SUCCESS)
		return HSAKMT_STATUS_INVALID_NODE_UNIT;

	for (i = 0; i < trace->num_blocks; i++) {
		ret = update_block_slots(PERF_TRACE_ACTION__ACQUIRE,
					trace->blocks[i].block_id,
					trace->blocks[i].num_counters);
		if (ret != HSAKMT_STATUS_SUCCESS)
			goto out;
		ret = open_perf_event_fd(&trace->blocks[i]);
		if (ret != HSAKMT_STATUS_SUCCESS) {
			i++; /* to release slots just reserved */
			goto out;
		}
	}

out:
	if (ret != HSAKMT_STATUS_SUCCESS) {
		for (j = i-1; j >= 0; j--) {
			update_block_slots(PERF_TRACE_ACTION__RELEASE,
					trace->blocks[j].block_id,
					trace->blocks[j].num_counters);
			close_perf_event_fd(&trace->blocks[j]);
		}
	}

	return ret;
}

HSAKMT_STATUS
HSAKMTAPI
hsaKmtPmcReleaseTraceAccess(
	HSAuint32   NodeId,     //IN
	HSATraceId  TraceId     //IN
	)
{
	struct perf_trace *trace;
	uint32_t i;

	if (TraceId == 0)
		return HSAKMT_STATUS_INVALID_PARAMETER;

	trace = (struct perf_trace *)PORT_UINT64_TO_VPTR(TraceId);

	if (trace->magic4cc != HSA_PERF_MAGIC4CC)
		return HSAKMT_STATUS_INVALID_HANDLE;

	for (i = 0; i < trace->num_blocks; i++) {
		update_block_slots(PERF_TRACE_ACTION__RELEASE,
				trace->blocks[i].block_id,
				trace->blocks[i].num_counters);
		close_perf_event_fd(&trace->blocks[i]);
	}

	return HSAKMT_STATUS_SUCCESS;
}


/**
  Starts tracing operation on a previously established set of performance counters
*/

HSAKMT_STATUS
HSAKMTAPI
hsaKmtPmcStartTrace(
	HSATraceId  TraceId,                //IN
	void*       TraceBuffer,            //IN (page aligned)
	HSAuint64   TraceBufferSizeBytes    //IN (page aligned)
	)
{
	struct perf_trace *trace =
			(struct perf_trace *)PORT_UINT64_TO_VPTR(TraceId);
	uint32_t i;
	int32_t j;
	HSAKMT_STATUS ret = HSAKMT_STATUS_SUCCESS;

	if (TraceId == 0 || TraceBuffer == NULL || TraceBufferSizeBytes == 0)
		return HSAKMT_STATUS_INVALID_PARAMETER;

	if (trace->magic4cc != HSA_PERF_MAGIC4CC)
		return HSAKMT_STATUS_INVALID_HANDLE;

	for (i = 0; i < trace->num_blocks; i++) {
		ret = perf_trace_ioctl(&trace->blocks[i],
					PERF_EVENT_IOC_ENABLE);
		if (ret != HSAKMT_STATUS_SUCCESS)
			break;
	}
	if (ret != HSAKMT_STATUS_SUCCESS) {
		/* Disable enabled blocks before returning the failure. */
		j = (int32_t)i;
		while (--j >= 0)
			perf_trace_ioctl(&trace->blocks[j],
					PERF_EVENT_IOC_DISABLE);
		return ret;
	}

	trace->state = PERF_TRACE_STATE__STARTED;
	trace->buf = TraceBuffer;
	trace->buf_size = TraceBufferSizeBytes;

	return HSAKMT_STATUS_SUCCESS;
}


/**
   Forces an update of all the counters that a previously started trace operation has registered
*/

HSAKMT_STATUS
HSAKMTAPI
hsaKmtPmcQueryTrace(
	HSATraceId    TraceId   //IN
	)
{
	struct perf_trace *trace =
			(struct perf_trace *)PORT_UINT64_TO_VPTR(TraceId);
	uint32_t i, j;
	HSAKMT_STATUS ret = HSAKMT_STATUS_SUCCESS;
	uint64_t *buf;
	uint64_t buf_filled = 0;

	if (TraceId == 0)
		return HSAKMT_STATUS_INVALID_PARAMETER;

	if (trace->magic4cc != HSA_PERF_MAGIC4CC)
		return HSAKMT_STATUS_INVALID_HANDLE;

	buf = (uint64_t *)trace->buf;
	for (i = 0; i < trace->num_blocks; i++)
		for (j = 0; j < trace->blocks[i].num_counters; j++) {
			buf_filled += sizeof(uint64_t);
			if (buf_filled > trace->buf_size)
				return HSAKMT_STATUS_NO_MEMORY;
			ret = query_trace(trace->blocks[i].perf_event_fd[j],
					buf);
			if (ret != HSAKMT_STATUS_SUCCESS)
				return ret;
			buf++;
		}

	return HSAKMT_STATUS_SUCCESS;
}


/**
  Stops tracing operation on a previously established set of performance counters
*/

HSAKMT_STATUS
HSAKMTAPI
hsaKmtPmcStopTrace(
	HSATraceId  TraceId     //IN
	)
{
	struct perf_trace *trace =
			(struct perf_trace *)PORT_UINT64_TO_VPTR(TraceId);
	uint32_t i;
	HSAKMT_STATUS ret = HSAKMT_STATUS_SUCCESS;

	if (TraceId == 0)
		return HSAKMT_STATUS_INVALID_PARAMETER;

	if (trace->magic4cc != HSA_PERF_MAGIC4CC)
		return HSAKMT_STATUS_INVALID_HANDLE;

	for (i = 0; i < trace->num_blocks; i++) {
		ret = perf_trace_ioctl(&trace->blocks[i],
					PERF_EVENT_IOC_DISABLE);
		if (ret != HSAKMT_STATUS_SUCCESS)
			return ret;
	}

	trace->state = PERF_TRACE_STATE__STOPPED;

	return ret;
}
