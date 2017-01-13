/*
 *	fs/f2fs/alfs_ext.h
 * 
 *	Copyright (c) 2013 MIT CSAIL
 * 
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License version 2 as
 *	published by the Free Software Foundation.
 **/

#include <linux/fs.h>
#include <linux/f2fs_fs.h>
#include <linux/bio.h>
#include <linux/blkdev.h>

#include "f2fs.h"
#include "alfs_ext.h"
#include "segment.h"

void f2fs_write_end_io(struct bio *bio);
/* 
 * Handling read and write operations 
 */
static void alfs_end_io_flash (struct bio *bio)
{
	struct alfs_bio_private *p = NULL;
	if (bio){
		__u32 read_size = 0;
		sector_t read_sector = bio->bi_iter.bi_sector;
		struct bio_vec* v;

		for(v = bio->bi_io_vec; v < &bio->bi_io_vec[bio->bi_iter.bi_idx]; ++v)
		{
			read_sector -= v->bv_len / 512;
			read_size += v->bv_len;
		}
		// dm_robusta_end_io()에서 읽어온 4k bio의 sector는 NVME_LBA를 의미한다

		p = bio->bi_private;
		if(p){
			if (p->page) {
				ClearPageUptodate (p->page);
				unlock_page (p->page);
				__free_pages (p->page, 0);
			}

			if (p->is_sync)
				complete (p->wait);

			kfree (p);
		}
		bio_put (bio);
	}
}

static int32_t alfs_readpage_flash (struct f2fs_sb_info *sbi, struct page* page, block_t blkaddr)
{
	//struct block_device* bdev = sbi->sb->s_bdev;
	struct bio* bio = NULL;
	struct alfs_bio_private* p = NULL;

	DECLARE_COMPLETION_ONSTACK (wait);

retry:
	p = kmalloc (sizeof (struct alfs_bio_private), GFP_NOFS);
	if (!p) {
		cond_resched();
		goto retry;
	}

	/* allocate a new bio */
	bio = f2fs_bio_alloc (1);

	/* initialize the bio */
	bio->bi_iter.bi_sector = SECTOR_FROM_BLOCK (blkaddr);
	bio->bi_end_io = alfs_end_io_flash;
	bio->bi_bdev = sbi->sb->s_bdev;

	p->sbi = sbi;
	p->page = NULL;
	bio->bi_private = p;

	/* put a bio into a bio queue */
	if (bio_add_page (bio, page, PAGE_SIZE, 0) < PAGE_SIZE) {
		f2fs_msg(sbi->sb, KERN_ERR, "Error occur while calling alfs_readpage");
		kfree (bio->bi_private);
		bio_put (bio);
		return -EFAULT;
	}

	/* submit a bio request to a device */
	p->is_sync = true;
	p->wait = &wait;
	bio->bi_opf = REQ_OP_READ;
	submit_bio (bio);
	wait_for_completion (&wait);

	/* see if page is correct or not */
	if (PageError(page))
		return -EIO;

	return 0;
}

static int32_t alfs_writepage_flash (struct f2fs_sb_info *sbi, struct page* page, block_t blkaddr, uint8_t sync)
{
	//struct block_device* bdev = sbi->sb->s_bdev;
	struct bio* bio = NULL;
	struct alfs_bio_private* p = NULL;
	DECLARE_COMPLETION_ONSTACK (wait);

retry:
	p = kmalloc (sizeof (struct alfs_bio_private), GFP_NOFS);
	if (!p) {
		cond_resched();
		goto retry;
	}

	p->sbi = sbi;
	p->page = page;

	/* allocate a new bio */
	bio = f2fs_bio_alloc (1);

	/* initialize the bio */
	bio->bi_private = p;
	bio->bi_iter.bi_sector = SECTOR_FROM_BLOCK (blkaddr);
	bio->bi_end_io = alfs_end_io_flash;
	bio->bi_bdev = sbi->sb->s_bdev;

	/* put a bio into a bio queue */
	if (bio_add_page (bio, page, PAGE_SIZE, 0) < PAGE_SIZE) {
		f2fs_msg(sbi->sb, KERN_ERR, "Error occur while calling alfs_readpage");
		kfree (bio->bi_private);
		bio_put (bio);
		return -EFAULT;
	}


	/* submit a bio request to a device */
	if (sync == 1) {
		p->is_sync = true;
		p->wait = &wait;
	} else {
		p->is_sync = false;
	}

	if (test_opt(sbi, NOBARRIER)){
		bio_set_op_attrs(bio, REQ_OP_WRITE, REQ_PREFLUSH | REQ_META | REQ_PRIO);
	}
	else{
		bio_set_op_attrs(bio, REQ_OP_WRITE, REQ_PREFLUSH | REQ_META | REQ_PRIO | REQ_FUA);
	}

	submit_bio (bio);
	if (sync == 1) {
		wait_for_completion (&wait);
	}

	/* see if page is correct or not */
	if (PageError(page))
		return -EIO;

	return 0;
}

static struct bio* get_new_bio(struct f2fs_sb_info *sbi, int npages)
{
	/* allocate a new bio */
	struct bio* bio = NULL;
	bio = f2fs_bio_alloc (npages);

	/* initialize the bio */
	// bio->bi_private = p;
	// bio->bi_iter.bi_sector = SECTOR_FROM_BLOCK (blkaddr);
	bio->bi_end_io = alfs_end_io_flash;
	bio->bi_bdev = sbi->sb->s_bdev;

	if(test_opt(sbi, NOBARRIER)){
		bio_set_op_attrs(bio, REQ_OP_WRITE, REQ_PREFLUSH | REQ_META | REQ_PRIO);
	}
	else{
		bio_set_op_attrs(bio, REQ_OP_WRITE, REQ_PREFLUSH | REQ_META | REQ_PRIO | REQ_FUA);
	}
	return bio;
}

