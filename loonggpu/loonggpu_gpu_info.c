/* SPDX-License-Identifier: MIT */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <xf86drm.h>

#include "loonggpu_internal.h"

int loonggpu_query_info(loonggpu_device_handle dev, unsigned info_id,
			unsigned size, void *value)
{
	struct drm_loonggpu_info info = {0};

	if (!dev || !value)
		return -EINVAL;

	info.return_pointer = (uintptr_t)value;
	info.return_size = size;
	info.query = info_id;
	return drmCommandWriteRead(dev->fd, DRM_LOONGGPU_INFO, &info, sizeof(info));
}

int loonggpu_query_crtc_from_id(loonggpu_device_handle dev, unsigned id, uint32_t *result)
{
	struct drm_loonggpu_info info = {0};

	if (!dev || !result)
		return -EINVAL;
	info.return_pointer = (uintptr_t)result;
	info.return_size = sizeof(*result);
	info.query = LOONGGPU_INFO_CRTC_FROM_ID;
	info.mode_crtc.id = id;
	return drmCommandWriteRead(dev->fd, DRM_LOONGGPU_INFO, &info, sizeof(info));
}

int loonggpu_read_mm_registers(loonggpu_device_handle dev, unsigned dword_offset,
			       unsigned count, uint32_t instance, uint32_t flags,
			       uint32_t *values)
{
	struct drm_loonggpu_info info = {0};

	if (!dev || !values || !count || count > UINT32_MAX / sizeof(uint32_t))
		return -EINVAL;
	info.return_pointer = (uintptr_t)values;
	info.return_size = count * sizeof(uint32_t);
	info.query = LOONGGPU_INFO_READ_MMR_REG;
	info.read_mmr_reg.dword_offset = dword_offset;
	info.read_mmr_reg.count = count;
	info.read_mmr_reg.instance = instance;
	info.read_mmr_reg.flags = flags;
	return drmCommandWriteRead(dev->fd, DRM_LOONGGPU_INFO, &info, sizeof(info));
}

int loonggpu_query_hw_ip_count(loonggpu_device_handle dev, unsigned type, uint32_t *count)
{
	struct drm_loonggpu_info info = {0};

	if (!dev || !count)
		return -EINVAL;
	info.return_pointer = (uintptr_t)count;
	info.return_size = sizeof(*count);
	info.query = LOONGGPU_INFO_HW_IP_COUNT;
	info.query_hw_ip.type = type;
	return drmCommandWriteRead(dev->fd, DRM_LOONGGPU_INFO, &info, sizeof(info));
}

int loonggpu_query_hw_ip_info(loonggpu_device_handle dev, unsigned type,
			      unsigned ip_instance,
			      struct drm_loonggpu_info_hw_ip *ip_info)
{
	struct drm_loonggpu_info info = {0};

	if (!dev || !ip_info)
		return -EINVAL;
	info.return_pointer = (uintptr_t)ip_info;
	info.return_size = sizeof(*ip_info);
	info.query = LOONGGPU_INFO_HW_IP_INFO;
	info.query_hw_ip.type = type;
	info.query_hw_ip.ip_instance = ip_instance;
	return drmCommandWriteRead(dev->fd, DRM_LOONGGPU_INFO, &info, sizeof(info));
}

int loonggpu_query_hw_ip_available(loonggpu_device_handle dev, unsigned type,
				   bool *available)
{
	struct drm_loonggpu_info_hw_ip info = {0};
	uint32_t count = 0;
	int r;

	if (available)
		*available = false;
	if (!dev || !available || type >= LOONGGPU_HW_IP_NUM)
		return -EINVAL;

	r = loonggpu_query_hw_ip_count(dev, type, &count);
	/* LOONGGPU_HW_IP_NUM covers more slots than any device implements, and
	 * the kernel answers -EINVAL for the missing ones. */
	if (r == -EINVAL)
		return 0;
	if (r || !count)
		return r;

	r = loonggpu_query_hw_ip_info(dev, type, 0, &info);
	if (r)
		return r;
	*available = info.available_rings != 0;
	return 0;
}

