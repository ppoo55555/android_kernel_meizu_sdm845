/*
 *  Copyright (C) 2024 MeizuCustoms enthusiasts
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include <asm/uaccess.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/device-mapper.h>
#include <linux/device.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/meizu.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/types.h>

#define UFS_BLK_SIZE 4096

static struct mz_device_info g_device = {
	.hw_version = 1 << 8,
	.sw_version = "0xdeadbeef",
	.model = MZ_DEVICE_UNKNOWN,
};
static struct proc_dir_entry *g_mz_dir, *g_mz_model, *g_mz_hw_ver;

extern dev_t sd_lookup_partition(const char *name);

static inline int __mz_part_read(const char *part, struct page **buf, loff_t offset)
{
	dev_t part_devt = MKDEV(0, 0);
	struct block_device *bdev;
	struct bio *bio;
	struct page *page;
	int ret = 0;

	if (offset % UFS_BLK_SIZE) {
		pr_err("%s: unsupported offset %lu\n", __func__, offset);
		return -EINVAL;
	}

	part_devt = sd_lookup_partition(part);
	if (MAJOR(part_devt) == 0) {
		pr_err("%s: failed to get block device type\n", __func__);
		return -EIO;
	}
	
	bdev = blkdev_get_by_dev(part_devt, FMODE_READ, NULL);
	if (IS_ERR(bdev)) {
		pr_err("%s: failed to get block device\n", __func__);
		return PTR_ERR(bdev);
	}

	bio = bio_alloc(GFP_KERNEL, 1);
	if (!bio) {
		pr_err("%s: failed to allocate bio\n", __func__);
		ret = -ENOMEM;
		goto free_blkdev;
	}

	page = alloc_page(GFP_KERNEL);
	if (!page) {
		pr_err("%s: failed to allocate page\n", __func__);
		ret = -ENOMEM;
		goto free_bio;
	}

	bio->bi_bdev = bdev;
	bio->bi_iter.bi_sector = offset >> SECTOR_SHIFT;
	bio_set_op_attrs(bio, REQ_OP_READ, READ_SYNC);

	if (!bio_add_page(bio, page, UFS_BLK_SIZE, 0)) {
		pr_err("%s: bio_add_page error\n", __func__);
		ret = -EIO;
		goto free_page;
	}
	
	if (submit_bio_wait(bio)) {
		pr_err("%s: bio read failed\n", __func__);
		ret = -EIO;
		goto free_page;
	}

	*buf = page;
	ret = 0;
	goto free_bio;

free_page:
	__free_page(page);
free_bio:
	bio_put(bio);
free_blkdev:
	blkdev_put(bdev, FMODE_READ);
	return ret;
}

int mz_part_read(const char *part, char *buf, size_t count, loff_t offset)
{
	int ret;
	loff_t shift = offset % UFS_BLK_SIZE;
	struct page *page;

	if (shift + count > UFS_BLK_SIZE) {
		pr_err("%s: overreading 4K block by %ld bytes\n",
			__func__, shift + count - UFS_BLK_SIZE);
		return -EINVAL;
	}

	ret = __mz_part_read(part, &page, offset - shift);
	if (ret < 0)
		return ret;

	memcpy(buf, page_address(page) + shift, count);
	__free_page(page);
	
	return ret;
}
EXPORT_SYMBOL(mz_part_read);

int mz_get_hw_version(void)
{
	return g_device.hw_version;
}
EXPORT_SYMBOL(mz_get_hw_version);

enum mz_device_model mz_get_model(void)
{
	return g_device.model;
}
EXPORT_SYMBOL(mz_get_model);

static int mz_gather_bootinfo(void)
{
	struct device_node *node =
		of_find_node_by_path("/bootinfo");
	const char *sw_version = NULL;
	int ret = 0;
	
	if (!node) {
		pr_err("%s: failed to find bootinfo\n", __func__);
		return -EINVAL;
	}

	ret = of_property_read_string(node,
		"sw_version", &sw_version);
	if (ret < 0) {
		pr_err("%s: failed to get sw_version\n", __func__);
		return -EINVAL;
	} else
		strncpy(g_device.sw_version, sw_version, 16);

	ret = of_property_read_u32(node,
		"hw_version", &g_device.hw_version);
	if (ret < 0) {
		pr_err("%s: failed to get hw_version\n", __func__);
		return -EINVAL;
	}

	// TODO: Zero support
	// if doesn't match 0x8X2XXXXX pattern
	if (g_device.sw_version[2] != '8'
		  || g_device.sw_version[4] != '2') {
invalid_sw_version:
		pr_warn("%s: invalid sw_version: %s\n",
			__func__, g_device.sw_version);
		g_device.model = MZ_DEVICE_UNKNOWN;
		return 0;
	}

	switch (g_device.sw_version[3]) {
	case '8': // 0x882XXXXX
		g_device.model = MZ_DEVICE_16TH;
		break;
	case '9': // 0x892XXXXX
		g_device.model = MZ_DEVICE_16THPLUS;
		break;
	default:
		goto invalid_sw_version;
	}

	return 0;
}

static inline const char *get_model_id(enum mz_device_model model)
{
	switch (model) {
	case MZ_DEVICE_UNKNOWN:
		return "UNKNOWN";
	case MZ_DEVICE_16TH:
		return "16TH";
	case MZ_DEVICE_16THPLUS:
		return "16THPLUS";
	case MZ_DEVICE_ZERO:
		return "ZERO";
	}
}

static inline const char *get_model_name(enum mz_device_model model)
{
	switch (model) {
	case MZ_DEVICE_UNKNOWN:
		return "Unknown";
	case MZ_DEVICE_16TH:
		return "16th";
	case MZ_DEVICE_16THPLUS:
		return "16th Plus";
	case MZ_DEVICE_ZERO:
		return "Zero";
	}
}

static int mz_model_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s\n", get_model_id(g_device.model));
	return 0;
}

static int mz_model_open(struct inode *inode, struct file *file)
{
	return single_open(file, mz_model_show, NULL);
}

static struct file_operations mz_model_fops = {
	.owner = THIS_MODULE,
	.open = mz_model_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int mz_hw_ver_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", g_device.hw_version);
	return 0;
}

static int mz_hw_ver_open(struct inode *inode, struct file *file)
{
	return single_open(file, mz_hw_ver_show, NULL);
}

static struct file_operations mz_hw_ver_fops = {
	.owner = THIS_MODULE,
	.open = mz_hw_ver_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void mz_init_proc(void) {
	g_mz_dir = proc_mkdir("meizu", NULL);
	if (!g_mz_dir) {
		pr_warn("%s: failed to create /proc/meizu\n", __func__);
		return;
	}

	g_mz_model = proc_create("model", 0644, g_mz_dir, &mz_model_fops);
	if (!g_mz_model) {
		pr_warn("%s: failed to create /proc/meizu/model\n", __func__);
		return;
	}

	g_mz_hw_ver = proc_create("hw_version", 0644, g_mz_dir, &mz_hw_ver_fops);
	if (!g_mz_hw_ver) {
		pr_warn("%s: failed to create /proc/meizu/hw_version\n", __func__);
		return;
	}
}

static void mz_exit_proc(void) {
	if (g_mz_hw_ver)
		proc_remove(g_mz_hw_ver);
	if (g_mz_model)
		proc_remove(g_mz_model);
	if (g_mz_dir)
		proc_remove(g_mz_dir);
}

int __init mz_init(void)
{
	int ret = mz_gather_bootinfo();
	if (ret < 0)
		return ret;

	pr_info("%s: running MS kernel on Meizu %s (hardware ver: %d)\n",
		__func__, get_model_name(g_device.model), g_device.hw_version);

	mz_init_proc();
	return 0;
}

void __exit mz_exit(void)
{
	return;
}

arch_initcall(mz_init);
module_exit(mz_exit);