static int32_t alfs_write_bio_flash (struct f2fs_sb_info *sbi, struct bio* bio, uint8_t sync)
{
	//struct block_device* bdev = sbi->sb->s_bdev;
	struct alfs_bio_private* p = NULL;
	struct bio_vec* v;
	DECLARE_COMPLETION_ONSTACK (wait);

retry:
	p = kmalloc (sizeof (struct alfs_bio_private), GFP_NOFS);
	if (!p) {
		cond_resched();
		goto retry;
	}

	v = bio->bi_io_vec;
	p->sbi = sbi;
	p->page = v->bv_page;	// the first page of bio

	bio->bi_private = p;

	/* submit a bio request to a device */
	if (sync == 1) {
		p->is_sync = true;
		p->wait = &wait;
	} else {
		p->is_sync = false;
	}

	submit_bio (bio);
	//bio->bi_end_io (bio);
	if (sync == 1) {
		wait_for_completion (&wait);
	}

	return 0;
}





int8_t alfs_readpage (struct f2fs_sb_info* sbi, struct page* page, block_t pblkaddr)
{
	int8_t ret;
	struct f2fs_bio_info *io;
	io = &sbi->read_io;

	down_read (&io->io_rwsem);
	ret = alfs_readpage_flash (sbi, page, pblkaddr);
	up_read (&io->io_rwsem);
	return ret;
}

int8_t alfs_writepage (
	struct f2fs_sb_info* sbi, 
	struct page* page, 
	block_t pblkaddr, 
	uint8_t sync)
{
	int8_t ret;
	struct f2fs_bio_info *io;
	io = &sbi->write_io[WRITE];
	down_write(&io->io_rwsem);
	ret = alfs_writepage_flash (sbi, page, pblkaddr, sync);
	io = &sbi->write_io[WRITE];
	return ret;
}


/*
 * Create mapping & summary tables 
 */
static int32_t create_metalog_mapping_table (struct f2fs_sb_info* sbi)
{
	struct alfs_info* ri = ALFS_RI (sbi);
	struct page* page = NULL;

	uint32_t i = 0, j = 0;
	uint8_t is_dead_section = 1;
	int32_t ret = 0;

	/* get the geometry information */
	ri->nr_mapping_phys_blks = NR_MAPPING_SECS * ri->blks_per_sec;
	ri->nr_mapping_logi_blks = ri->nr_metalog_logi_blks / 1020;
	if (ri->nr_metalog_logi_blks % 1020 != 0) {
		ri->nr_mapping_logi_blks++;
	}

	f2fs_msg(sbi->sb, KERN_INFO, "--------------------------------");
	f2fs_msg(sbi->sb, KERN_INFO, " # of mapping entries: %u", ri->nr_metalog_logi_blks);
	f2fs_msg(sbi->sb, KERN_INFO, " * mapping table blkaddr: %u (blk)", ri->mapping_blkofs);
	f2fs_msg(sbi->sb, KERN_INFO , " * mapping table length: %u (blk)", ri->nr_mapping_phys_blks);

	/* allocate the memory space for the summary table */
	if ((ri->map_blks = (struct alfs_map_blk*)kmalloc (
			sizeof (struct alfs_map_blk) * ri->nr_mapping_logi_blks, GFP_KERNEL)) == NULL) {
		f2fs_msg(sbi->sb, KERN_INFO, "Errors occur while allocating memory space for the mapping table");
		goto out;
	}
	memset (ri->map_blks, 0x00, sizeof (struct alfs_map_blk) * ri->nr_mapping_logi_blks);

	/* get the free page from the memory pool */
	page = alloc_page (GFP_NOFS | __GFP_ZERO);
	if (IS_ERR (page)) {
		f2fs_msg(sbi->sb, KERN_INFO, "Errors occur while allocating page");
		kfree (ri->map_blks);
		ret = PTR_ERR (page);
		goto out;
	}
	lock_page (page);

	/* read the mapping info from the disk */
	ri->mapping_gc_sblkofs = -1;
	ri->mapping_gc_eblkofs = -1;

	/* read the mapping info from the disk */
	for (i = 0; i < NR_MAPPING_SECS; i++) {
		is_dead_section = 1;

		for (j = 0; j < ri->blks_per_sec; j++) {
			__le32* ptr_page_addr = NULL;
			struct alfs_map_blk* new_map_blk = NULL;
			struct f2fs_bio_info *io;
			io = &sbi->read_io;

			/* read the mapping data from NAND devices */
			down_read (&io->io_rwsem);
			if (alfs_readpage_flash (sbi, page, ri->mapping_blkofs + (i * ri->blks_per_sec) + j) != 0) {
				up_read (&io->io_rwsem);
				f2fs_msg(sbi->sb, KERN_INFO, "Errors occur while reading the mapping data from NAND devices");
				ret = -1;
				goto out;
			}
			up_read (&io->io_rwsem);

			/* get the virtual address from the page */
			ptr_page_addr = (__le32*)page_address (page);
			new_map_blk = (struct alfs_map_blk*)ptr_page_addr;

			/* check version # */
			if (new_map_blk->magic == cpu_to_le32 (0xEF)) {
				uint32_t index = le32_to_cpu (new_map_blk->index);
				if (le32_to_cpu (ri->map_blks[index/1020].ver) <= le32_to_cpu (new_map_blk->ver)) {
					memcpy (&ri->map_blks[index/1020], ptr_page_addr, F2FS_BLKSIZE);
					is_dead_section = 0; /* this section has a valid blk */
				}
			}

			/* goto the next page */
			ClearPageUptodate (page);
		}

		/* is it dead? */
		if (is_dead_section == 1) {
			f2fs_msg(sbi->sb, KERN_INFO, "dead section detected: %u\n", i);
			if (ri->mapping_gc_eblkofs == -1 && ri->mapping_gc_sblkofs == -1) {
				ri->mapping_gc_eblkofs = i * ri->blks_per_sec;
				ri->mapping_gc_sblkofs = i * ri->blks_per_sec + ri->blks_per_sec;
				ri->mapping_gc_sblkofs = ri->mapping_gc_sblkofs % ri->nr_mapping_phys_blks;
				alfs_do_trim (sbi, ri->mapping_blkofs + ri->mapping_gc_eblkofs, ri->blks_per_sec); 
			}
		}
	}

	/* is there a free section for the mapping table? */
	if (ri->mapping_gc_sblkofs == -1 || ri->mapping_gc_eblkofs == -1) {
		f2fs_msg(sbi->sb, KERN_ERR, "[ERROR] oops! there is no free space for the mapping table");
		ret = -1;
	} else {
		f2fs_msg(sbi->sb, KERN_INFO, "-------------------------------");
		f2fs_msg(sbi->sb, KERN_INFO, "ri->mapping_gc_slbkofs: %u (%u)", 
			ri->mapping_gc_sblkofs, ri->mapping_blkofs + ri->mapping_gc_sblkofs);
		f2fs_msg(sbi->sb, KERN_INFO, "ri->mapping_gc_eblkofs: %u (%u)", 
			ri->mapping_gc_eblkofs, ri->mapping_blkofs + ri->mapping_gc_eblkofs);
		f2fs_msg(sbi->sb, KERN_INFO, "-------------------------------");
	}

out:
	/* unlock & free the page */
	unlock_page (page);
	__free_pages (page, 0);

	return ret;
}

