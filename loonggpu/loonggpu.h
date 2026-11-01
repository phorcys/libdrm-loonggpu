/* SPDX-License-Identifier: MIT */
/*
 * Public API of libdrm_loonggpu.
 *
 * The library exposes the LoongGPU kernel UAPI (loonggpu_drm.h) as a device
 * handle, GEM buffer objects, a userspace virtual address manager, command
 * submission with contexts and sync objects, and user-queue (compute)
 * helpers.
 */
#ifndef _LIBDRM_LOONGGPU_H_
#define _LIBDRM_LOONGGPU_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct drm_loonggpu_info_hw_ip;
struct drm_loonggpu_cs_chunk;
struct drm_loonggpu_cs_chunk_data;
struct drm_loonggpu_cs_chunk_dep;
struct drm_loonggpu_ctx_state3;

/** Infinite relative timeout for BO and fence waits. */
#define LOONGGPU_TIMEOUT_INFINITE 0xffffffffffffffffull

/** Pass to loonggpu_cs_query_fence_status() when timeout_ns is a deadline. */
#define LOONGGPU_QUERY_FENCE_TIMEOUT_IS_ABSOLUTE (1u << 0)

enum loonggpu_bo_handle_type {
	loonggpu_bo_handle_type_gem_flink_name = 0,
	loonggpu_bo_handle_type_kms = 1,
	loonggpu_bo_handle_type_dma_buf_fd = 2,
};

enum loonggpu_gpu_va_range {
	loonggpu_gpu_va_range_general = 0,
};

enum loonggpu_sw_info {
	loonggpu_sw_info_address32_hi = 0,
};

typedef struct loonggpu_device *loonggpu_device_handle;
typedef struct loonggpu_bo *loonggpu_bo_handle;
typedef struct loonggpu_bo_list *loonggpu_bo_list_handle;
typedef struct loonggpu_context *loonggpu_context_handle;
typedef struct loonggpu_va *loonggpu_va_handle;
typedef struct loonggpu_semaphore *loonggpu_semaphore_handle;

struct loonggpu_bo_alloc_request {
	/** Requested allocation size in bytes. */
	uint64_t alloc_size;
	/** Physical start address alignment in bytes. */
	uint64_t phys_alignment;
	/** LOONGGPU_GEM_DOMAIN_* mask of the preferred placement. */
	uint32_t preferred_heap;
	/** LOONGGPU_GEM_CREATE_* flags. */
	uint64_t flags;
};

struct loonggpu_bo_metadata {
	/** Metadata flags; driver defined. */
	uint64_t flags;
	/** Tiling information; driver defined. */
	uint64_t tiling_info;
	/** Size of the metadata in bytes. */
	uint32_t size_metadata;
	/** Opaque userspace metadata, 256 bytes in total. */
	uint32_t umd_metadata[64];
};

struct loonggpu_bo_info {
	uint64_t alloc_size;
	uint64_t phys_alignment;
	uint32_t preferred_heap;
	uint64_t alloc_flags;
	struct loonggpu_bo_metadata metadata;
	uint32_t current_heap;
	uint32_t unused;
};

/*
 * Snapshot of the device configuration the kernel reports through
 * DRM_IOCTL_LOONGGPU_INFO (LOONGGPU_INFO_DEV_INFO).  Fields the kernel does
 * not fill in are zero.
 */
struct loonggpu_gpu_info {
	/** PCI device ID. */
	uint32_t asic_id;
	/** Internal chip revision. */
	uint32_t chip_rev;
	/** External chip revision. */
	uint32_t chip_external_rev;
	/** LOONGGPU_FAMILY_* of the device. */
	uint32_t family_id;
	/** LOONGGPU_IDS_FLAGS_* of the device. */
	uint64_t ids_flags;
	/** Default engine clock, in KHz. */
	uint64_t max_engine_clk;
	/** Default memory clock, in KHz. */
	uint64_t max_memory_clk;
	uint32_t num_shader_engines;
	uint32_t num_shader_arrays_per_engine;
	uint32_t num_cu_per_sh;
	uint32_t num_tcc_blocks;
	uint32_t num_hw_gfx_contexts;
	uint32_t rb_pipes;
	uint32_t enabled_rb_pipes_mask;
	/** Frequency of the GPU counter, in KHz. */
	uint32_t gpu_counter_freq;
	uint32_t cu_active_number;
	uint32_t cu_ao_mask;
	uint32_t cu_bitmap[4][4];
	/** Number of threads per wavefront. */
	uint32_t wave_front_size;
	/** Number of shader visible vector GPRs. */
	uint32_t num_shader_visible_vgprs;
	uint32_t gs_vgt_table_depth;
	uint32_t gs_prim_buffer_depth;
	uint32_t max_gs_waves_per_vgt;
	uint32_t gc_double_offchip_lds_buf;
	uint32_t ce_ram_size;
	uint32_t vram_type;
	uint32_t vram_bit_width;
	uint32_t vce_harvest_config;
	uint32_t pci_rev_id;
	/** Page table entry fragment size in bytes. */
	uint32_t pte_fragment_size;
	/** GART page size in bytes. */
	uint32_t gart_page_size;
	/** First address of the general VA range. */
	uint64_t virtual_address_offset;
	/** Exclusive end of the general VA range. */
	uint64_t virtual_address_max;
	/** First address of the high VA range, or zero when there is none. */
	uint64_t high_va_offset;
	/** Exclusive end of the high VA range, or zero when there is none. */
	uint64_t high_va_max;
};

