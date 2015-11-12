/*
 * Kernel Device Mapper for abstracting ZAC/ZBC devices as normal
 * block devices for linux file systems.
 *
 * Copyright (C) 2015 Seagate Technology PLC
 *
 * Written by:
 * Shaun Tancheff <shaun.tancheff@seagate.com>
 *
 * This file is licensed under  the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef _DM_ZONED_H
#define _DM_ZONED_H

#define ZDM_IOC_MZCOUNT		0x5a4e0001
#define ZDM_IOC_WPS		0x5a4e0002
#define ZDM_IOC_FREE		0x5a4e0003
#define ZDM_IOC_STATUS		0x5a4e0004

#define DM_MSG_PREFIX		"zoned"

#define ZDM_RESERVED_ZNR	0
#define ZDM_CRC_STASH_ZNR	1 /* first 64 blocks */
#define ZDM_RMAP_ZONE		2
#define ZDM_SECTOR_MAP_ZNR	3
#define ZDM_DATA_START_ZNR	4

#define Z_WP_GC_FULL		(1u << 31)
#define Z_WP_GC_ACTIVE		(1u << 30)
#define Z_WP_GC_TARGET		(1u << 29)
#define Z_WP_GC_READY		(1u << 28)
#define Z_WP_GC_BITS		(0xFu << 28)

#define Z_WP_GC_PENDING		(Z_WP_GC_FULL|Z_WP_GC_ACTIVE)
#define Z_WP_NON_SEQ		(1u << 27)
#define Z_WP_RESV_01		(1u << 26)
#define Z_WP_RESV_02		(1u << 25)
#define Z_WP_RESV_03		(1u << 24)

#define Z_WP_VALUE_MASK		(~0u >> 8)
#define Z_WP_FLAGS_MASK		(~0u << 24)

#define Z_AQ_GC			(1u << 31)
#define Z_AQ_META		(1u << 30)
#define Z_AQ_NORMAL		(0)

#define Z_C4K			(4096ul)
#define Z_UNSORTED		(Z_C4K / sizeof(struct map_sect_to_lba))
#define Z_BLOCKS_PER_DM_SECTOR	(Z_C4K/512)
#define MZ_METADATA_ZONES	(8ul)

#define LBA_SB_START		1

#define SUPERBLOCK_LOCATION	0
#define SUPERBLOCK_MAGIC	0x5a6f4e65ul	/* ZoNe */
#define SUPERBLOCK_CSUM_XOR	146538381
#define MIN_ZONED_VERSION	1
#define Z_VERSION		1
#define MAX_ZONED_VERSION	1
#define INVALID_WRITESET_ROOT	SUPERBLOCK_LOCATION

#define UUID_LEN		16

#define Z_TYPE_SMR		2
#define Z_TYPE_SMR_HA		1
#define Z_VPD_INFO_BYTE		8

#define MAX_CACHE_INCR		320ul
#define CACHE_COPIES		3
#define MAX_MZ_SUPP		64

#define IO_VCACHE_ORDER         8
#define IO_VCACHE_PAGES        (1 << IO_VCACHE_ORDER)  /* 256 pages => 1MiB */