static int32_t create_metalog_summary_table (struct f2fs_sb_info* sbi)
{
	struct alfs_info* ri = ALFS_RI (sbi);
	uint32_t sum_length = 0;
	uint32_t i = 0, j = 0;
	uint8_t is_dead = 1;
	int32_t ret = 0;

	/* get the geometry information */
	sum_length = (sizeof (uint8_t) * ri->nr_metalog_phys_blks + F2FS_BLKSIZE - 1) / F2FS_BLKSIZE;

	f2fs_msg(sbi->sb, KERN_INFO, "--------------------------------");
	f2fs_msg(sbi->sb, KERN_INFO, " * summary table length: %u", sum_length);
	f2fs_msg(sbi->sb, KERN_INFO, "--------------------------------");

	/* allocate the memory space for the summary table */
	if ((ri->summary_table = 
			(uint8_t*)kmalloc (sum_length * F2FS_BLKSIZE, GFP_KERNEL)) == NULL) {
		f2fs_msg(sbi->sb, KERN_ERR, "Errors occur while allocating memory space for the mapping table");
		ret = -1;
		goto out;
	}

	/* set all the entries of the summary table invalid */
	memset (ri->summary_table, 2, sum_length * F2FS_BLKSIZE);

	/* set the entries which are vailid in the mapping valid */
	for (i = 0; i < ri->nr_mapping_logi_blks; i++) {
		for (j = 0; j < 1020; j++) {
			__le32 phyofs = ri->map_blks[i].mapping[j];
			if (le32_to_cpu (phyofs) != -1) {
				ri->summary_table[le32_to_cpu (phyofs) - ri->metalog_blkofs] = 1;
			}
		}
	}

	/* search for a section that contains only invalid blks */
	for (i = 0; i < ri->nr_metalog_phys_blks / ri->blks_per_sec; i++) {
		is_dead = 1;
		for (j = 0; j < ri->blks_per_sec; j++) {
			if (ri->summary_table[i*ri->blks_per_sec+j] != 2) {
				is_dead = 0;
				break;
			}
		}
		if (is_dead == 1) {
			ri->metalog_gc_eblkofs = i * ri->blks_per_sec;
			ri->metalog_gc_sblkofs = i * ri->blks_per_sec;
			ri->metalog_gc_sblkofs = (ri->metalog_gc_sblkofs + ri->blks_per_sec) % ri->nr_metalog_phys_blks;

			alfs_do_trim (sbi, ri->mapping_blkofs + ri->metalog_gc_eblkofs, ri->blks_per_sec); 
			memset (&ri->summary_table[i*ri->blks_per_sec], 0x00, ri->blks_per_sec);
			break;
		}
	}

	/* metalog must have at least one dead section */
	if (is_dead == 0) {
		f2fs_msg(sbi->sb, KERN_ERR, "[ERROR] oops! cannot find dead sections in metalog");
		ret = -1;
	} else {
		f2fs_msg(sbi->sb, KERN_INFO, "-------------------------------");
		f2fs_msg(sbi->sb, KERN_INFO, "ri->metalog_gc_sblkofs: %u (%u)",
			ri->metalog_gc_sblkofs, ri->metalog_blkofs + ri->metalog_gc_sblkofs);
		f2fs_msg(sbi->sb, KERN_INFO, "ri->metalog_gc_eblkofs: %u (%u)",
			ri->metalog_gc_eblkofs, ri->metalog_blkofs + ri->metalog_gc_eblkofs);
		f2fs_msg(sbi->sb, KERN_INFO, "-------------------------------");
	}

out:
	return ret;
}


static void destroy_metalog_summary_table (struct f2fs_sb_info* sbi)
{
	struct alfs_info* ri = ALFS_RI (sbi);
	if (ri->summary_table) {
		kfree (ri->summary_table);
		ri->summary_table = NULL;
	}
}

static void destroy_metalog_mapping_table (struct f2fs_sb_info* sbi)
{
	struct alfs_info* ri = ALFS_RI (sbi);
	if (ri->map_blks) {
		kfree (ri->map_blks);
		ri->map_blks = NULL;
	}
}

static void destroy_ri (struct f2fs_sb_info* sbi)
{
	if (sbi->ai) {
		kfree (sbi->ai);
		sbi->ai = NULL;
	}
}


/* 
 * create the structure for ALFS (ai) 
 */
int32_t alfs_create_ai (struct f2fs_sb_info* sbi)
{
	struct alfs_info* ri = NULL;
	uint32_t nr_logi_metalog_segments = 0;
	uint32_t nr_phys_metalog_segments = 0;

	/* create alfs_info structure */
	if ((ri = (struct alfs_info*)kmalloc (
			sizeof (struct alfs_info), GFP_KERNEL)) == NULL) {
		f2fs_msg(sbi->sb, KERN_INFO, "Errors occur while creating alfs_info");
		return -1;
	}
	sbi->ai = ri;

	/* initialize some variables */
	ri->mapping_blkofs = get_mapping_blkofs (sbi);
	ri->metalog_blkofs = get_metalog_blkofs (sbi);

	nr_logi_metalog_segments = get_nr_logi_meta_segments (sbi);
	nr_phys_metalog_segments = get_nr_phys_meta_segments (sbi, nr_logi_metalog_segments);

	ri->nr_metalog_logi_blks = SEGS2BLKS (sbi, nr_logi_metalog_segments);
	ri->nr_metalog_phys_blks = SEGS2BLKS (sbi, nr_phys_metalog_segments);

	ri->blks_per_sec = sbi->segs_per_sec * (1 << sbi->log_blocks_per_seg);

	/* create mutex for GC */
	mutex_init(&ri->alfs_gc_mutex);

	/* display information about metalog */
	f2fs_msg(sbi->sb, KERN_INFO, "--------------------------------");
	f2fs_msg(sbi->sb, KERN_INFO, " * mapping_blkofs: %u", ri->mapping_blkofs);
	f2fs_msg(sbi->sb, KERN_INFO, " * metalog_blkofs: %u", ri->metalog_blkofs);
	f2fs_msg(sbi->sb, KERN_INFO, " * # of blks per sec: %u", ri->blks_per_sec);
	f2fs_msg(sbi->sb, KERN_INFO, " * # of logical meta-log blks: %u", ri->nr_metalog_logi_blks);
	f2fs_msg(sbi->sb, KERN_INFO, " * # of physical meta-log blks: %u", ri->nr_metalog_phys_blks);
	f2fs_msg(sbi->sb, KERN_INFO, " * the range of logical meta address: %u - %u", 
		ri->metalog_blkofs, ri->metalog_blkofs + ri->nr_metalog_logi_blks);
	f2fs_msg(sbi->sb, KERN_INFO, " * the range of physical meta address: %u - %u", 
		ri->metalog_blkofs, ri->metalog_blkofs + ri->nr_metalog_phys_blks);

	return 0;
}