struct loonggpu_buffer_size_alignments {
	uint64_t size_local;
	uint64_t size_remote;
};

struct loonggpu_heap_info {
	uint64_t total_heap_size;
	uint64_t usable_heap_size;
	uint64_t heap_usage;
	uint64_t max_allocation;
};

struct loonggpu_gds_resource_info {
	uint32_t gds_gfx_partition_size;
	uint32_t compute_partition_size;
	uint32_t gds_total_size;
	uint32_t gws_per_gfx_partition;
	uint32_t gws_per_compute_partition;
	uint32_t oa_per_gfx_partition;
	uint32_t oa_per_compute_partition;
};

/** Identifies one command submission: context, engine, ring and sequence. */
struct loonggpu_cs_fence {
	loonggpu_context_handle context;
	/** LOONGGPU_HW_IP_* of the engine the submission went to. */
	uint32_t ip_type;
	uint32_t ip_instance;
	uint32_t ring;
	/** Sequence number the kernel returned for the submission. */
	uint64_t fence;
};

/**
 * Describes where a fence writeback value lives inside a BO.
 *
 * This is what loonggpu_cs_chunk_fence_info_to_data() turns into a fence
 * chunk.  loonggpu_cs_fence cannot express it because it names an already
 * submitted sequence, not the memory the value is written to.
 */
struct loonggpu_cs_fence_info {
	loonggpu_bo_handle handle;
	/** Offset inside the BO in units of uint64_t. */
	uint64_t offset;
};

struct loonggpu_cs_ib_info {
	/** GPU virtual address the IB starts at. */
	uint64_t ib_mc_address;
	/** Size of the IB in bytes. */
	uint32_t size;
	/** LOONGGPU_IB_FLAG_* */
	uint32_t flags;
};

struct loonggpu_cs_request {
	/** Reserved; must be zero. */
	uint64_t flags;
	loonggpu_context_handle context;
	uint32_t ip_type;
	uint32_t ip_instance;
	uint32_t ring;
	loonggpu_bo_list_handle resources;
	uint32_t number_of_ibs;
	struct loonggpu_cs_ib_info *ibs;
	/** Returned sequence number of this submission. */
	uint64_t seq_no;
	/** Optional BO that receives the fence value when the job completes. */
	loonggpu_bo_handle fence_memory;
	/** Byte offset in fence_memory; the kernel requires a one page BO. */
	uint64_t fence_offset;
};

struct loonggpu_bo_import_result {
	loonggpu_bo_handle buf_handle;
	uint64_t alloc_size;
};

int loonggpu_device_initialize(int fd, uint32_t *major_version,
			      uint32_t *minor_version,
			      loonggpu_device_handle *device_handle);
int loonggpu_device_deinitialize(loonggpu_device_handle device);
/** Take another reference to a device that is already open. */
int loonggpu_device_reference(loonggpu_device_handle device,
			      loonggpu_device_handle *out);
/** Borrowed fd; valid while a device or child-object reference is held. */
int loonggpu_device_get_fd(loonggpu_device_handle device);
const char *loonggpu_get_marketing_name(loonggpu_device_handle device);

int loonggpu_query_info(loonggpu_device_handle dev, unsigned info_id,
			unsigned size, void *value);
int loonggpu_query_crtc_from_id(loonggpu_device_handle dev, unsigned id, uint32_t *result);
int loonggpu_read_mm_registers(loonggpu_device_handle dev, unsigned dword_offset,
			       unsigned count, uint32_t instance, uint32_t flags, uint32_t *values);
