/**
 * Simple mtd driver that dynamically allocates/frees memory
 *
 * $Id: ramtd.c,v 1.5 2005/05/20 15:50:43 joern Exp $
 *
 * Copyright (c) 2005 Joern Engel <joern@wh.fh-wedel.de>
 */
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/vmalloc.h>

struct ramtd {
	struct mtd_info mtd;
	struct list_head list;
	u32 size;
	void *page[];
};

static LIST_HEAD(ramtd_list);
static DECLARE_MUTEX(ramtd_mutex);


static void *get_pool_page(void)
{
	void *ret = (void*)__get_free_page(GFP_KERNEL);
	return ret;
}


static void free_pool_page(void *page)
{
	free_page((unsigned long)page);
}


static int ramtd_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	struct ramtd *ramtd = container_of(mtd, typeof(*ramtd), mtd);
	u32 addr = instr->addr;
	u32 len = instr->len;

	if (addr + len > mtd->size)
		return -EINVAL;
	if (addr % PAGE_SIZE)
		return -EINVAL;
	if (len % PAGE_SIZE)
		return -EINVAL;

	while (len) {
		u32 page = addr / PAGE_SIZE;

		down_interruptible(&ramtd_mutex);
		free_pool_page(ramtd->page[page]);
		ramtd->page[page] = NULL;
		up(&ramtd_mutex);

		addr += PAGE_SIZE;
		len -= PAGE_SIZE;
	}
	instr->state = MTD_ERASE_DONE;
	mtd_erase_callback(instr);

	return 0;
}


static int ramtd_read(struct mtd_info *mtd, loff_t from, size_t len,
		size_t *retlen, u_char *buf)
{
	struct ramtd *ramtd = container_of(mtd, typeof(*ramtd), mtd);

	if (from >= mtd->size)
		return -EINVAL;

	if (len > mtd->size - from)
		len = mtd->size - from;

	*retlen = 0;
	while (len) {
		u32 page = from / PAGE_SIZE;
		u32 ofs = from % PAGE_SIZE;
		u32 rlen = min_t(u32, len, PAGE_SIZE - ofs);

		down_interruptible(&ramtd_mutex);
		if (!ramtd->page[page])
			memset(buf, 0xff, rlen);
		else
			memcpy(buf, ramtd->page[page] + ofs, rlen);
		up(&ramtd_mutex);

		buf += rlen;
		from += rlen;
		*retlen += rlen;
		len -= rlen;
	}
	return 0;
}


static int ramtd_write(struct mtd_info *mtd, loff_t to, size_t len,
		size_t *retlen, const u_char *buf)
{
	struct ramtd *ramtd = container_of(mtd, typeof(*ramtd), mtd);

	if (to >= mtd->size)
		return -EINVAL;

	if (len > mtd->size - to)
		len = mtd->size - to;

	*retlen = 0;
	while (len) {
		u32 page = to / PAGE_SIZE;
		u32 ofs = to % PAGE_SIZE;
		u32 wlen = min_t(u32, len, PAGE_SIZE - ofs);

		down_interruptible(&ramtd_mutex);
		if (!ramtd->page[page]) {
			ramtd->page[page] = get_pool_page();
			memset(ramtd->page[page], 0xff, PAGE_SIZE);
		}
		if (!ramtd->page[page])
			return -EIO;

		memcpy(ramtd->page[page] + ofs, buf, wlen);
		up(&ramtd_mutex);

		buf += wlen;
		to += wlen;
		*retlen += wlen;
		len -= wlen;
	}
	return 0;
}


static int register_device(const char *name, u32 size)
{
	struct ramtd *new;
	u32 pages_size;

	size = PAGE_ALIGN(size);
	pages_size = size / PAGE_SIZE * sizeof(void*);
	new = vmalloc(sizeof(*new) + pages_size);
	if (!new)
		return -ENOMEM;
	memset(new, 0, sizeof(*new) + pages_size);

	new->mtd.name = (char*)name;
	new->mtd.size = size;
	new->mtd.flags = MTD_CAP_RAM | MTD_ERASEABLE | MTD_VOLATILE;
	new->mtd.owner = THIS_MODULE;
	new->mtd.type = MTD_RAM;
	new->mtd.erasesize = PAGE_SIZE;
	new->mtd.write = ramtd_write;
	new->mtd.read = ramtd_read;
	new->mtd.erase = ramtd_erase;

	if (add_mtd_device(&new->mtd)) {
		free_pool_page(new);
		return -EAGAIN;
	}

	down_interruptible(&ramtd_mutex);
	list_add(&new->list, &ramtd_list);
	up(&ramtd_mutex);
	return 0;
}


static int __init ramtd_init(void)
{
	return register_device("ramtd", 4*1024*1024); /* FIXME */
}

static void __exit ramtd_exit(void)
{
	struct ramtd *this, *next;

	down_interruptible(&ramtd_mutex);
	list_for_each_entry_safe(this, next, &ramtd_list, list) {
		del_mtd_device(&this->mtd);
		kfree(this);
	}
	up(&ramtd_mutex);
}


module_init(ramtd_init);
module_exit(ramtd_exit);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joern Engel <joern@wh.fh-wedel.de>");
MODULE_DESCRIPTION("MTD using dynamic memory allocation");