int32_t alfs_build_ai (struct f2fs_sb_info *sbi)
{
	/* see if ri is initialized or not */
	if (sbi == NULL || sbi->ai == NULL) {
		f2fs_msg(sbi->sb, KERN_ERR, "Error occur because some input parameters are NULL");
		return -1;
	}

	/* build meta-log mapping table */
	if (create_metalog_mapping_table (sbi) != 0) {
		f2fs_msg(sbi->sb, KERN_ERR, "Errors occur while creating the metalog mapping table");
		goto error_metalog_mapping;
	}

	/* build meta-log summary table */
	if (create_metalog_summary_table (sbi) != 0) {
		f2fs_msg(sbi->sb, KERN_ERR, "Errors occur while creating the metalog summary table");
		goto error_metalog_summary;
	}

	return 0;

error_metalog_summary:
	destroy_metalog_mapping_table (sbi);

error_metalog_mapping:

	return -1;
}

void alfs_destory_ai (struct f2fs_sb_info* sbi)
{
	destroy_metalog_summary_table (sbi);
	destroy_metalog_mapping_table (sbi);
	destroy_ri (sbi);
}


/*
 * mapping table management 
 */
int32_t get_mapping_free_blks (struct f2fs_sb_info* sbi)
{
	struct alfs_info* ri = ALFS_RI (sbi);
	uint32_t nr_free_blks;

	if (ri->mapping_gc_sblkofs < ri->mapping_gc_eblkofs) {
		nr_free_blks = ri->nr_mapping_phys_blks - ri->mapping_gc_eblkofs + ri->mapping_gc_sblkofs;
	} else if (ri->mapping_gc_sblkofs > ri->mapping_gc_eblkofs) {
		nr_free_blks = ri->mapping_gc_sblkofs - ri->mapping_gc_eblkofs;
	} else {
		f2fs_msg(sbi->sb, KERN_ERR, "[ERROR] 'ri->mapping_gc_sblkofs (%u)' is equal to 'ri->mapping_gc_eblkofs (%u)'", 
			ri->mapping_gc_sblkofs, ri->mapping_gc_eblkofs);
		nr_free_blks = -1;
	}

	return nr_free_blks;
}

int8_t is_mapping_gc_needed (struct f2fs_sb_info* sbi, int32_t nr_free_blks)
{
	if (nr_free_blks <= (sbi->segs_per_sec * sbi->blocks_per_seg)) {
		return 0;
	}
	return -1;
}

int8_t alfs_do_mapping_gc (struct f2fs_sb_info* sbi)
{
	struct alfs_info* ri = ALFS_RI (sbi);

	/* perform gc */
	alfs_do_trim (sbi, ri->mapping_blkofs + ri->mapping_gc_sblkofs, ri->blks_per_sec); 

	/* advance 'mapping_gc_sblkofs' */
	ri->mapping_gc_sblkofs = (ri->mapping_gc_sblkofs + ri->blks_per_sec) % 
		ri->nr_mapping_phys_blks;

	return 0;
}

int32_t alfs_write_mapping_entries (struct f2fs_sb_info* sbi)
{
	struct alfs_info* ri = ALFS_RI (sbi);
	struct page* page = NULL;
	int32_t nr_free_blks = 0;
	uint32_t i = 0;

	/* see if gc is needed for the mapping area */
	nr_free_blks = get_mapping_free_blks (sbi);
	if (is_mapping_gc_needed (sbi, nr_free_blks) == 0) {
		alfs_do_mapping_gc (sbi);
	}

	/* TODO: see if there are any dirty mapping entries */

	/* write dirty entries to the mapping area */
	for (i = 0; i < ri->nr_mapping_logi_blks; i++) {
		__le32* ptr_page_addr = NULL;
		uint32_t version = 0;

		/* see if it is dirty or not */
		if (ri->map_blks[i].dirty == 0) {
			continue;
		}

		/* increase version numbers */
		version = le32_to_cpu (ri->map_blks[i].ver) + 1;
		ri->map_blks[i].ver = cpu_to_le32 (version);
		ri->map_blks[i].dirty = cpu_to_le32 (0);

		/* get the free page from the memory pool */
		page = alloc_page (GFP_NOFS | __GFP_ZERO);
		if (IS_ERR (page)) {
			f2fs_msg(sbi->sb, KERN_ERR, "Errors occur while allocating a new page");
			return PTR_ERR (page);
		}
		lock_page (page);

		/* write dirty entires to NAND flash */
		ptr_page_addr = (__le32*)page_address (page);
		memcpy (ptr_page_addr, &ri->map_blks[i], F2FS_BLKSIZE);

		alfs_writepage_flash (sbi, page, ri->mapping_blkofs + ri->mapping_gc_eblkofs, 0);

		/* update physical location */
		ri->mapping_gc_eblkofs = 
			(ri->mapping_gc_eblkofs + 1) % ri->nr_mapping_phys_blks;
	}

	return 0;
}


/*
 * metalog management 
 */