int loonggpu_query_hw_ip_count(loonggpu_device_handle dev, unsigned type, uint32_t *count);
int loonggpu_query_hw_ip_info(loonggpu_device_handle dev, unsigned type, unsigned ip_instance,
			      struct drm_loonggpu_info_hw_ip *info);
/*
 * Report whether the driver exposes at least one ring for this HW-IP type.
 *
 * This is queue discovery only: it says nothing about the command stream or
 * shader support behind the queue.  An engine type the device does not
 * implement is reported as unavailable rather than as an error.
 */
int loonggpu_query_hw_ip_available(loonggpu_device_handle dev, unsigned type, bool *available);
int loonggpu_query_firmware_version(loonggpu_device_handle dev, unsigned fw_type,
				    unsigned ip_instance, unsigned index,
				    uint32_t *version, uint32_t *feature);
int loonggpu_query_gpu_info(loonggpu_device_handle dev, struct loonggpu_gpu_info *info);
int loonggpu_query_heap_info(loonggpu_device_handle dev, uint32_t heap, uint32_t flags,
			     struct loonggpu_heap_info *info);
/** GDS, GWS and OA partition sizes; -ENODATA when the device has no GDS. */
int loonggpu_query_gds_info(loonggpu_device_handle dev, struct loonggpu_gds_resource_info *gds_info);
int loonggpu_query_sensor_info(loonggpu_device_handle dev, unsigned sensor_type, unsigned size, void *value);
int loonggpu_query_sw_info(loonggpu_device_handle dev, enum loonggpu_sw_info info, void *value);
int loonggpu_query_buffer_size_alignment(loonggpu_device_handle dev,
					 struct loonggpu_buffer_size_alignments *alignment);

int loonggpu_bo_alloc(loonggpu_device_handle dev, struct loonggpu_bo_alloc_request *request,
		      loonggpu_bo_handle *buf_handle);
int loonggpu_bo_set_metadata(loonggpu_bo_handle buf_handle, struct loonggpu_bo_metadata *info);
int loonggpu_bo_query_info(loonggpu_bo_handle buf_handle, struct loonggpu_bo_info *info);
int loonggpu_bo_export(loonggpu_bo_handle buf_handle, enum loonggpu_bo_handle_type type, uint32_t *shared_handle);
int loonggpu_bo_import(loonggpu_device_handle dev, enum loonggpu_bo_handle_type type,
		       uint32_t shared_handle, struct loonggpu_bo_import_result *output);
int loonggpu_create_bo_from_user_mem(loonggpu_device_handle dev, void *cpu, uint64_t size,
				     loonggpu_bo_handle *buf_handle);
int loonggpu_bo_free(loonggpu_bo_handle buf_handle);
int loonggpu_bo_cpu_map(loonggpu_bo_handle buf_handle, void **cpu);
/** GEM handle of a BO, for DRM ioctls that take one. */
uint32_t loonggpu_bo_gem_handle(loonggpu_bo_handle bo);
/** Whether the BO ever left this library through export or import. */
bool loonggpu_bo_is_shared(loonggpu_bo_handle bo);
int loonggpu_bo_cpu_unmap(loonggpu_bo_handle buf_handle);
/** timeout_ns is a relative duration; LOONGGPU_TIMEOUT_INFINITE never expires. */
int loonggpu_bo_wait_for_idle(loonggpu_bo_handle buf_handle, uint64_t timeout_ns, bool *busy);

int loonggpu_bo_list_create(loonggpu_device_handle dev, uint32_t number_of_resources,
			    loonggpu_bo_handle *resources, uint8_t *resource_prios,
			    loonggpu_bo_list_handle *result);
/** On failure the list remains valid and may be retried. */
int loonggpu_bo_list_destroy(loonggpu_bo_list_handle list);
int loonggpu_bo_list_update(loonggpu_bo_list_handle list, uint32_t number_of_resources,
			    loonggpu_bo_handle *resources, uint8_t *resource_prios);

int loonggpu_va_range_query(loonggpu_device_handle dev, enum loonggpu_gpu_va_range type,
			    uint64_t *start, uint64_t *end);
int loonggpu_va_range_alloc(loonggpu_device_handle dev, enum loonggpu_gpu_va_range va_range_type,
			    uint64_t size, uint64_t va_base_alignment, uint64_t va_base_required,
			    uint64_t *va_base_allocated, loonggpu_va_handle *va_range_handle,
			    uint64_t flags);
int loonggpu_va_range_free(loonggpu_va_handle va_range_handle);
int loonggpu_bo_va_op(loonggpu_bo_handle bo, uint64_t offset, uint64_t size, uint64_t addr,
		      uint64_t flags, uint32_t ops);
