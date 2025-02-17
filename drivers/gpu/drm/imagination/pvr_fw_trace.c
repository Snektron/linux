// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#include "pvr_device.h"
#include "pvr_gem.h"
#include "pvr_rogue_fwif.h"
#include "pvr_rogue_fwif_sf.h"
#include "pvr_fw_trace.h"

#include <drm/drm_file.h>

#include <linux/build_bug.h>
#include <linux/dcache.h>
#include <linux/sysfs.h>
#include <linux/types.h>

/*
 * The tuple pairs that will be generated using XMacros will be stored here.
 * This macro definition must match the definition of rogue_fw_log_sfids in
 * pvr_rogue_fwif_sf.h.
 */
static const struct rogue_km_stid_fmt stid_fmts[] = {
#define X(a, b, c, d, e) { ROGUE_FW_LOG_CREATESFID(a, b, e), d },
	ROGUE_FW_LOG_SFIDLIST
#undef X
};

int pvr_fw_trace_init(struct pvr_device *pvr_dev)
{
	struct pvr_fw_trace *fw_trace = &pvr_dev->fw_dev.fw_trace;
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	u32 thread_nr;
	int err;

	fw_trace->tracebuf_ctrl =
		pvr_fw_object_create_and_map(pvr_dev,
					     sizeof(*fw_trace->tracebuf_ctrl),
					     PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
					     DRM_PVR_BO_CREATE_ZEROED,
					     &fw_trace->tracebuf_ctrl_obj);
	if (IS_ERR(fw_trace->tracebuf_ctrl)) {
		drm_err(drm_dev, "Unable to allocate trace buffer control structure\n");
		err = PTR_ERR(fw_trace->tracebuf_ctrl);
		goto err_out;
	}

	BUILD_BUG_ON(ARRAY_SIZE(fw_trace->tracebuf_ctrl->tracebuf) !=
		     ARRAY_SIZE(fw_trace->buffers));

	for (thread_nr = 0; thread_nr < ARRAY_SIZE(fw_trace->buffers); thread_nr++) {
		struct rogue_fwif_tracebuf_space *tracebuf_space =
			&fw_trace->tracebuf_ctrl->tracebuf[thread_nr];
		struct pvr_fw_trace_buffer *trace_buffer = &fw_trace->buffers[thread_nr];

		trace_buffer->buf =
			pvr_fw_object_create_and_map(pvr_dev,
						     ROGUE_FW_TRACE_BUF_DEFAULT_SIZE_IN_DWORDS *
						     sizeof(*trace_buffer->buf),
						     PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
						     DRM_PVR_BO_CREATE_ZEROED,
						     &trace_buffer->buf_obj);
		if (IS_ERR(trace_buffer->buf)) {
			drm_err(drm_dev, "Unable to allocate trace buffer\n");
			err = PTR_ERR(trace_buffer->buf);
			trace_buffer->buf = NULL;
			goto err_free_buf;
		}
		trace_buffer->tracebuf_space = tracebuf_space;

		pvr_fw_object_get_fw_addr(trace_buffer->buf_obj,
					  &tracebuf_space->trace_buffer_fw_addr);

		tracebuf_space->trace_buffer = trace_buffer->buf;
		tracebuf_space->trace_pointer = 0;
	}

	fw_trace->tracebuf_ctrl->tracebuf_size_in_dwords =
		ROGUE_FW_TRACE_BUF_DEFAULT_SIZE_IN_DWORDS;
	fw_trace->tracebuf_ctrl->tracebuf_flags = 0;

	fw_trace->group_mask = pvr_dev->params.fw_trace_mask;
	if (fw_trace->group_mask)
		fw_trace->tracebuf_ctrl->log_type = fw_trace->group_mask |
						    ROGUE_FWIF_LOG_TYPE_TRACE;
	else
		fw_trace->tracebuf_ctrl->log_type = ROGUE_FWIF_LOG_TYPE_NONE;

	return 0;

err_free_buf:
	for (thread_nr = 0; thread_nr < ARRAY_SIZE(fw_trace->buffers); thread_nr++) {
		struct pvr_fw_trace_buffer *trace_buffer = &fw_trace->buffers[thread_nr];

		if (trace_buffer->buf)
			pvr_fw_object_unmap_and_destroy(trace_buffer->buf_obj);
	}

	pvr_fw_object_unmap_and_destroy(fw_trace->tracebuf_ctrl_obj);

err_out:
	return err;
}