int loonggpu_query_firmware_version(loonggpu_device_handle dev, unsigned fw_type,
				    unsigned ip_instance, unsigned index,
				    uint32_t *version, uint32_t *feature)
{
	struct drm_loonggpu_info info = {0};
	struct drm_loonggpu_info_firmware fw = {0};
	int r;

	if (!dev || !version || !feature)
		return -EINVAL;
	info.return_pointer = (uintptr_t)&fw;
	info.return_size = sizeof(fw);
	info.query = LOONGGPU_INFO_FW_VERSION;
	info.query_fw.fw_type = fw_type;
	info.query_fw.ip_instance = ip_instance;
	info.query_fw.index = index;
	r = drmCommandWriteRead(dev->fd, DRM_LOONGGPU_INFO, &info, sizeof(info));
	if (!r) {
		*version = fw.ver;
		*feature = fw.feature;
	}
	return r;
}

/*
 * Fetch the device description once and derive the public view of it.  The
 * kernel reports a VA window that the userspace manager takes as its range, so
 * an empty window is a device this library cannot drive.
 */
int loonggpu_query_gpu_info_init(loonggpu_device_handle dev)
{
	struct loonggpu_gpu_info *info = &dev->info;
	int r;

	memset(&dev->dev_info, 0, sizeof(dev->dev_info));
	r = loonggpu_query_info(dev, LOONGGPU_INFO_DEV_INFO, sizeof(dev->dev_info),
				&dev->dev_info);
	if (r)
		return r;
	if (dev->dev_info.virtual_address_max <= dev->dev_info.virtual_address_offset)
		return -EINVAL;

	if (dev->dev_info.virtual_address_alignment &&
	    (dev->dev_info.virtual_address_alignment &
	     (dev->dev_info.virtual_address_alignment - 1)))
		return -EINVAL;

	memset(info, 0, sizeof(*info));
	info->asic_id = dev->dev_info.device_id;
	info->chip_rev = dev->dev_info.chip_rev;
	info->chip_external_rev = dev->dev_info.external_rev;
	info->family_id = dev->dev_info.family;
	info->ids_flags = dev->dev_info.ids_flags;
	/* The kernel reports all clocks in KHz. */
	info->max_engine_clk = dev->dev_info.max_engine_clock;
	info->max_memory_clk = dev->dev_info.max_memory_clock;
	info->gpu_counter_freq = dev->dev_info.gpu_counter_freq;
	info->num_shader_engines = dev->dev_info.num_shader_engines;
	info->num_shader_arrays_per_engine = dev->dev_info.num_shader_arrays_per_engine;
	info->num_cu_per_sh = dev->dev_info.num_cu_per_sh;
	info->num_tcc_blocks = dev->dev_info.num_tcc_blocks;
	info->num_hw_gfx_contexts = dev->dev_info.num_hw_gfx_contexts;
	info->rb_pipes = dev->dev_info.num_rb_pipes;
	info->enabled_rb_pipes_mask = dev->dev_info.enabled_rb_pipes_mask;
	info->cu_active_number = dev->dev_info.cu_active_number;
	info->cu_ao_mask = dev->dev_info.cu_ao_mask;
	memcpy(info->cu_bitmap, dev->dev_info.cu_bitmap, sizeof(info->cu_bitmap));
	info->wave_front_size = dev->dev_info.wave_front_size;
	info->num_shader_visible_vgprs = dev->dev_info.num_shader_visible_vgprs;
	info->gs_vgt_table_depth = dev->dev_info.gs_vgt_table_depth;
	info->gs_prim_buffer_depth = dev->dev_info.gs_prim_buffer_depth;
	info->max_gs_waves_per_vgt = dev->dev_info.max_gs_waves_per_vgt;
	info->gc_double_offchip_lds_buf = dev->dev_info.gc_double_offchip_lds_buf;
	info->ce_ram_size = dev->dev_info.ce_ram_size;
	info->vram_type = dev->dev_info.vram_type;
	info->vram_bit_width = dev->dev_info.vram_bit_width;
	info->vce_harvest_config = dev->dev_info.vce_harvest_config;
	info->pci_rev_id = dev->dev_info.pci_rev;
	info->pte_fragment_size = dev->dev_info.pte_fragment_size;
	info->gart_page_size = dev->dev_info.gart_page_size;
	info->virtual_address_offset = dev->dev_info.virtual_address_offset;
	info->virtual_address_max = dev->dev_info.virtual_address_max;
	info->high_va_offset = dev->dev_info.high_va_offset;
	info->high_va_max = dev->dev_info.high_va_max;
	return 0;
}