int loonggpu_bo_va_op_raw(loonggpu_device_handle dev, loonggpu_bo_handle bo, uint64_t offset,
			  uint64_t size, uint64_t addr, uint64_t flags, uint32_t ops);

/*
 * User-mode compute queues.
 *
 * On LoongGPU compute is driven by a queue that lives in userspace memory: the
 * client writes a command stream into a ring and rings a doorbell, and the
 * kernel only publishes the new write pointer.  These wrap
 * DRM_IOCTL_LOONGGPU_USERQ.  @ip_type is a LOONGGPU_HW_IP_* value; @queue_va
 * and @queue_size describe a 64-byte-aligned ring and a power-of-two size
 * between LOONGGPU_USERQ_MIN_SIZE and LOONGGPU_USERQ_MAX_SIZE.  The ring and
 * its preceding LOONGGPU_USERQ_INDEX_SIZE bytes must remain mapped into the
 * device VM for the queue's lifetime.  @wptr is a 64-byte-aligned byte offset
 * within the ring.  Only LOONGGPU_HW_IP_COMPUTE is supported.
 */
int loonggpu_userq_create(loonggpu_device_handle dev, uint32_t ip_type,
			  uint64_t queue_va, uint64_t queue_size,
			  uint32_t *queue_id);
int loonggpu_userq_free(loonggpu_device_handle dev, uint32_t queue_id);
int loonggpu_userq_doorbell(loonggpu_device_handle dev, uint32_t queue_id,
			    uint64_t wptr);

int loonggpu_vm_reserve_vmid(loonggpu_device_handle dev, uint32_t flags);
int loonggpu_vm_unreserve_vmid(loonggpu_device_handle dev, uint32_t flags);

int loonggpu_cs_ctx_create2(loonggpu_device_handle dev, uint32_t priority, loonggpu_context_handle *context);
int loonggpu_cs_ctx_create(loonggpu_device_handle dev, loonggpu_context_handle *context);
/**
 * Drop the caller's context reference.  Semaphores retain the kernel context
 * until their last reference is released.  The caller's handle is consumed
 * even if the final kernel release reports an error.
 */
int loonggpu_cs_ctx_free(loonggpu_context_handle context);
int loonggpu_cs_query_reset_state(loonggpu_context_handle context, uint32_t *state, uint32_t *hangs);
/**
 * Query the sticky LOONGGPU_CTX_QUERY2_FLAGS_* of a context.  Unlike
 * loonggpu_cs_query_reset_state(), querying does not acknowledge a reset.
 */
int loonggpu_cs_query_reset_state2(loonggpu_context_handle context, uint32_t *flags, uint32_t *hangs);
/** Typed health query, including the reset and admission epochs. */
int loonggpu_cs_query_state3(loonggpu_context_handle context,
			      struct drm_loonggpu_ctx_state3 *state);
int loonggpu_cs_submit(loonggpu_device_handle dev, uint64_t flags,
		       struct loonggpu_cs_request *ibs_request, uint32_t number_of_requests);
int loonggpu_cs_submit_raw(loonggpu_device_handle dev, loonggpu_context_handle context,
			   loonggpu_bo_list_handle bo_list_handle, int num_chunks,
			   struct drm_loonggpu_cs_chunk *chunks, uint64_t *seq_no);
int loonggpu_cs_submit_raw2(loonggpu_device_handle dev, loonggpu_context_handle context,
			    uint32_t bo_list_handle, int num_chunks,
			    struct drm_loonggpu_cs_chunk *chunks, uint64_t *seq_no);
void loonggpu_cs_chunk_fence_info_to_data(struct loonggpu_cs_fence_info *fence_info,
					  struct drm_loonggpu_cs_chunk_data *data);
void loonggpu_cs_chunk_fence_to_dep(struct loonggpu_cs_fence *fence,
				    struct drm_loonggpu_cs_chunk_dep *dep);
int loonggpu_cs_query_fence_status(struct loonggpu_cs_fence *fence, uint64_t timeout_ns,
				   uint64_t flags, uint32_t *expired);

/*
 * Snapshot of a submission's retirement and execution status.  flags contains
 * LOONGGPU_FENCE_* from loonggpu_drm.h; error is the execution error, if any.
 * RETIRED permits reuse with respect to this submission's GPU accesses only.
 * CPU users, other submissions and shared-buffer synchronization remain the
 * caller's responsibility.  An execution error does not prove retirement.
 *
 * confirmed is the kernel's completion watermark for this context and ring;
 * it does not override ERROR or an absent RETIRED flag.  Epoch fields identify
 * the device state associated with the answer.  The query does not wait.
 * -ENOTTY means the kernel does not implement this query.
 */