int32_t is_valid_meta_lblkaddr (struct f2fs_sb_info* sbi, 
	block_t lblkaddr)
{
	struct alfs_info* ri = ALFS_RI (sbi);

	if (sbi->ai == NULL)
		return -1;
	
	if (lblkaddr >= ri->metalog_blkofs &&
		lblkaddr < ri->metalog_blkofs + ri->nr_metalog_logi_blks)
		return 0;

	return -1;
}

int32_t is_valid_meta_pblkaddr (struct f2fs_sb_info* sbi, block_t pblkaddr)
{
	struct alfs_info* ri = ALFS_RI (sbi);

	if (sbi->ai == NULL)
		return -1;
	
	if (pblkaddr >= ri->metalog_blkofs &&
		pblkaddr < ri->metalog_blkofs + ri->nr_metalog_phys_blks)
		return 0;

	return -1;
}

int32_t get_metalog_free_blks (struct f2fs_sb_info* sbi)
{
	struct alfs_info* ri = ALFS_RI (sbi);
	uint32_t nr_free_blks;

	if (ri->metalog_gc_sblkofs < ri->metalog_gc_eblkofs) {
		nr_free_blks = ri->nr_metalog_phys_blks - ri->metalog_gc_eblkofs + ri->metalog_gc_sblkofs;
	} else if (ri->metalog_gc_sblkofs > ri->metalog_gc_eblkofs) {
		nr_free_blks = ri->metalog_gc_sblkofs - ri->metalog_gc_eblkofs;
	} else {
		f2fs_msg(sbi->sb, KERN_ERR, "[ERROR] 'ri->metalog_gc_sblkofs (%u)' is equal to 'ri->metalog_gc_eblkofs (%u)'", 
			ri->metalog_gc_sblkofs, ri->metalog_gc_eblkofs);
		nr_free_blks = -1;
	}

	return nr_free_blks;
}

int8_t is_gc_needed (struct f2fs_sb_info* sbi, int32_t nr_free_blks)
{
	struct alfs_info* ri = ALFS_RI (sbi);

	mutex_lock (&ri->alfs_gc_mutex); 
	if (nr_free_blks <= (sbi->segs_per_sec * sbi->blocks_per_seg)) {
		mutex_unlock (&ri->alfs_gc_mutex); 
		return 0;
	}

	mutex_unlock (&ri->alfs_gc_mutex); 
	return -1;
}

uint32_t alfs_get_mapped_pblkaddr (struct f2fs_sb_info* sbi, block_t lblkaddr)
{
	struct alfs_info* ri = ALFS_RI (sbi);
	block_t pblkaddr;
	block_t new_lblkaddr;

	/* see if ri is initialized or not */
	if (sbi->ai == NULL)
		return NULL_ADDR;

	/* get the physical blkaddr from the mapping table */
	new_lblkaddr = lblkaddr - ri->metalog_blkofs;
	pblkaddr = le32_to_cpu (ri->map_blks[new_lblkaddr/1020].mapping[new_lblkaddr%1020]);
	if (pblkaddr == -1)
		pblkaddr = 0;

	/* see if 'pblkaddr' is valid or not */
	if (is_valid_meta_pblkaddr (sbi, pblkaddr) == -1) {
		if (pblkaddr != NULL_ADDR) {
			f2fs_msg(sbi->sb, KERN_ERR, "invalid pblkaddr: (%llu (=%llu-%llu) => %u)", 
				(int64_t)lblkaddr - (int64_t)ri->metalog_blkofs,
				(int64_t)lblkaddr, 
				(int64_t)ri->metalog_blkofs, 
				pblkaddr);
		}
		return NULL_ADDR;
	}

	/* see if the summary table is correct or not */
	if (ri->summary_table[pblkaddr - ri->metalog_blkofs] == 0 ||
		ri->summary_table[pblkaddr - ri->metalog_blkofs] == 2) {
		f2fs_msg(sbi->sb, KERN_ERR, "the summary table is incorrect: pblkaddr=%u (%u)",
			pblkaddr, ri->summary_table[pblkaddr - ri->metalog_blkofs]);
	}

	return pblkaddr;
}

uint32_t alfs_get_new_pblkaddr (struct f2fs_sb_info* sbi, block_t lblkaddr, uint32_t length)
{
	struct alfs_info* ri = ALFS_RI (sbi);
	block_t pblkaddr = NULL_ADDR;

	/* see if ri is initialized or not */
	if (sbi->ai == NULL)
		return NULL_ADDR;

	/* have sufficent free blks - go ahead */
	if (ri->summary_table[ri->metalog_gc_eblkofs] == 0) {
		/* get the physical blkoff */
		pblkaddr = ri->metalog_blkofs + ri->metalog_gc_eblkofs;

		/* see if pblk is valid or not */
		if (is_valid_meta_pblkaddr (sbi, pblkaddr) == -1) {
			f2fs_msg(sbi->sb, KERN_ERR, "pblkaddr is invalid (%u)", pblkaddr);
			return NULL_ADDR;
		}
	} else {
		f2fs_msg(sbi->sb, KERN_ERR, "metalog_gc_eblkofs is NOT free: summary_table[%u] = %u",
			ri->metalog_gc_eblkofs, ri->summary_table[ri->metalog_gc_eblkofs]);
		return NULL_ADDR;
	}

	return pblkaddr;
}