void pvr_fw_trace_fini(struct pvr_device *pvr_dev)
{
	struct pvr_fw_trace *fw_trace = &pvr_dev->fw_dev.fw_trace;
	u32 thread_nr;

	for (thread_nr = 0; thread_nr < ARRAY_SIZE(fw_trace->buffers); thread_nr++) {
		struct pvr_fw_trace_buffer *trace_buffer = &fw_trace->buffers[thread_nr];

		pvr_fw_object_unmap_and_destroy(trace_buffer->buf_obj);
	}
	pvr_fw_object_unmap_and_destroy(fw_trace->tracebuf_ctrl_obj);
}

/**
 * update_logtype() - Send KCCB command to trigger FW to update logtype
 * @pvr_dev: Target PowerVR device
 * @group_mask: New log group mask.
 *
 * Returns:
 *  * 0 on success, or
 *  * Any error returned by pvr_kccb_send_cmd().
 */
static int
update_logtype(struct pvr_device *pvr_dev, u32 group_mask)
{
	struct pvr_fw_trace *fw_trace = &pvr_dev->fw_dev.fw_trace;
	struct rogue_fwif_kccb_cmd cmd;

	if (group_mask)
		fw_trace->tracebuf_ctrl->log_type = ROGUE_FWIF_LOG_TYPE_TRACE | group_mask;
	else
		fw_trace->tracebuf_ctrl->log_type = ROGUE_FWIF_LOG_TYPE_NONE;

	cmd.cmd_type = ROGUE_FWIF_KCCB_CMD_LOGTYPE_UPDATE;
	cmd.kccb_flags = 0;

	return pvr_kccb_send_cmd(pvr_dev, &cmd, NULL);
}

#if defined(CONFIG_DEBUG_FS)

static int fw_trace_group_mask_show(struct seq_file *m, void *data)
{
	struct pvr_device *pvr_dev = m->private;

	seq_printf(m, "%08x\n", pvr_dev->fw_dev.fw_trace.group_mask);

	return 0;
}

static int fw_trace_group_mask_open(struct inode *inode, struct file *file)
{
	return single_open(file, fw_trace_group_mask_show, inode->i_private);
}

static ssize_t fw_trace_group_mask_write(struct file *file, const char __user *ubuf, size_t len,
					 loff_t *offp)
{
	struct seq_file *m = file->private_data;
	struct pvr_device *pvr_dev = m->private;
	u32 new_group_mask;
	int err;

	err = kstrtouint_from_user(ubuf, len, 0, &new_group_mask);
	if (err)
		goto err_out;

	err = update_logtype(pvr_dev, new_group_mask);
	if (err)
		goto err_out;

	pvr_dev->fw_dev.fw_trace.group_mask = new_group_mask;

err_out:
	return err ? err : (ssize_t)len;
}

static const struct file_operations pvr_fw_trace_group_mask_fops = {
	.owner = THIS_MODULE,
	.open = fw_trace_group_mask_open,
	.read = seq_read,
	.write = fw_trace_group_mask_write,
	.llseek = default_llseek,
	.release = single_release,
};

struct pvr_fw_trace_seq_data {
	/** @buffer: Pointer to copy of trace data. */
	u32 *buffer;

	/** @start_offset: Starting offset in trace data, as reported by FW. */
	u32 start_offset;

	/** @idx: Current index into trace data. */
	u32 idx;

	/** @assert_buf: Trace assert buffer, as reported by FW. */
	struct rogue_fwif_file_info_buf assert_buf;
};

static u32 find_sfid(u32 id)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(stid_fmts); i++) {
		if (stid_fmts[i].id == id)
			return i;
	}

	return ROGUE_FW_SF_LAST;
}

static u32 read_fw_trace(struct pvr_fw_trace_seq_data *trace_seq_data, u32 offset)
{
	u32 idx;

	idx = trace_seq_data->idx + offset;
	if (idx >= ROGUE_FW_TRACE_BUF_DEFAULT_SIZE_IN_DWORDS)
		return 0;

	idx = (idx + trace_seq_data->start_offset) % ROGUE_FW_TRACE_BUF_DEFAULT_SIZE_IN_DWORDS;
	return trace_seq_data->buffer[idx];
}

/**
 * fw_trace_get_next() - Advance trace index to next entry
 * @trace_seq_data: Trace sequence data.
 *
 * Returns:
 *  * %true if trace index is now pointing to a valid entry, or
 *  * %false if trace index is pointing to an invalid entry, or has hit the end
 *    of the trace.
 */
