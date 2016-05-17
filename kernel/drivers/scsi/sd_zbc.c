/*
 * sd_zbc.c - SCSI Zoned Block commands
 *
 * Copyright (C) 2014-2015 SUSE Linux GmbH
 * Written by: Hannes Reinecke <hare@suse.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139,
 * USA.
 *
 */

#include <linux/blkdev.h>
#include <linux/rbtree.h>

#include <asm/unaligned.h>

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_driver.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_eh.h>

#include "sd.h"
#include "scsi_priv.h"

enum zbc_zone_cond {
	ZBC_ZONE_COND_NO_WP,
	ZBC_ZONE_COND_EMPTY,
	ZBC_ZONE_COND_IMPLICIT_OPEN,
	ZBC_ZONE_COND_EXPLICIT_OPEN,
	ZBC_ZONE_COND_CLOSED,
	ZBC_ZONE_COND_READONLY = 0xd,
	ZBC_ZONE_COND_FULL,
	ZBC_ZONE_COND_OFFLINE,
};

#define SD_ZBC_BUF_SIZE 524288

#define sd_zbc_debug(sdkp, fmt, args...)				\
	pr_err  ("%s %s [%s]: " fmt,					\
		 dev_driver_string(&(sdkp)->device->sdev_gendev),	\
		 dev_name(&(sdkp)->device->sdev_gendev),		\
		 (sdkp)->disk->disk_name, ## args)

#define sd_zbc_debug_ratelimit(sdkp, fmt, args...)		\
	do {							\
		if (printk_ratelimit())				\
			sd_zbc_debug(sdkp, fmt, ## args);	\
	} while( 0 )

struct zbc_update_work {
	struct work_struct zone_work;
	struct scsi_disk *sdkp;
	spinlock_t	zone_lock;
	sector_t	zone_sector;
	int		zone_buflen;
	char		zone_buf[0];
};

static
struct blk_zone *zbc_desc_to_zone(struct scsi_disk *sdkp, unsigned char *rec)
{
	struct blk_zone *zone;
	enum zbc_zone_cond zone_cond;
	sector_t wp = (sector_t)-1;

	zone = kzalloc(sizeof(struct blk_zone), GFP_KERNEL);
	if (!zone)
		return NULL;

	spin_lock_init(&zone->lock);
	zone->type = rec[0] & 0xf;
	zone_cond = (rec[1] >> 4) & 0xf;
	zone->len = logical_to_sectors(sdkp->device,
				       get_unaligned_be64(&rec[8]));
	zone->start = logical_to_sectors(sdkp->device,
					 get_unaligned_be64(&rec[16]));

	if (blk_zone_is_smr(zone)) {
		wp = logical_to_sectors(sdkp->device,
					get_unaligned_be64(&rec[24]));
		if (zone_cond == ZBC_ZONE_COND_READONLY) {
			zone->state = BLK_ZONE_READONLY;
		} else if (zone_cond == ZBC_ZONE_COND_OFFLINE) {
			zone->state = BLK_ZONE_OFFLINE;
		} else {
			zone->state = BLK_ZONE_OPEN;
		}
	} else
		zone->state = BLK_ZONE_NO_WP;

	zone->wp = wp;
	/*
	 * Fixup block zone state
	 */
	if (zone_cond == ZBC_ZONE_COND_EMPTY &&
	    zone->wp != zone->start) {
		sd_zbc_debug(sdkp,
			     "zone %zx state EMPTY wp %zx: adjust wp\n",
			     zone->start, zone->wp);
		zone->wp = zone->start;
	}
	if (zone_cond == ZBC_ZONE_COND_FULL &&
	    zone->wp != zone->start + zone->len) {
		sd_zbc_debug(sdkp,
			     "zone %zx state FULL wp %zx: adjust wp\n",
			     zone->start, zone->wp);
		zone->wp = zone->start + zone->len;
	}

	return zone;
}

static
sector_t zbc_parse_zones(struct scsi_disk *sdkp, u64 zlen, unsigned char *buf,
			 unsigned int buf_len)
{
	struct request_queue *q = sdkp->disk->queue;
	unsigned char *rec = buf;
	int rec_no = 0;
	unsigned int list_length;
	sector_t next_sector = -1;
	u8 same;

	/* Parse REPORT ZONES header */
	list_length = get_unaligned_be32(&buf[0]);
	same = buf[4] & 0xf;
	rec = buf + 64;
	list_length += 64;

	if (list_length < buf_len)
		buf_len = list_length;

	while (rec < buf + buf_len) {
		struct blk_zone *this, *old;
		unsigned long flags;

		this = zbc_desc_to_zone(sdkp, rec);
		if (!this)
			break;

		if (same == 0 && this->len != zlen) {
			next_sector = this->start + this->len;
			break;
		}

		next_sector = this->start + this->len;
		old = blk_insert_zone(q, this);
		if (old) {
			spin_lock_irqsave(&old->lock, flags);
			if (blk_zone_is_smr(old)) {
				old->wp = this->wp;
				old->state = this->state;
			}
			spin_unlock_irqrestore(&old->lock, flags);
			kfree(this);
		}
		rec += 64;
		rec_no++;
	}

	sd_zbc_debug(sdkp,
		     "Inserted %d zones, next sector %zx len %d\n",
		     rec_no, next_sector, list_length);

	return next_sector;
}

static void sd_zbc_refresh_zone_work(struct work_struct *work)
{
	struct zbc_update_work *zbc_work =
		container_of(work, struct zbc_update_work, zone_work);
	struct scsi_disk *sdkp = zbc_work->sdkp;
	struct request_queue *q = sdkp->disk->queue;
	unsigned char *zone_buf = zbc_work->zone_buf;
	unsigned int zone_buflen = zbc_work->zone_buflen;
	int ret;
	u8 same;
	u64 zlen = 0;
	sector_t last_sector;
	sector_t capacity = logical_to_sectors(sdkp->device, sdkp->capacity);

	ret = sd_zbc_report_zones(sdkp, zone_buf, zone_buflen,
				  zbc_work->zone_sector,
				  ZBC_ZONE_REPORTING_OPTION_ALL, true);
	if (ret)
		goto done_free;

	/* this whole path is unlikely so extra reports shouldn't be a
	 * large impact */
	same = zone_buf[4] & 0xf;
	if (same == 0) {
		unsigned char *desc = &zone_buf[64];
		unsigned int blen = zone_buflen;

		/* just pull the first zone */
		if (blen > 512)
			blen = 512;
		ret = sd_zbc_report_zones(sdkp, zone_buf, blen, 0,
					  ZBC_ZONE_REPORTING_OPTION_ALL, true);
		if (ret)
			goto done_free;

		/* Read the zone length from the first zone descriptor */
		zlen = logical_to_sectors(sdkp->device,
					  get_unaligned_be64(&desc[8]));

		ret = sd_zbc_report_zones(sdkp, zone_buf, zone_buflen,
					  zbc_work->zone_sector,
					  ZBC_ZONE_REPORTING_OPTION_ALL, true);
		if (ret)
			goto done_free;
	}

	last_sector = zbc_parse_zones(sdkp, zlen, zone_buf, zone_buflen);
	capacity = logical_to_sectors(sdkp->device, sdkp->capacity);
	if (last_sector != -1 && last_sector < capacity) {
		if (test_bit(SD_ZBC_ZONE_RESET, &sdkp->zone_flags)) {
			sd_zbc_debug(sdkp,
				     "zones in reset, canceling refresh\n");
			ret = -EAGAIN;
			goto done_free;
		}

		zbc_work->zone_sector = last_sector;
		queue_work(sdkp->zone_work_q, &zbc_work->zone_work);
		/* Kick request queue to be on the safe side */
		goto done_start_queue;
	}
done_free:
	kfree(zbc_work);
	if (test_and_clear_bit(SD_ZBC_ZONE_INIT, &sdkp->zone_flags) && ret) {
		sd_zbc_debug(sdkp,
			     "Canceling zone initialization\n");
	}
done_start_queue:
	if (q->mq_ops)
		blk_mq_start_hw_queues(q);
	else {
		unsigned long flags;

		spin_lock_irqsave(q->queue_lock, flags);
		blk_start_queue(q);
		spin_unlock_irqrestore(q->queue_lock, flags);
	}
}

/**
 * sd_zbc_update_zones - Update zone information for @sector
 * @sdkp: SCSI disk for which the zone information needs to be updated
 * @sector: sector to be updated
 * @bufsize: buffersize to be allocated
 * @reason: non-zero if existing zones should be updated
 */
void sd_zbc_update_zones(struct scsi_disk *sdkp, sector_t sector, int bufsize,
			 int reason)
{
	struct request_queue *q = sdkp->disk->queue;
	struct zbc_update_work *zbc_work;
	struct blk_zone *zone;
	struct rb_node *node;
	int zone_num = 0, zone_busy = 0, num_rec;
	sector_t next_sector = sector;

	if (test_bit(SD_ZBC_ZONE_RESET, &sdkp->zone_flags)) {
		sd_zbc_debug(sdkp,
			     "zones in reset, not starting reason\n");
		return;
	}

	if (reason != SD_ZBC_INIT) {
		/* lookup sector, is zone pref? then ignore */
		struct blk_zone *zone = blk_lookup_zone(q, sector);

		if (reason == SD_ZBC_RESET_WP)
			sd_zbc_debug(sdkp, "RESET WP failed %lx\n", sector);

		if (zone && zone->type == BLK_ZONE_TYPE_SEQWRITE_PREF)
			return;
	}

retry:
	zbc_work = kzalloc(sizeof(struct zbc_update_work) + bufsize,
			   GFP_KERNEL);
	if (!zbc_work) {
		if (bufsize > 512) {
			sd_zbc_debug(sdkp,
				     "retry with buffer size %d\n", bufsize);
			bufsize = bufsize >> 1;
			goto retry;
		}
		sd_zbc_debug(sdkp,
			     "failed to allocate %d bytes\n", bufsize);
		if (reason == SD_ZBC_INIT)
			clear_bit(SD_ZBC_ZONE_INIT, &sdkp->zone_flags);
		return;
	}
	zbc_work->zone_sector = sector;
	zbc_work->zone_buflen = bufsize;
	zbc_work->sdkp = sdkp;
	INIT_WORK(&zbc_work->zone_work, sd_zbc_refresh_zone_work);
	num_rec = (bufsize / 64) - 1;

	/*
	 * Mark zones under update as BUSY
	 */
	if (reason != SD_ZBC_INIT) {
		for (node = rb_first(&q->zones); node; node = rb_next(node)) {
			unsigned long flags;

			zone = rb_entry(node, struct blk_zone, node);
			if (num_rec == 0)
				break;
			if (zone->start != next_sector)
				continue;
			next_sector += zone->len;
			num_rec--;

			spin_lock_irqsave(&zone->lock, flags);
			if (blk_zone_is_smr(zone)) {


				if (zone->state == BLK_ZONE_BUSY) {
					zone_busy++;
				} else {
					zone->state = BLK_ZONE_BUSY;
					zone->wp = zone->start;
				}
				zone_num++;
			}
			spin_unlock_irqrestore(&zone->lock, flags);
		}
		if (zone_num && (zone_num == zone_busy)) {
			sd_zbc_debug(sdkp,
				     "zone update for %zx in progress\n",
				     sector);
			kfree(zbc_work);
			return;
		}
	}

	if (!queue_work(sdkp->zone_work_q, &zbc_work->zone_work)) {
		sd_zbc_debug(sdkp,
			     "zone update already queued?\n");
		kfree(zbc_work);
	}
}

/**
 * sd_zbc_report_zones - Issue a REPORT ZONES scsi command
 * @sdkp: SCSI disk to which the command should be send
 * @buffer: response buffer
 * @bufflen: length of @buffer
 * @start_sector: logical sector for the zone information should be reported
 * @option: reporting option to be used
 * @partial: flag to set the 'partial' bit for report zones command
 */
int sd_zbc_report_zones(struct scsi_disk *sdkp, unsigned char *buffer,
			int bufflen, sector_t start_sector,
			enum zbc_zone_reporting_options option, bool partial)
{
	struct scsi_device *sdp = sdkp->device;
	const int timeout = sdp->request_queue->rq_timeout
			* SD_FLUSH_TIMEOUT_MULTIPLIER;
	struct scsi_sense_hdr sshdr;
	sector_t start_lba = sectors_to_logical(sdkp->device, start_sector);
	unsigned char cmd[16];
	int result;

	if (!scsi_device_online(sdp))
		return -ENODEV;

	sd_zbc_debug(sdkp, "REPORT ZONES lba %zx len %d\n", start_lba, bufflen);

	memset(cmd, 0, 16);
	cmd[0] = ZBC_IN;
	cmd[1] = ZI_REPORT_ZONES;
	put_unaligned_be64(start_lba, &cmd[2]);
	put_unaligned_be32(bufflen, &cmd[10]);
	cmd[14] = (partial ? ZBC_REPORT_ZONE_PARTIAL : 0) | option;
	memset(buffer, 0, bufflen);

	result = scsi_execute_req(sdp, cmd, DMA_FROM_DEVICE,
				buffer, bufflen, &sshdr,
				timeout, SD_MAX_RETRIES, NULL);

	if (result) {
		sd_zbc_debug(sdkp,
			     "REPORT ZONES lba %zx failed with %d/%d\n",
			     start_lba, host_byte(result), driver_byte(result));
		return -EIO;
	}

	return 0;
}

/**
 * sd_zbc_lookup_zone - Test if request if valid for containing zone.
 * @sdkp: SCSI disk to which the command should be send
 * @rq: Active io request to test.
 * @sector: Sector of io start
 * @num_sectors: Number of sectors of io to consider
 */
int sd_zbc_lookup_zone(struct scsi_disk *sdkp, struct request *rq,
		       sector_t sector, unsigned int num_sectors)
{
	struct request_queue *q = sdkp->disk->queue;
	struct blk_zone *zone = NULL;
	int ret = BLKPREP_OK;
	unsigned long flags;

	zone = blk_lookup_zone(q, sector);
	/* Might happen during zone initialization */
	if (!zone) {
		if (sdkp->zoned != 1)
			sd_zbc_debug_ratelimit(sdkp,
				"zone for sector %zx not found, skipping\n",
				sector);
		return BLKPREP_OK;
	}
	spin_lock_irqsave(&zone->lock, flags);
	if (zone->state == BLK_ZONE_UNKNOWN ||
	    zone->state == BLK_ZONE_BUSY) {
		sd_zbc_debug_ratelimit(sdkp,
				       "zone %zx state %x, deferring\n",
				       zone->start, zone->state);
		ret = BLKPREP_DEFER;
	} else if (zone->state == BLK_ZONE_OFFLINE) {
		/* let the drive fail the command */
		goto out;
	} else {
		if (rq->cmd_flags & (REQ_WRITE | REQ_WRITE_SAME)) {
			if (zone->type == BLK_ZONE_TYPE_SEQWRITE_PREF)
				zone->wp += num_sectors;
			if (zone->type != BLK_ZONE_TYPE_SEQWRITE_REQ)
				goto out;
			if (zone->state == BLK_ZONE_READONLY)
				goto out;
			if (blk_zone_is_full(zone)) {
				sd_zbc_debug(sdkp,
					     "Write to full zone %zx/%zx\n",
					     sector, zone->wp);
				ret = BLKPREP_KILL;
				goto out;
			}
			if (zone->wp != sector) {
				sd_zbc_debug(sdkp,
					     "Misaligned write %zx/%zx\n",
					     sector, zone->wp);
				ret = BLKPREP_KILL;
				goto out;
			}
			zone->wp += num_sectors;
		} else if (blk_zone_is_seq_req(zone) && (zone->wp <= sector)) {
			sd_zbc_debug(sdkp,
				     "Read beyond wp %zx/%zx\n",
				     sector, zone->wp);
			if (zone->type == BLK_ZONE_TYPE_SEQWRITE_REQ)
				ret = BLKPREP_DONE;
		}
	}
out:
	spin_unlock_irqrestore(&zone->lock, flags);

	return ret;
}

/**
 * sd_zbc_setup - Load zones of matching zlen size into rb tree.
 *
 */
int sd_zbc_setup(struct scsi_disk *sdkp, u64 zlen, char *buf, int buf_len)
{
	sector_t capacity = logical_to_sectors(sdkp->device, sdkp->capacity);
	sector_t last_sector;

	if (test_and_set_bit(SD_ZBC_ZONE_INIT, &sdkp->zone_flags)) {
		sdev_printk(KERN_WARNING, sdkp->device,
			    "zone initialization already running\n");
		return 0;
	}

	if (!sdkp->zone_work_q) {
		char wq_name[32];

		sprintf(wq_name, "zbc_wq_%s", sdkp->disk->disk_name);
		sdkp->zone_work_q = create_singlethread_workqueue(wq_name);
		if (!sdkp->zone_work_q) {
			sdev_printk(KERN_WARNING, sdkp->device,
				    "create zoned disk workqueue failed\n");
			return -ENOMEM;
		}
	} else if (!test_and_set_bit(SD_ZBC_ZONE_RESET, &sdkp->zone_flags)) {
		drain_workqueue(sdkp->zone_work_q);
		clear_bit(SD_ZBC_ZONE_RESET, &sdkp->zone_flags);
	}

	last_sector = zbc_parse_zones(sdkp, zlen, buf, buf_len);
	capacity = logical_to_sectors(sdkp->device, sdkp->capacity);
	if (last_sector != -1 && last_sector < capacity) {
		sd_zbc_update_zones(sdkp, last_sector,
				    SD_ZBC_BUF_SIZE, SD_ZBC_INIT);
	} else
		clear_bit(SD_ZBC_ZONE_INIT, &sdkp->zone_flags);

	return 0;
}

/**
 * sd_zbc_remove -
 */
void sd_zbc_remove(struct scsi_disk *sdkp)
{
	if (sdkp->zone_work_q) {
		if (!test_and_set_bit(SD_ZBC_ZONE_RESET, &sdkp->zone_flags))
			drain_workqueue(sdkp->zone_work_q);
		clear_bit(SD_ZBC_ZONE_INIT, &sdkp->zone_flags);
		destroy_workqueue(sdkp->zone_work_q);
	}
}
/**
 * sd_zbc_discard_granularity - Determine discard granularity.
 * @sdkp: SCSI disk used to calculate discard granularity.
 *
 * Discard granularity should match the (maximum non-CMR) zone
 * size reported on the drive.
 */
unsigned int sd_zbc_discard_granularity(struct scsi_disk *sdkp)
{
	unsigned int bytes = 1;
	struct request_queue *q = sdkp->disk->queue;
	struct rb_node *node = rb_first(&q->zones);

	if (node) {
		struct blk_zone *zone = rb_entry(node, struct blk_zone, node);

		bytes = zone->len;
	}
	bytes <<= ilog2(sdkp->device->sector_size);
	return bytes;
}