int8_t alfs_map_l2p (struct f2fs_sb_info* sbi, block_t lblkaddr, block_t pblkaddr, uint32_t length)
{
	struct alfs_info* ri = ALFS_RI (sbi);
	block_t cur_lblkaddr = lblkaddr;
	block_t cur_pblkaddr = pblkaddr;
	block_t new_lblkaddr;
	uint32_t loop = 0;

	/* see if ri is initialized or not */
	if (sbi->ai == NULL)
		return -1;

	/* see if pblkaddr is valid or not */
	if (pblkaddr == NULL_ADDR)
		return -1;

	for (loop = 0; loop < length; loop++) {
		block_t prev_pblkaddr = NULL_ADDR;

		/* see if cur_lblkaddr is valid or not */
		if (is_valid_meta_lblkaddr (sbi, cur_lblkaddr) == -1) {
			f2fs_msg(sbi->sb, KERN_ERR, "is_valid_meta_lblkaddr is failed (cur_lblkaddr: %u)", cur_lblkaddr);
			return -1;
		}

		/* get the new pblkaddr */
		if (cur_pblkaddr == NULL_ADDR) {
			if ((cur_pblkaddr = alfs_get_new_pblkaddr (sbi, cur_lblkaddr, length)) == NULL_ADDR) {
				f2fs_msg(sbi->sb, KERN_ERR, "cannot get the new free block (cur_lblkaddr: %u)", cur_lblkaddr);
				return -1;
			} 
		}

		/* get the old pblkaddr */
		new_lblkaddr = cur_lblkaddr - ri->metalog_blkofs;
		prev_pblkaddr = le32_to_cpu (ri->map_blks[new_lblkaddr/1020].mapping[new_lblkaddr%1020]);
		if (prev_pblkaddr == -1)
			prev_pblkaddr = 0;

		/* see if 'prev_pblkaddr' is valid or not */
		if (is_valid_meta_pblkaddr (sbi, prev_pblkaddr) == 0) {
			/* make the entry of the summary table invalid */
			ri->summary_table[prev_pblkaddr - ri->metalog_blkofs] = 2;	/* set to invalid */

			/* trim */
			if (alfs_do_trim (sbi, prev_pblkaddr, 1) == -1) {
				f2fs_msg(sbi->sb, KERN_ERR, KERN_INFO "Errors occur while trimming the page during alfs_map_l2p");
			}
		} else if (prev_pblkaddr != NULL_ADDR) {
			f2fs_msg(sbi->sb, KERN_ERR, "invalid prev_pblkaddr = %llu", (int64_t)prev_pblkaddr);
		} else {
			/* it is porible that 'prev_pblkaddr' is invalid */
		}

		/* update the mapping & summary table */
		new_lblkaddr = cur_lblkaddr - ri->metalog_blkofs;
		ri->map_blks[new_lblkaddr/1020].mapping[new_lblkaddr%1020] = cpu_to_le32 (cur_pblkaddr);
		ri->map_blks[new_lblkaddr/1020].dirty = 1;

		ri->summary_table[cur_pblkaddr - ri->metalog_blkofs] = 1; /* set to valid */

		/* adjust end_blkofs in the meta-log */
		ri->metalog_gc_eblkofs = (ri->metalog_gc_eblkofs + 1) % (ri->nr_metalog_phys_blks);

		/* go to the next logical blkaddr */
		cur_lblkaddr++;
		cur_pblkaddr = NULL_ADDR;
	}

	return 0;
}

int8_t alfs_do_trim (struct f2fs_sb_info* sbi, block_t pblkaddr, uint32_t nr_blks)
{
	if (test_opt (sbi, DISCARD)) {
		blkdev_issue_discard (
			sbi->sb->s_bdev, 
			SECTOR_FROM_BLOCK (pblkaddr), 
			nr_blks * 8, 
			GFP_NOFS, 
			0);
		return 0;
	}
	return -1;
}

int8_t alfs_do_gc (struct f2fs_sb_info* sbi)
{
	struct alfs_info* ri = ALFS_RI (sbi);
	struct page* page = NULL;

	uint32_t cur_blkofs = 0;
	uint32_t loop = 0;

	mutex_lock (&ri->alfs_gc_mutex); 

	/* see if ri is initialized or not */
	if (sbi->ai == NULL) {
		mutex_unlock(&ri->alfs_gc_mutex); 
		return -1;
	}

	/* check the alignment */
	if (ri->metalog_gc_sblkofs % (sbi->segs_per_sec * sbi->blocks_per_seg) != 0) {
		f2fs_msg(sbi->sb, KERN_ERR, "ri->metalog_gc_sblkofs %% sbi->blocks_per_seg != 0 (%u)", 
			ri->metalog_gc_sblkofs % (sbi->segs_per_sec * sbi->blocks_per_seg));
		mutex_unlock(&ri->alfs_gc_mutex); 
		return -1;
	}

	/* read all valid blks in the victim segment */
	for (cur_blkofs = ri->metalog_gc_sblkofs; 
		 cur_blkofs < ri->metalog_gc_sblkofs + (sbi->segs_per_sec * sbi->blocks_per_seg); 
		 cur_blkofs++) 
	{
		uint8_t is_mapped = 0;
		uint32_t src_pblkaddr;
		uint32_t dst_pblkaddr;

		/* see if the block is valid or not */
		if (ri->summary_table[cur_blkofs] == 0 || ri->summary_table[cur_blkofs] == 2) {
			/* go to the next blks */
			ri->summary_table[cur_blkofs] = cpu_to_le32 (0); /* set to free */

			continue;
		}

		/* allocate a new page (This page is released later by 'alfs_end_io_flash' */
		page = alloc_page (GFP_NOFS | __GFP_ZERO);
		if (IS_ERR (page)) {
			f2fs_msg(sbi->sb, KERN_INFO, "page is invalid");
			mutex_unlock(&ri->alfs_gc_mutex); 
			return PTR_ERR (page);
		}
		lock_page (page);

		/* determine src & dst blks */
		src_pblkaddr = ri->metalog_blkofs + cur_blkofs;
		dst_pblkaddr = ri->metalog_blkofs + ri->metalog_gc_eblkofs;

		/* read the valid blk into the page */
		if (alfs_readpage (sbi, page, src_pblkaddr) == -1) {
			f2fs_msg(sbi->sb, KERN_ERR, "[ERROR] errors occur while reading the page during GC");
			continue;
		}

		/* write the valid pages into the free segment */
		if (alfs_writepage (sbi, page, dst_pblkaddr, 0) == -1) {
			f2fs_msg(sbi->sb, KERN_ERR, "[ERROR] errors occur while writing the page during GC");
			continue;
		}

		/* trim the source page */
		if (alfs_do_trim (sbi, src_pblkaddr, 1) == -1) {
			f2fs_msg(sbi->sb, KERN_ERR, "[ERROR] errors occur while trimming the page during GC");
			continue;
		}

		/* update mapping table */
		for (loop = 0; loop < ri->nr_metalog_logi_blks; loop++) {
			if (ri->map_blks[loop/1020].mapping[loop%1020] == src_pblkaddr) {
				ri->map_blks[loop/1020].mapping[loop%1020] = dst_pblkaddr;
				ri->map_blks[loop/1020].dirty = 1;
				is_mapped = 1;
				break;
			}
		}

		if (is_mapped != 1) {
			f2fs_msg(sbi->sb, KERN_ERR, "[ERROR] cannot find a mapped physical blk");
		}

		/* update summary table */
		ri->summary_table[src_pblkaddr - ri->metalog_blkofs] = cpu_to_le32 (0); /* set to free */
		ri->summary_table[dst_pblkaddr - ri->metalog_blkofs] = cpu_to_le32 (1); /* see to valid */

		/* update end offset */
		ri->metalog_gc_eblkofs = (ri->metalog_gc_eblkofs + 1) % ri->nr_metalog_phys_blks;

	}
	if (alfs_do_trim (sbi, ri->metalog_blkofs , ri->blks_per_sec) == -1) {
		f2fs_msg(sbi->sb, KERN_ERR, "Errors occur while trimming the page during GC");
	}

	if (alfs_do_trim (sbi, ri->metalog_blkofs , ri->blks_per_sec) == -1) {
		f2fs_msg(sbi->sb, KERN_ERR, "Errors occur while trimming the page during GC");
	}

	/* update start offset */
	ri->metalog_gc_sblkofs = 
		(ri->metalog_gc_sblkofs + (sbi->segs_per_sec * sbi->blocks_per_seg)) % 
		ri->nr_metalog_phys_blks;

	mutex_unlock (&ri->alfs_gc_mutex); 

	return 0;
}