static bool fw_trace_get_next(struct pvr_fw_trace_seq_data *trace_seq_data)
{
	u32 id, sf_id;

	while (trace_seq_data->idx < ROGUE_FW_TRACE_BUF_DEFAULT_SIZE_IN_DWORDS) {
		id = read_fw_trace(trace_seq_data, 0);
		trace_seq_data->idx++;
		if (!ROGUE_FW_LOG_VALIDID(id))
			continue;
		if (id == ROGUE_FW_SF_MAIN_ASSERT_FAILED) {
			/* Assertion failure marks the end of the trace. */
			return false;
		}

		sf_id = find_sfid(id);
		if (sf_id == ROGUE_FW_SF_FIRST)
			continue;
		if (sf_id == ROGUE_FW_SF_LAST) {
			/*
			 * Could not match with an ID in the SF table, trace is
			 * most likely corrupt from this point.
			 */
			return false;
		}

		/* Skip over the timestamp, and any parameters. */
		trace_seq_data->idx += 2 + ROGUE_FW_SF_PARAMNUM(id);

		/* Ensure index is now pointing to a valid trace entry. */
		id = read_fw_trace(trace_seq_data, 0);
		if (!ROGUE_FW_LOG_VALIDID(id))
			continue;

		return true;
	};

	/* Hit end of trace data. */
	return false;
}

/**
 * fw_trace_get_first() - Find first valid entry in trace
 * @trace_seq_data: Trace sequence data.
 *
 * Skips over invalid (usually zero) and ROGUE_FW_SF_FIRST entries.
 *
 * If the trace has no valid entries, this function will exit with the trace
 * index pointing to the end of the trace. trace_seq_show() will return an error
 * in this state.
 */
static void fw_trace_get_first(struct pvr_fw_trace_seq_data *trace_seq_data)
{
	trace_seq_data->idx = 0;

	while (trace_seq_data->idx < ROGUE_FW_TRACE_BUF_DEFAULT_SIZE_IN_DWORDS) {
		u32 id = read_fw_trace(trace_seq_data, 0);

		if (ROGUE_FW_LOG_VALIDID(id)) {
			u32 sf_id = find_sfid(id);

			if (sf_id != ROGUE_FW_SF_FIRST)
				break;
		}
		trace_seq_data->idx++;
	}
}

static void *fw_trace_seq_start(struct seq_file *s, loff_t *pos)
{
	struct pvr_fw_trace_seq_data *trace_seq_data = s->private;
	u32 i;

	/* Reset trace index, then advance to *pos. */
	fw_trace_get_first(trace_seq_data);

	for (i = 0; i < *pos; i++) {
		if (!fw_trace_get_next(trace_seq_data))
			return NULL;
	}

	return (trace_seq_data->idx < ROGUE_FW_TRACE_BUF_DEFAULT_SIZE_IN_DWORDS) ? pos : NULL;
}

static void *fw_trace_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	struct pvr_fw_trace_seq_data *trace_seq_data = s->private;

	(*pos)++;
	if (!fw_trace_get_next(trace_seq_data))
		return NULL;

	return (trace_seq_data->idx < ROGUE_FW_TRACE_BUF_DEFAULT_SIZE_IN_DWORDS) ? pos : NULL;
}

static void fw_trace_seq_stop(struct seq_file *s, void *v)
{
}

static int fw_trace_seq_show(struct seq_file *s, void *v)
{
	struct pvr_fw_trace_seq_data *trace_seq_data = s->private;
	u64 timestamp;
	u32 id;
	u32 sf_id;
	int err;

	if (trace_seq_data->idx >= ROGUE_FW_TRACE_BUF_DEFAULT_SIZE_IN_DWORDS) {
		err = -EINVAL;
		goto err_out;
	}

	id = read_fw_trace(trace_seq_data, 0);
	if (!ROGUE_FW_LOG_VALIDID(id)) {
		/* Index is not pointing at a valid entry. */
		err = -EINVAL;
		goto err_out;
	}

	sf_id = find_sfid(id);
	if (sf_id == ROGUE_FW_SF_LAST) {
		/* Index is not pointing at a valid entry. */
		err = -EINVAL;
		goto err_out;
	}

	timestamp = read_fw_trace(trace_seq_data, 1) |
		((u64)read_fw_trace(trace_seq_data, 2) << 32);
	timestamp = (timestamp & ~ROGUE_FWT_TIMESTAMP_TIME_CLRMSK) >>
		ROGUE_FWT_TIMESTAMP_TIME_SHIFT;

	seq_printf(s, "[%llu] : ", timestamp);
	if (id == ROGUE_FW_SF_MAIN_ASSERT_FAILED) {
		seq_printf(s, "ASSERTION %s failed at %s:%u",
			   trace_seq_data->assert_buf.info,
			   trace_seq_data->assert_buf.path,
			   trace_seq_data->assert_buf.line_num);
	} else {
		seq_printf(s, stid_fmts[sf_id].name,
			   read_fw_trace(trace_seq_data, 3),
			   read_fw_trace(trace_seq_data, 4),
			   read_fw_trace(trace_seq_data, 5),
			   read_fw_trace(trace_seq_data, 6),
			   read_fw_trace(trace_seq_data, 7),
			   read_fw_trace(trace_seq_data, 8),
			   read_fw_trace(trace_seq_data, 9),
			   read_fw_trace(trace_seq_data, 10),
			   read_fw_trace(trace_seq_data, 11),
			   read_fw_trace(trace_seq_data, 12),
			   read_fw_trace(trace_seq_data, 13),
			   read_fw_trace(trace_seq_data, 14),
			   read_fw_trace(trace_seq_data, 15),
			   read_fw_trace(trace_seq_data, 16),
			   read_fw_trace(trace_seq_data, 17),
			   read_fw_trace(trace_seq_data, 18),
			   read_fw_trace(trace_seq_data, 19),
			   read_fw_trace(trace_seq_data, 20),
			   read_fw_trace(trace_seq_data, 21),
			   read_fw_trace(trace_seq_data, 22));
	}
	seq_puts(s, "\n");
	return 0;

err_out:
	return err;
}

