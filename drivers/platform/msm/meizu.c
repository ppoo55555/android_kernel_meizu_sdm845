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
#include <linux/printk.h>
#include <linux/types.h>

extern dev_t sd_lookup_partition(const char *name);

int mz_part_read(const char *part, char *buf, size_t count, loff_t offset)
{
	int retry = 50;
	sector_t start, size;
	dev_t part_devt = MKDEV(0, 0);
	struct block_device *bdev;
	struct bio *bio;
	struct page *page;
	int ret = 0;
	
	DECLARE_COMPLETION_ONSTACK(wait);

	if (count > (1 << SECTOR_SHIFT) || offset % (1 << SECTOR_SHIFT)) {
		pr_err("%s: unsupported count or offset\n", __func__);
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

	if (!bio_add_page(bio, page, PAGE_SIZE, 0)) {
		pr_err("%s: bio_add_page error\n", __func__);
		ret = -EIO;
		goto free_page;
	}
	
	if (submit_bio_wait(bio)) {
		pr_err("%s: bio read failed\n", __func__);
		ret = -EIO;
		goto free_page;
	}

	memcpy(buf, page_address(page), count);
	ret = 0;
	
free_page:
	__free_page(page);
free_bio:
	bio_put(bio);
free_blkdev:
	blkdev_put(bdev, FMODE_READ);
	return ret;
}

// ssize_t mz_part_read(const char *part, char *buf, size_t count, loff_t offset)
// {
// 	struct file *fp;
// 	char path[128];
// 	mm_segment_t old_fs;
// 	ssize_t ret = 0;

// 	snprintf(path, 128, "/dev/block/bootdevice/by-name/%s", part);

// 	old_fs = get_fs();
// 	set_fs(get_ds());
// 	fp = filp_open(path, O_RDONLY, 0);
// 	mdelay(5);
// 	if (IS_ERR(fp)) {
// 		pr_err("%s: Failed to open private partition: %d\n",
// 			__func__, PTR_ERR(fp));
// 		goto out;
// 	}

// 	fp->f_pos = offset;
// 	if (!fp->f_op->read)
// 		ret = vfs_read(fp, buf, count, &fp->f_pos);
// 	else
// 		ret = fp->f_op->read(fp, buf, count, &fp->f_pos);

// 	filp_close(fp, NULL);

// out:
//   	set_fs(old_fs);
//     return ret;
// }
EXPORT_SYMBOL(mz_part_read);