void alfs_submit_bio_w (struct f2fs_sb_info* sbi, struct bio* bio, uint8_t sync)
{
	struct page* src_page = NULL;
	struct page* dst_page = NULL;
	struct bio_vec *bvec = NULL;

	uint8_t* src_page_addr = NULL;
	uint8_t* dst_page_addr = NULL;
	uint32_t bioloop = 0;
	int8_t ret = 0;

	bio_for_each_segment_all(bvec, bio, bioloop) {
		uint32_t pblkaddr = NULL_ADDR;
		uint32_t lblkaddr = NULL_ADDR;

		/* allocate a new page (This page is released later by 'alfs_end_io_flash' */
		dst_page = alloc_page (GFP_KERNEL | __GFP_ZERO);
		if (IS_ERR (dst_page)) {
			f2fs_msg(sbi->sb, KERN_ERR, "Errors occur while allocating page");
			bio->bi_end_io (bio);
			return;
		}
		lock_page (dst_page);

		/* check error cases */
		if (bvec == NULL || bvec->bv_len == 0 || bvec->bv_page == NULL)  {
			f2fs_msg(sbi->sb, KERN_ERR, "bvec is wrong");
			break;
		}

		/* get the soruce page */
		src_page = bvec->bv_page;

		/* get the new pblkaddr */
		spin_lock (&sbi->mapping_lock);
		lblkaddr = src_page->index;
		pblkaddr = alfs_get_new_pblkaddr (sbi, lblkaddr, 1);
		if (pblkaddr == NULL_ADDR) {
			spin_unlock (&sbi->mapping_lock);
			f2fs_msg(sbi->sb, KERN_ERR, "alfs_get_new_pblkaddr failed");
			ret = -1;
			goto out;
		}
		/* update mapping table */
		if (alfs_map_l2p (sbi, lblkaddr, pblkaddr, 1) != 0) {
			spin_unlock (&sbi->mapping_lock);
			f2fs_msg(sbi->sb, KERN_ERR, "alfs_map_l2p failed");
			ret = -1;
			goto out;
		}
		spin_unlock (&sbi->mapping_lock);

		/* write the requested page */
		src_page_addr = (uint8_t*)page_address (src_page);
		dst_page_addr = (uint8_t*)page_address (dst_page);
		memcpy (dst_page_addr, src_page_addr, PAGE_SIZE);

		if (alfs_writepage_flash (sbi, dst_page, pblkaddr, sync) != 0) {
			f2fs_msg(sbi->sb, KERN_ERR, "alfs_writepage_flash failed");
			ret = -1;
			goto out;
		}
	}

out:
	if (ret == 0) {
		// BIO_UPTODATE MACRO CONSTANT HAD BEEN REMOVED ON 4.8.1
		//set_bit (BIO_UPTODATE, &bio->bi_flags);
		bio->bi_end_io (bio);
	} else {
		bio->bi_end_io (bio);
	}
}


void alfs_submit_merged_bio_w (struct f2fs_sb_info* sbi, struct bio* bio, uint8_t sync)
{
	struct page* src_page = NULL;
	struct page* dst_page = NULL;
	struct bio_vec *bvec = NULL;
	struct bio* new_bio = NULL;
	uint8_t* src_page_addr = NULL;
	uint8_t* dst_page_addr = NULL;
	uint32_t bioloop = 0;
	int8_t ret = 0;

	new_bio = get_new_bio(sbi, bio->bi_iter.bi_size/4096);

	bio_for_each_segment_all(bvec, bio, bioloop) {
		uint32_t pblkaddr = NULL_ADDR;
		uint32_t lblkaddr = NULL_ADDR;
		/* check error cases */
		if (bvec == NULL || bvec->bv_len == 0 || bvec->bv_page == NULL)  {
			f2fs_msg(sbi->sb, KERN_ERR, "bvec is wrong");
			goto out;
		}

		/* get the soruce page */
		src_page = bvec->bv_page;

		/* get the new pblkaddr */
		spin_lock (&sbi->mapping_lock);

		lblkaddr = src_page->index;
		pblkaddr = alfs_get_new_pblkaddr (sbi, lblkaddr, 1);
		if (pblkaddr == NULL_ADDR) {
			spin_unlock (&sbi->mapping_lock);
			f2fs_msg(sbi->sb, KERN_ERR, "alfs_get_new_pblkaddr failed");
			ret = -1;
			goto out;
		}
		/* update mapping table */
		if (alfs_map_l2p (sbi, lblkaddr, pblkaddr, 1) != 0) {
			spin_unlock (&sbi->mapping_lock);
			f2fs_msg(sbi->sb, KERN_ERR, "alfs_map_l2p failed");
			ret = -1;
			goto out;
		}
		spin_unlock (&sbi->mapping_lock);
		/* allocate a new page (This page is released later by 'alfs_end_io_flash' */
		dst_page = alloc_page (GFP_NOFS | __GFP_ZERO);
		if (IS_ERR (dst_page)) {
			f2fs_msg(sbi->sb, KERN_ERR, "Errors occur while allocating page");
			goto out;
		}
		lock_page (dst_page);

		/* write the requested page */
		src_page_addr = (uint8_t*)page_address (src_page);
		dst_page_addr = (uint8_t*)page_address (dst_page);

		memcpy (dst_page_addr, src_page_addr, PAGE_SIZE);

		if (bioloop == 0 )
		{
			new_bio->bi_iter.bi_sector = SECTOR_FROM_BLOCK (pblkaddr);
		}
		/* put a page into a new_bio queue */
		if (bio_add_page (new_bio, dst_page, PAGE_SIZE, 0) < PAGE_SIZE) {
			f2fs_msg(sbi->sb, KERN_ERR, "Error occur while calling alfs_write_bio_flash");
			kfree (new_bio->bi_private);
			bio_put (new_bio);
			goto out;
		}
	}
	if (alfs_write_bio_flash (sbi, new_bio, sync) != 0) {
		f2fs_msg(sbi->sb, KERN_ERR, "alfs_write_bio_flash failed");
		ret = -1;
		goto out;
	}
out:
	if (ret == 0) {
		// BIO_UPTODATE MACRO CONSTANT HAD BEEN REMOVED ON 4.8.1
		//set_bit (BIO_UPTODATE, &bio->bi_flags);
		bio->bi_end_io (bio);
	} else {
		bio->bi_end_io (bio);
	}
}