#ifdef __cplusplus
extern "C" {
#endif


enum superblock_flags_t {
	SB_DIRTY = 1,
};

struct z_io_req_t {
	struct dm_io_region *where;
	struct dm_io_request *io_req;
	struct work_struct work;
	int result;
};

#define Z_LOWER48 (~0ul >> 16)
#define Z_UPPER16 (~Z_LOWER48)

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

enum pg_flag_enum {
	IS_DIRTY,
	IS_GC,
	IS_STALE,
};

enum work_flags_enum {
	DO_JOURNAL_MOVE,
	DO_MEMPOOL,
	DO_SYNC,
	DO_JOURNAL_LOAD,
	DO_META_CHECK,
	DO_GC_NO_PURGE,
	DO_METAWORK_QD,
};

enum gc_flags_enum {
	DO_GC_NEW,
	DO_GC_PREPARE,		/* -> READ or COMPLETE state */
	DO_GC_WRITE,
	DO_GC_META,		/* -> PREPARE state */
	DO_GC_COMPLETE,
};

enum znd_flags_enum {
	ZF_FREEZE,
	ZF_POOL_FWD,
	ZF_POOL_REV,
	ZF_POOL_CRCS,
};

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

struct zoned;

/**
 * struct gc_state - A page of map table
 * @megaz:
 * @gc_flags:
 * @r_ptr:
 * @w_ptr:
 * @nblks:
 * @result:
 * @z_gc:
 * @tag:
 *
 * Longer description of this structure.
 */
struct gc_state {
	struct megazone *megaz;
	unsigned long gc_flags;

	u32 r_ptr;
	u32 w_ptr;

	u32 nblks;		/* 1-65536 */
	int result;

	u16 z_gc;
	u16 tag;
};

/**
 * struct map_sect_to_lba - Sector to LBA mapping.
 * @logical:
 * @physical:
 *
 * Longer description of this structure.
 */
struct map_sect_to_lba {
	__le64 logical;		/* record type [16 bits] + logical sector # */
	__le64 physical;	/* csum 16 [16 bits] + 'physical' block lba */
} __packed;

/**
 * struct map_pg - A page of map table
 * @inpool:
 * @age:
 * @lba:
 * @flags:
 * @md_lock:
 * @mdata:
 * @refcount:
 * @last_write:
 *
 * Longer description of this structure.
 */
struct map_pg {
	__le32 *mdata;		/* 4k page of table entries */
	atomic_t refcount;
	u64 age;		/* most recent access in jiffies */
	u64 lba;		/* Z_LOWER48 contains the BLOCK where this
				 * data originates from .. */
	struct mutex md_lock;	/* lock mdata i/o */
	u64 last_write;		/* last known position on disk */
	unsigned long flags;    /* is dirty flag */
	struct list_head inpool;
};

/**
 * struct map_addr - A page of map table
 * @dm_s:
 * @zone_id:
 * @crc: CRC Entry within the CRC PG
 * @mz_id:
 * @offentry:
 * @lut_s:
 * @lut_r:
 *
 * Longer description of this structure.
 */
struct map_addr {
	u64 dm_s;		/* full map on dm layer         */
	u64 zone_id;		/* z_id match zone_list_t.z_id  */
	u64 crc;		/* megazone offset              */
	u64 mz_id;		/* mega zone #                  */
	u64 offentry;		/* entry in lut (0-1023)        */
	u64 lut_s;		/* sector table lba  */
	u64 lut_r;		/* reverse table lba */
};

/**
 * struct mzlam - A page of map table
 * @mz_base:
 * @r_base:
 * @s_base:
 * @sk_low:
 * @sk_high:
 * @crc_low:
 * @crc_hi:
 *
 * Longer description of this structure.
 */
 struct mzlam {
	u64 mz_base;
	u64 r_base;
	u64 s_base;
	u64 sk_low;
	u64 sk_high;
	u64 crc_low;
	u64 crc_hi;
};

/**
 * struct map_cache - A page of map table
 * @jlist:
 * @jdata:
 * @refcount:
 * @cached_lock:
 * @no_sort_flag:
 * @jcount:
 * @jsorted:
 * @jsize:
 *
 * Longer description of this structure.
 */
struct map_cache {
	struct list_head jlist;
	struct map_sect_to_lba *jdata;	/* 4k page of data */
	atomic_t refcount;
	struct mutex cached_lock;
	unsigned long no_sort_flag;
	u32 jcount;
	u32 jsorted;
	u32 jsize;
};

/**
 * struct crc_pg - A page of map table
 * @age:
 * @lba:
 * @flags:
 * @refcount:
 * @last_write:
 * @lock_pg:
 * @cdata:
 *
 * Longer description of this structure.
 */
struct crc_pg {
	u64 age;		/* most recent access in jiffies */
	u64 lba;		/* logical home */
	unsigned long flags;	/* IS_DIRTY flag */
	atomic_t refcount;	/* REF count (move to flags?) */
	u64 last_write;
	struct mutex lock_pg;
	__le16 *cdata;		/* attached 4K page: [2048] entries */
};

#define MZKY_NBLKS  64
#define MZKY_NCRC   32

/**
 * struct zdm_superblock - A page of map table
 * @uuid:
 * @nr_zones:
 * @magic:
 * @zdstart:
 * @version:
 * @packed_meta:
 * @flags:
 * @csum:
 *
 * Longer description of this structure.
 */
struct zdm_superblock {
	u8 uuid[UUID_LEN];	/* 16 */
	__le64 nr_zones;	/*  8 */
	__le64 magic;		/*  8 */
	__le64 zdstart;		/*  8 */
	__le32 version;		/*  4 */
	__le32 packed_meta;	/*  4 */
	__le32 flags;		/*  4 */
	__le32 csum;		/*  4 */
} __packed;			/* 56 */

#define MAX_CACHE_SYNC 400

/**
 * struct mz_superkey - A page of map table
 * @sig0:           8 - Native endian
 * @sig1:           8 - Little endian
 * @stm_keys:     512 - LBA64 for each key page of the Sector Table
 * @stm_crc_lba:  256 - LBA64 for each CRC page
 * @stm_crc_pg:    64 - CRC16 for each CRC page
 * @rtm_crc_lba:  256 - LBA64 for each CRC page
 * @rtm_crc_pg:    64 - CRC16 for each CRC page
 * @crcs:         816 - Testing worst case so far - 142 entries.
 * @reserved:    2040 -
 * @gc_resv:        8 -
 * @meta_resv:      8 -
 * @n_crcs:         2 -
 * @zp_crc:         2 -
 * @free_crc:       2 -
 * @sblock:        56 -
 * @generation:     8 -
 * @key_crc:        2 -
 * @magic:          8 -
 *
 * Longer description of this structure.
 */
struct mz_superkey {
	u64 sig0;
	__le64 sig1;
	__le64 stm_keys[MZKY_NBLKS];
	__le64 stm_crc_lba[MZKY_NCRC];
	__le16 stm_crc_pg[MZKY_NCRC];
	__le64 rtm_crc_lba[MZKY_NCRC];
	__le16 rtm_crc_pg[MZKY_NCRC];
	__le16 crcs[MAX_CACHE_SYNC];
	u16 reserved[1020];
	u32 gc_resv;
	u32 meta_resv;
	__le16 n_crcs;
	__le16 zp_crc;
	__le16 free_crc;
	struct zdm_superblock sblock;
	__le64 generation;
	__le16 key_crc;
	__le64 magic;
} __packed;

/**
 * struct mz_state - Sector to LBA mapping.
 * @bmkeys:
 * @z_ptrs:
 * @zfree:
 *
 * Longer description of this structure.
 */
struct mz_state {
	struct mz_superkey bmkeys;
	u32                z_ptrs[1024];  /* future: __le32 */
	u32                zfree[1024];   /* future: __le32 */
} __packed;

/**
 * struct io_4k_block - Sector to LBA mapping.
 * @data:	A 4096 byte block
 *
 * Longer description of this structure.
 */
struct io_4k_block {
	u8 data[Z_C4K];
};

/**
 * struct io_dm_block - Sector to LBA mapping.
 * @data:	A 512 byte block
 *
 * Longer description of this structure.
 */
struct io_dm_block {
	u8 data[512];
};

/**
 * struct megazone - A page of map table
 * @flags:
 * @znd:
 * @jlist:
 * @smtpool:
 * @sectortm:
 * @reversetm:
 * @sync_io:
 * @z_ptrs:
 * @zfree_count:
 * @z_commit:
 * @bmkeys:
 * @logical_map:
 * @stm_crc:
 * @rtm_crc:
 * @meta_work:
 * @last_w:
 * @cow_block:
 * @cow_addr:
 * @mapkey_lock:
 * @mz_io_mutex:
 * @zp_lock:
 * @jlock:
 * @map_pool_lock:
 * @discard_lock:
 * @age:
 * @mega_nr:
 * @z_count:
 * @z_gc_free:
 * @z_data:
 * @z_current:
 * @z_gc_resv:
 * @z_meta_resv:
 * @incore_count:
 * @discard_count:
 * @mc_entries:
 * @meta_result:
 *
 * Longer description of this structure.
 */
struct megazone {
	unsigned long flags;
	struct zoned *znd;
	struct list_head jlist;		/* journal */
	struct list_head smtpool;	/* in-use sm table  entries */
	struct map_pg **sectortm;
	struct map_pg **reversetm;
	struct mz_state *sync_io;
	u32 *z_ptrs;
	u32 *zfree_count;
	u32  z_commit[1024];
	struct mz_superkey *bmkeys;
	struct mzlam       logical_map;
	struct crc_pg stm_crc[MZKY_NCRC];
	struct crc_pg rtm_crc[MZKY_NCRC];
	struct work_struct meta_work;
	sector_t last_w;
	u8 *cow_block;
	u64 cow_addr;
	struct mutex mapkey_lock;	/* for normal i/o */
	struct mutex mz_io_mutex;	/* for normal i/o */
	struct mutex zp_lock;		/* general lock (block acquire)  */
	spinlock_t jlock;		/* journal lock */
	spinlock_t map_pool_lock;	/* smtpool: memory pool lock */
	struct mutex discard_lock;
	u64 age;			/* most recent access in jiffies */
	u32 mega_nr;
	u32 z_count;			/* megazone data span: 4-1024 */
	u32 z_gc_free;			/* current empty zone count */
	u32 z_data;			/* Range: 2->1023 */
	u32 z_current;			/* Range: 2->1023 */
	u32 z_gc_resv;
	u32 z_meta_resv;
	s32 incore_count;
	u32 discard_count;
	int mc_entries;
	int meta_result;
};

/*
 *
 * Partition -----------------------------------------------------------------+
 *     Table ---+                                                             |
 *              |                                                             |
 *   SMR Drive |^-------------------------------------------------------------^|
 * CMR Zones     ^^^^^^^^^
 *  meta data    |||||||||
 *
 *   Remaining partition is filesystem data
 *
 */

/**
 * struct zoned - A page of map table
 * @ti:
 * @callbacks:
 * @dev:
 * @bg_work:
 * @bg_wq:
 * @stats_lock:
 * @gc_active:
 * @gc_lock:
 * @gc_work:
 * @gc_wq:
 * @data_zones:
 * @mz_count:
 * @nr_blocks:
 * @pool_lba:		LBA at start of metadata pool
 * @data_lba:		LBA at start of data pool
 * @zdstart:		ZONE # at start of data pool (first Possible WP ZONE)
 * @start_sect:
 * @flags:		See: enum znd_flags_enum
 * @gc_backlog:
 * @gc_io_buf:
 * @io_vcache[32]:
 * @io_vcache_flags:
 * @z_superblock:
 * @super_block:
 * @z_mega:
 * @meta_wq:
 * @gc_postmap:
 * @io_client:
 * @io_wq:
 * @timer:
 * @bins:
 * @bdev_name:
 * @memstat:
 * @suspended:
 * @gc_mz_pref:
 * @mz_provision:
 * @zinqtype:
 * @ata_passthrough:
 * @is_empty:
 * @gc_throttle:
 *
 * Longer description of this structure.
 */
struct zoned {
	struct dm_target *ti;
	struct dm_target_callbacks callbacks;
	struct dm_dev *dev;
	/* background activity: */
	struct work_struct bg_work;
	struct workqueue_struct *bg_wq;
	spinlock_t stats_lock;

	/* zoned gc: */
	struct gc_state *gc_active;
	spinlock_t gc_lock;
	struct delayed_work gc_work;
	struct workqueue_struct *gc_wq;

	u64 data_zones;		/* # of data zones on device */
	u64 mz_count;		/* # of 256G mega-zones */
	u64 nr_blocks;		/* 4k blocks on backing device */
	u64 pool_lba;
	u64 data_lba;

	/* first run of metadata starts at zdstart.
	 * second run of metadata starts at the following zone boundary
	 * metadata runs upto start_sect */

	u64 zdstart; /* where ZDM data starts (zone#) */
	u64 start_sect;   /* where ZDM partition starts (RAW LBA) */
	unsigned long flags;

	int gc_backlog;
	void *gc_io_buf;
	struct io_4k_block *io_vcache[32];
	unsigned long io_vcache_flags;

	/* superblock: */
	void *z_superblock;
	struct zdm_superblock *super_block;

	/* array of mega-zones: */
	struct megazone *z_mega;
	struct workqueue_struct *meta_wq;

	struct map_cache gc_postmap;

	struct dm_io_client *io_client;
	struct workqueue_struct *io_wq;
	struct timer_list timer;

	u32 bins[40];
	char bdev_name[BDEVNAME_SIZE];

	size_t memstat;
	atomic_t suspended;
	atomic_t gc_throttle;
	u16 gc_mz_pref;
	u16 mz_provision;
	u8 zinqtype;
	u8 ata_passthrough;
	u8 is_empty; /* for fast discards on initial format */
};

/**
 * struct zdm_ioc_request - Sector to LBA mapping.
 * @result_size:
 * @megazone_nr:
 *
 * Longer description of this structure.
 */
struct zdm_ioc_request {
	u32 result_size;
	u32 megazone_nr;
};

/**
 * struct zdm_ioc_status - Sector to LBA mapping.
 * @b_used:
 * @b_available:
 * @b_discard:
 * @m_used:
 * @mc_entries:
 * @mlut_blocks:
 * @crc_blocks:
 * @memstat:
 * @bins:
 *
 * Longer description of this structure.
 */
struct zdm_ioc_status {
	u64 b_used;
	u64 b_available;
	u64 b_discard;
	u64 m_used;
	u64 mc_entries;
	u64 mlut_blocks;
	u64 crc_blocks;
	u64 memstat;
	u32 bins[40];
};

#ifdef __cplusplus
}
#endif

#endif /* _DM_ZONED_H */