int loonggpu_query_gpu_info(loonggpu_device_handle dev, struct loonggpu_gpu_info *info)
{
	if (!dev || !info)
		return -EINVAL;
	*info = dev->info;
	return 0;
}

int loonggpu_query_heap_info(loonggpu_device_handle dev, uint32_t heap,
			     uint32_t flags, struct loonggpu_heap_info *info)
{
	struct drm_loonggpu_memory_info mem = {0};
	struct drm_loonggpu_heap_info *src;
	int r;

	if (!dev || !info ||
	    (heap != LOONGGPU_GEM_DOMAIN_VRAM && heap != LOONGGPU_GEM_DOMAIN_GTT) ||
	    (flags & ~LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED))
		return -EINVAL;
	r = loonggpu_query_info(dev, LOONGGPU_INFO_MEMORY, sizeof(mem), &mem);
	if (r)
		return r;

	if (heap == LOONGGPU_GEM_DOMAIN_GTT)
		src = &mem.gtt;
	else if (flags & LOONGGPU_GEM_CREATE_CPU_ACCESS_REQUIRED)
		src = &mem.cpu_accessible_vram;
	else
		src = &mem.vram;
	info->total_heap_size = src->total_heap_size;
	info->usable_heap_size = src->usable_heap_size;
	info->heap_usage = src->heap_usage;
	info->max_allocation = src->max_allocation;
	return 0;
}

int loonggpu_query_gds_info(loonggpu_device_handle dev,
			    struct loonggpu_gds_resource_info *gds_info)
{
	struct drm_loonggpu_info_gds gds = {0};
	int r;

	if (!dev || !gds_info)
		return -EINVAL;
	memset(gds_info, 0, sizeof(*gds_info));
	r = loonggpu_query_info(dev, LOONGGPU_INFO_GDS_CONFIG, sizeof(gds), &gds);
	if (r)
		return r;
	gds_info->gds_gfx_partition_size = gds.gds_gfx_partition_size;
	gds_info->compute_partition_size = gds.compute_partition_size;
	gds_info->gds_total_size = gds.gds_total_size;
	gds_info->gws_per_gfx_partition = gds.gws_per_gfx_partition;
	gds_info->gws_per_compute_partition = gds.gws_per_compute_partition;
	gds_info->oa_per_gfx_partition = gds.oa_per_gfx_partition;
	gds_info->oa_per_compute_partition = gds.oa_per_compute_partition;
	return 0;
}

int loonggpu_query_sensor_info(loonggpu_device_handle dev, unsigned sensor_type,
			       unsigned size, void *value)
{
	struct drm_loonggpu_info info = {0};

	if (!dev || !value)
		return -EINVAL;
	info.return_pointer = (uintptr_t)value;
	info.return_size = size;
	info.query = LOONGGPU_INFO_SENSOR;
	info.sensor_info.type = sensor_type;
	return drmCommandWriteRead(dev->fd, DRM_LOONGGPU_INFO, &info, sizeof(info));
}

int loonggpu_query_sw_info(loonggpu_device_handle dev, enum loonggpu_sw_info info, void *value)
{
	uint32_t *v = value;

	if (!dev || !value)
		return -EINVAL;
	if (info != loonggpu_sw_info_address32_hi)
		return -EINVAL;
	if (dev->dev_info.virtual_address_max <= 1)
		return -EINVAL;
	/* Upper 32 bits of the highest usable address, which is how a winsys
	 * derives the reach of the general VA range. */
	*v = (uint32_t)((dev->dev_info.virtual_address_max - 1) >> 32);
	return 0;
}

int loonggpu_query_buffer_size_alignment(loonggpu_device_handle dev,
					 struct loonggpu_buffer_size_alignments *alignment)
{
	if (!dev || !alignment)
		return -EINVAL;
	/*
	 * The UAPI only describes page granularity, not the size each heap
	 * prefers, so report the VA and GART page sizes respectively.
	 */
	alignment->size_local = dev->dev_info.virtual_address_alignment ?
		dev->dev_info.virtual_address_alignment : 4096;
	alignment->size_remote = dev->dev_info.gart_page_size ?
		dev->dev_info.gart_page_size : alignment->size_local;
	return 0;
}