struct loonggpu_cs_fence_result {
	uint32_t flags;
	int32_t error;
	uint64_t seq_epoch;
	uint64_t current_epoch;
	uint64_t confirmed;
};

int loonggpu_cs_query_fence_result(struct loonggpu_cs_fence *fence,
				   struct loonggpu_cs_fence_result *result);
int loonggpu_cs_wait_fences(struct loonggpu_cs_fence *fences, uint32_t fence_count, bool wait_all,
			    uint64_t timeout_ns, uint32_t *status, uint32_t *first);

/**
 * Semaphores record the last loonggpu_cs_submit() sequence; raw submissions
 * do not update them.  Waiting blocks the calling CPU thread.  Access to one
 * semaphore must be externally serialized, including signal and destruction.
 */
int loonggpu_cs_create_semaphore(loonggpu_semaphore_handle *sem);
int loonggpu_cs_signal_semaphore(loonggpu_context_handle ctx, uint32_t ip_type, uint32_t ip_instance,
				 uint32_t ring, loonggpu_semaphore_handle sem);
int loonggpu_cs_wait_semaphore(loonggpu_context_handle ctx, uint32_t ip_type, uint32_t ip_instance,
			       uint32_t ring, loonggpu_semaphore_handle sem);
int loonggpu_cs_destroy_semaphore(loonggpu_semaphore_handle sem);

int loonggpu_cs_create_syncobj2(loonggpu_device_handle dev, uint32_t flags, uint32_t *handle);
int loonggpu_cs_create_syncobj(loonggpu_device_handle dev, uint32_t *handle);
int loonggpu_cs_destroy_syncobj(loonggpu_device_handle dev, uint32_t handle);
int loonggpu_cs_syncobj_reset(loonggpu_device_handle dev, const uint32_t *syncobjs, uint32_t syncobj_count);
int loonggpu_cs_syncobj_signal(loonggpu_device_handle dev, const uint32_t *syncobjs, uint32_t syncobj_count);
int loonggpu_cs_syncobj_timeline_signal(loonggpu_device_handle dev, const uint32_t *syncobjs,
					uint64_t *points, uint32_t syncobj_count);
int loonggpu_cs_syncobj_wait(loonggpu_device_handle dev, uint32_t *handles, unsigned num_handles,
			     int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
int loonggpu_cs_syncobj_timeline_wait(loonggpu_device_handle dev, uint32_t *handles, uint64_t *points,
				      unsigned num_handles, int64_t timeout_nsec, unsigned flags,
				      uint32_t *first_signaled);
int loonggpu_cs_syncobj_query(loonggpu_device_handle dev, uint32_t *handles, uint64_t *points, unsigned num_handles);
int loonggpu_cs_syncobj_query2(loonggpu_device_handle dev, uint32_t *handles, uint64_t *points,
			       unsigned num_handles, uint32_t flags);
int loonggpu_cs_export_syncobj(loonggpu_device_handle dev, uint32_t handle, int *shared_fd);
int loonggpu_cs_import_syncobj(loonggpu_device_handle dev, int shared_fd, uint32_t *handle);
int loonggpu_cs_syncobj_export_sync_file(loonggpu_device_handle dev, uint32_t syncobj, int *sync_file_fd);
int loonggpu_cs_syncobj_export_sync_file2(loonggpu_device_handle dev, uint32_t syncobj, uint64_t point,
					  uint32_t flags, int *sync_file_fd);
int loonggpu_cs_syncobj_import_sync_file(loonggpu_device_handle dev, uint32_t syncobj, int sync_file_fd);
int loonggpu_cs_syncobj_import_sync_file2(loonggpu_device_handle dev, uint32_t syncobj, uint64_t point, int sync_file_fd);
int loonggpu_cs_syncobj_transfer(loonggpu_device_handle dev, uint32_t dst_handle, uint64_t dst_point,
				 uint32_t src_handle, uint64_t src_point, uint32_t flags);
int loonggpu_cs_fence_to_handle(loonggpu_device_handle dev, struct loonggpu_cs_fence *fence,
				uint32_t what, uint32_t *out_handle);
int loonggpu_hw_sema_get(loonggpu_device_handle dev, uint32_t *handle);
int loonggpu_hw_sema_put(loonggpu_device_handle dev, uint32_t handle);

#ifdef __cplusplus
}
#endif
#endif