static const struct seq_operations pvr_fw_trace_seq_ops = {
	.start = fw_trace_seq_start,
	.next = fw_trace_seq_next,
	.stop = fw_trace_seq_stop,
	.show = fw_trace_seq_show
};

static int fw_trace_open(struct inode *inode, struct file *file)
{
	struct pvr_fw_trace_buffer *trace_buffer = inode->i_private;
	struct rogue_fwif_tracebuf_space *tracebuf_space =
		trace_buffer->tracebuf_space;
	struct pvr_fw_trace_seq_data *trace_seq_data;
	int err;

	trace_seq_data = kzalloc(sizeof(*trace_seq_data), GFP_KERNEL);
	if (!trace_seq_data)
		return -ENOMEM;

	trace_seq_data->buffer = kcalloc(ROGUE_FW_TRACE_BUF_DEFAULT_SIZE_IN_DWORDS,
					 sizeof(*trace_seq_data->buffer), GFP_KERNEL);
	if (!trace_seq_data->buffer) {
		err = -ENOMEM;
		goto err_free_data;
	}

	/*
	 * Take a local copy of the trace buffer, as firmware may still be
	 * writing to it. This will exist as long as this file is open.
	 */
	memcpy(trace_seq_data->buffer, trace_buffer->buf,
	       ROGUE_FW_TRACE_BUF_DEFAULT_SIZE_IN_DWORDS * sizeof(u32));
	trace_seq_data->start_offset = READ_ONCE(tracebuf_space->trace_pointer);
	trace_seq_data->assert_buf = tracebuf_space->assert_buf;
	fw_trace_get_first(trace_seq_data);

	err = seq_open(file, &pvr_fw_trace_seq_ops);
	if (err)
		goto err_free_buffer;

	((struct seq_file *)file->private_data)->private = trace_seq_data;

	return 0;

err_free_buffer:
	kfree(trace_seq_data->buffer);

err_free_data:
	kfree(trace_seq_data);

	return err;
}

static int fw_trace_release(struct inode *inode, struct file *file)
{
	struct pvr_fw_trace_seq_data *trace_seq_data =
		((struct seq_file *)file->private_data)->private;

	seq_release(inode, file);
	kfree(trace_seq_data->buffer);
	kfree(trace_seq_data);

	return 0;
}

static const struct file_operations pvr_fw_trace_fops = {
	.owner = THIS_MODULE,
	.open = fw_trace_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = fw_trace_release,
};

void
pvr_fw_trace_mask_update(struct pvr_device *pvr_dev, u32 old_mask, u32 new_mask)
{
	if (old_mask != new_mask)
		update_logtype(pvr_dev, new_mask);
}

void
pvr_fw_trace_debugfs_init(struct pvr_device *pvr_dev, struct dentry *dir)
{
	struct pvr_fw_trace *fw_trace = &pvr_dev->fw_dev.fw_trace;
	u32 thread_nr;

	static_assert(ARRAY_SIZE(fw_trace->buffers) <= 10,
		      "The filename buffer is only large enough for a "
		      "single-digit thread count");

	for (thread_nr = 0; thread_nr < ARRAY_SIZE(fw_trace->buffers); ++thread_nr) {
		char filename[8];

		snprintf(filename, ARRAY_SIZE(filename), "trace_%u", thread_nr);
		debugfs_create_file(filename, 0400, dir,
				    &fw_trace->buffers[thread_nr],
				    &pvr_fw_trace_fops);
	}
}
#endif