void alfs_submit_bio_r (struct f2fs_sb_info* sbi, struct bio* bio)
{
	struct page* src_page = NULL;
	struct page* dst_page = NULL;
	struct bio_vec *bvec = NULL;

	uint8_t* src_page_addr = NULL;
	uint8_t* dst_page_addr = NULL;
	uint32_t bioloop = 0;
	int8_t ret = 0;

	src_page = alloc_page (GFP_NOFS | __GFP_ZERO);
	if (IS_ERR (src_page)) {
		f2fs_msg(sbi->sb, KERN_ERR, "Errors occur while allocating page");
		bio->bi_end_io (bio);
		return;
	}
	lock_page (src_page);

	bio_for_each_segment_all(bvec, bio, bioloop) {
	//bio_for_each_segment (bvec, bio, bioloop) {
		uint32_t pblkaddr = NULL_ADDR;
		uint32_t lblkaddr = NULL_ADDR;

		/* check error cases */
		if (bvec == NULL || bvec->bv_len == 0 || bvec->bv_page == NULL)  {
			f2fs_msg(sbi->sb, KERN_ERR, "bvec is wrong");
			ret = -1;
			break;
		}

		/* get a destination page */
		dst_page = bvec->bv_page;

		/* get a mapped phyiscal page */
		lblkaddr = dst_page->index;
		pblkaddr = alfs_get_mapped_pblkaddr (sbi, lblkaddr);
		if (pblkaddr == NULL_ADDR) {
			ret = -1;
			goto out;
		}

		/* read the requested page */
		if (alfs_readpage_flash (sbi, src_page, pblkaddr) != 0) {
			f2fs_msg(sbi->sb, KERN_ERR, "alfs_readpage_flash failed");
			ret = -1;
			goto out;
		}

		/* copy memory data */
		src_page_addr = (uint8_t*)page_address (src_page);
		dst_page_addr = (uint8_t*)page_address (dst_page);
		memcpy (dst_page_addr, src_page_addr, PAGE_SIZE);

		/* go to the next page */
		ClearPageUptodate (src_page);
	}

out:
	/* unlock & free the page */
	unlock_page (src_page);
	__free_pages (src_page, 0);

	if (ret == 0) {
		// BIO_UPTODATE MACRO CONSTANT HAD BEEN REMOVED ON 4.8.1
		//set_bit (BIO_UPTODATE, &bio->bi_flags);
		bio->bi_end_io (bio);
	} else {
		bio->bi_end_io (bio);
	}
}

static uint8_t alfs_is_cp_blk (struct f2fs_sb_info* sbi, block_t lblkaddr)
{
	block_t start_addr =
		le32_to_cpu(F2FS_RAW_SUPER(sbi)->cp_blkaddr);

	if (lblkaddr == start_addr)
		return 1;
	else if (lblkaddr == (start_addr + sbi->blocks_per_seg))
		return 1;

	return 0;
}

void alfs_submit_bio (struct f2fs_sb_info* sbi, int rw, struct bio * bio, uint8_t sync)
{
	block_t lblkaddr = bio->bi_iter.bi_sector * 512 / 4096;

	if (alfs_is_cp_blk (sbi, lblkaddr)) {
		alfs_write_mapping_entries (sbi);
	}

	if (is_valid_meta_lblkaddr (sbi, lblkaddr) == 0) {
		/* if WRITE then */
		if (bio_op(bio) == REQ_OP_WRITE) {
			alfs_submit_bio_w (sbi, bio, sync);
		} else if (bio_op(bio) == READ || bio_op(bio) == REQ_RAHEAD) {
			alfs_submit_bio_r (sbi, bio);
		} else {
			f2fs_msg(sbi->sb, KERN_ERR, "[WARNING] unknown type: rw %d", rw);
			f2fs_msg(sbi->sb, KERN_ERR, "[WARNING] unknown type: bio_io%d", bio_op(bio));
		}
	} else {
		submit_bio (bio);
	}
}

void alfs_submit_merged_bio (struct f2fs_sb_info* sbi, int rw, struct bio * bio, uint8_t sync)
{
	block_t lblkaddr = bio->bi_iter.bi_sector * 512 / 4096;

	if (alfs_is_cp_blk (sbi, lblkaddr)) {
		alfs_write_mapping_entries (sbi);
	}


	if (is_valid_meta_lblkaddr (sbi, lblkaddr) == 0) {
		/* if WRITE then */
		if (rw == 1) {
			alfs_submit_merged_bio_w (sbi, bio, sync);
		} else if (rw != 1) {
			alfs_submit_bio_r (sbi, bio);
		} else {
			f2fs_msg(sbi->sb, KERN_ERR, "[WARNING] unknown type: rw %d", rw);
			f2fs_msg(sbi->sb, KERN_ERR, "[WARNING] unknown type: bio_io%d", bio_op(bio));
			submit_bio (bio);
		}
	} else {
		submit_bio (bio);
	}
}