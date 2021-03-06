/**
 * f2fs_format.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 *
 * Dual licensed under the GPL or LGPL version 2 licenses.
 */
#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#ifndef ANDROID_WINDOWS_HOST
#include <sys/stat.h>
#include <sys/mount.h>
#endif
#include <time.h>
#include <uuid/uuid.h>

#define ENABLE_DBG_LOG
#define AMF_SNAPSHOT
#define AMF_META_LOGGING

#ifdef AMF_SNAPSHOT
#include <liblightnvm.h>
#endif


#include "f2fs_fs.h"
#include "quota.h"
#include "f2fs_format_utils.h"
#include "config.h"




#ifdef ENABLE_DBG_LOG
#define dbg_log(fmt, ...)	\
	do {	\
		printf(fmt, ##__VA_ARGS__);	\
	} while (0);
#else
	#define dbg_log(fmt, ...)
#endif

#if defined(AMF_SNAPSHOT) && defined(AMF_META_LOGGING)
#define NR_SUPERBLK_SECS	1	/* # of sections for the super block */
#define NR_MAPPING_SECS		3 	/* # of sections for mapping entries */
#define NR_METALOG_TIMES	2	/* # of sections for meta-log */
unsigned long long _mapping_blkofs = 0, _mapping_byteofs = 0;
unsigned long long _meta_log_blkofs = 0, _meta_log_blkofs_2 = 0;

struct amf_map_blk {
	__le32 magic;
	__le32 ver;
	__le32 index;
	__le32 dirty;
	__le32 mapping[F2FS_BLKSIZE/sizeof(__le32)-4];
};


static char nvm_dev_path[20] = "/dev/nvme0n1";
static struct nvm_dev *nvmdev;
static struct nvm_geo *nvmgeo;
unsigned int NVM_GEO_MIN_WRITE_PAGE = 0;
u64  nvm_head_pg = 0;//按道理，f2fs只有super_block会写入0，而super_block貌似不是用nvm写入的
char * nvm_buf_w = NULL;
size_t nvm_buf_w_nbytes = 0;
unsigned int nvm_count_write_pg = 0;
struct nvm_addr *nvm_write_addrs;
int pmode;	//NVM_PLANE_SINGLE

bool isCover(u64 cur){
	if(nvm_head_pg/NVM_GEO_MIN_WRITE_PAGE == cur/NVM_GEO_MIN_WRITE_PAGE)
		return true;
	return false;
}


//由于所有都是写入1个addr，所以naddrs没有考虑
int nvm_ocssd_cmd_write(struct nvm_dev *dev, u64 lblkaddr, int naddrs, const void *data, struct nvm_ret *ret)
{ int i;
	if(nvm_buf_w == NULL){
		nvm_buf_w = nvm_buf_alloc(dev, nvm_buf_w_nbytes, NULL);
		memset(nvm_buf_w, 0x00, nvm_buf_w_nbytes);
		if(!nvm_buf_w){
			dbg_log("nvm_buf_alloc() failed\n");
			return 1;
		}

	}
	if(!isCover(lblkaddr) || nvm_count_write_pg >= NVM_GEO_MIN_WRITE_PAGE || naddrs==0){//下刷，并重新分配buf
		if(nvm_count_write_pg != 0){//buf_w中有数据，应该下刷		
			for(i = 0; i < NVM_GEO_MIN_WRITE_PAGE; i++){
			nvm_write_addrs[i] = nvm_addr_dev2gen(dev, nvm_head_pg + i);
			}
			//dbg_log("nvm_write_addrs[0].ppa = 0x%llx\n",nvm_write_addrs[0].ppa);
			int res = nvm_cmd_write(dev, nvm_write_addrs, NVM_GEO_MIN_WRITE_PAGE, nvm_buf_w, NULL, pmode, ret);
			if(res < 0){
				dbg_log("nvm_cmd_write failure\n");
				return 1;
			}
		
			//nvm_buf_w置位
			memset(nvm_buf_w, 0x00, nvm_buf_w_nbytes);
			nvm_head_pg = 0;
			nvm_count_write_pg = 0;	
		}		
	}

	if(naddrs == 0)
		return 0;
				
	if(nvm_head_pg == 0 && nvm_count_write_pg == 0){
		nvm_head_pg = lblkaddr/NVM_GEO_MIN_WRITE_PAGE * NVM_GEO_MIN_WRITE_PAGE;
	}

	memcpy(nvm_buf_w + (lblkaddr % NVM_GEO_MIN_WRITE_PAGE) * nvmgeo->sector_nbytes, data, 4096);	
	nvm_count_write_pg++;
	
	return 0;
}




#endif


/*end*/


extern struct f2fs_configuration c;
struct f2fs_super_block raw_sb;
struct f2fs_super_block *sb = &raw_sb;
struct f2fs_checkpoint *cp;

/* Return first segment number of each area */
#define prev_zone(cur)		(c.cur_seg[cur] - c.segs_per_zone)
#define next_zone(cur)		(c.cur_seg[cur] + c.segs_per_zone)
#define last_zone(cur)		((cur - 1) * c.segs_per_zone)
#define last_section(cur)	(cur + (c.secs_per_zone - 1) * c.segs_per_sec)

static unsigned int quotatype_bits = 0;

const char *media_ext_lists[] = {
	"jpg",
	"gif",
	"png",
	"avi",
	"divx",
	"mp4",
	"mp3",
	"3gp",
	"wmv",
	"wma",
	"mpeg",
	"mkv",
	"mov",
	"asx",
	"asf",
	"wmx",
	"svi",
	"wvx",
	"wm",
	"mpg",
	"mpe",
	"rm",
	"ogg",
	"jpeg",
	"video",
	"apk",	/* for android system */
	"so",	/* for android system */
	NULL
};

const char *hot_ext_lists[] = {
	"db",
	NULL
};

const char **default_ext_list[] = {
	media_ext_lists,
	hot_ext_lists
};

static bool is_extension_exist(const char *name)
{
	int i;

	for (i = 0; i < F2FS_MAX_EXTENSION; i++) {
		char *ext = (char *)sb->extension_list[i];
		if (!strcmp(ext, name))
			return 1;
	}

	return 0;
}

static void cure_extension_list(void)
{
	const char **extlist;
	char *ext_str;
	char *ue;
	int name_len;
	int i, pos = 0;

	set_sb(extension_count, 0);
	memset(sb->extension_list, 0, sizeof(sb->extension_list));

	for (i = 0; i < 2; i++) {
		ext_str = c.extension_list[i];
		extlist = default_ext_list[i];

		while (*extlist) {
			name_len = strlen(*extlist);
			memcpy(sb->extension_list[pos++], *extlist, name_len);
			extlist++;
		}
		if (i == 0)
			set_sb(extension_count, pos);
		else
			sb->hot_ext_count = pos - get_sb(extension_count);;

		if (!ext_str)
			continue;

		/* add user ext list */
		ue = strtok(ext_str, ", ");
		while (ue != NULL) {
			name_len = strlen(ue);
			if (name_len >= 8) {
				MSG(0, "\tWarn: Extension name (%s) is too long\n", ue);
				goto next;
			}
			if (!is_extension_exist(ue))
				memcpy(sb->extension_list[pos++], ue, name_len);
next:
			ue = strtok(NULL, ", ");
			if (pos >= F2FS_MAX_EXTENSION)
				break;
		}

		if (i == 0)
			set_sb(extension_count, pos);
		else
			sb->hot_ext_count = pos - get_sb(extension_count);

		free(c.extension_list[i]);
	}
}

static void verify_cur_segs(void)
{
	int i, j;
	int reorder = 0;

	for (i = 0; i < NR_CURSEG_TYPE; i++) {
		for (j = i + 1; j < NR_CURSEG_TYPE; j++) {
			if (c.cur_seg[i] == c.cur_seg[j]) {
				reorder = 1;
				break;
			}
		}
	}

	if (!reorder)
		return;

	c.cur_seg[0] = 0;
	for (i = 1; i < NR_CURSEG_TYPE; i++)
		c.cur_seg[i] = next_zone(i - 1);
}

static int f2fs_prepare_super_block(void)
{
	u_int32_t blk_size_bytes;
	u_int32_t log_sectorsize, log_sectors_per_block;
	u_int32_t log_blocksize, log_blks_per_seg;
	u_int32_t segment_size_bytes, zone_size_bytes;
	u_int32_t sit_segments, nat_segments;
	u_int32_t blocks_for_sit, blocks_for_nat, blocks_for_ssa;
	u_int32_t total_valid_blks_available;
	u_int64_t zone_align_start_offset, diff;
	u_int64_t total_meta_zones, total_meta_segments;
	u_int32_t sit_bitmap_size, max_sit_bitmap_size;
	u_int32_t max_nat_bitmap_size, max_nat_segments;
	u_int32_t total_zones;
	enum quota_type qtype;
	int i;

#ifdef AMF_SNAPSHOT
		u_int32_t nr_meta_logging_segments = 0;
		u_int32_t nr_meta_logging_blks = 0;
		u_int64_t zone_align_start_offset2;
#endif



	set_sb(magic, F2FS_SUPER_MAGIC);
	set_sb(major_ver, F2FS_MAJOR_VERSION);
	set_sb(minor_ver, F2FS_MINOR_VERSION);
//printf("F2FS_MAJOR_VERSION = %d\n",F2FS_MAJOR_VERSION);
	log_sectorsize = log_base_2(c.sector_size);
	log_sectors_per_block = log_base_2(c.sectors_per_blk);
	log_blocksize = log_sectorsize + log_sectors_per_block;
	log_blks_per_seg = log_base_2(c.blks_per_seg);

	set_sb(log_sectorsize, log_sectorsize);
	set_sb(log_sectors_per_block, log_sectors_per_block);

	set_sb(log_blocksize, log_blocksize);
	set_sb(log_blocks_per_seg, log_blks_per_seg);

	set_sb(segs_per_sec, c.segs_per_sec);
	set_sb(secs_per_zone, c.secs_per_zone);

	blk_size_bytes = 1 << log_blocksize;
	segment_size_bytes = blk_size_bytes * c.blks_per_seg;
	zone_size_bytes =
		blk_size_bytes * c.secs_per_zone *
		c.segs_per_sec * c.blks_per_seg;

	set_sb(checksum_offset, 0);

	set_sb(block_count, c.total_sectors >> log_sectors_per_block);
#ifdef AMF_SNAPSHOT	
	dbg_log("config.start_sector: %u\n"
				 "DEFAULT_SECTOR_SIZE: %u\n"
				 "F2FS_BLKSIZE: %u\n"
				 "segment_size_bytes: %u\n"
				 "zone_size_bytes: %u\n\n",
				 c.start_sector,
				 DEFAULT_SECTOR_SIZE,
				 F2FS_BLKSIZE,
				 segment_size_bytes,
				 zone_size_bytes);//0, 512, 4096,  2097152 = 512*4096, 2097152

	zone_align_start_offset =
		(c.start_sector * c.sector_size +
		2 * F2FS_BLKSIZE + zone_size_bytes - 1) /
		zone_size_bytes * zone_size_bytes -
		c.start_sector * c.sector_size;

	dbg_log("segment0_blkaddr.org: %u\n",
			cpu_to_le32(zone_align_start_offset / blk_size_bytes));//512

	
	zone_align_start_offset =
			segment_size_bytes * cpu_to_le32(c.segs_per_sec) * 
			(NR_SUPERBLK_SECS + NR_MAPPING_SECS);	/* snapshot region */
#else

	zone_align_start_offset =
		(c.start_sector * c.sector_size +
		2 * F2FS_BLKSIZE + zone_size_bytes - 1) /
		zone_size_bytes * zone_size_bytes -
		c.start_sector * c.sector_size;
#endif

	if (c.start_sector % c.sectors_per_blk) {
		MSG(1, "\t%s: Align start sector number to the page unit\n",
				c.zoned_mode ? "FAIL" : "WARN");
		MSG(1, "\ti.e., start sector: %d, ofs:%d (sects/page: %d)\n",
				c.start_sector,
				c.start_sector % c.sectors_per_blk,
				c.sectors_per_blk);
		if (c.zoned_mode)
			return -1;
	}


	set_sb(segment0_blkaddr, zone_align_start_offset / blk_size_bytes);

	sb->cp_blkaddr = sb->segment0_blkaddr;

	MSG(0, "Info: zone aligned segment0 blkaddr: %u\n",
					get_sb(segment0_blkaddr));

	if (c.zoned_mode && (get_sb(segment0_blkaddr) + c.start_sector /
					c.sectors_per_blk) % c.zone_blocks) {
		MSG(1, "\tError: Unaligned segment0 block address %u\n",
				get_sb(segment0_blkaddr));
		return -1;
	}

	for (i = 0; i < c.ndevs; i++) {
		if (i == 0) {
			c.devices[i].total_segments =
				(c.devices[i].total_sectors *
				c.sector_size - zone_align_start_offset) /
				segment_size_bytes;
			c.devices[i].start_blkaddr = 0;
			c.devices[i].end_blkaddr = c.devices[i].total_segments *
						c.blks_per_seg - 1 +
						sb->segment0_blkaddr;
		} else {
			c.devices[i].total_segments =
				c.devices[i].total_sectors /
				(c.sectors_per_blk * c.blks_per_seg);
			c.devices[i].start_blkaddr =
					c.devices[i - 1].end_blkaddr + 1;
			c.devices[i].end_blkaddr = c.devices[i].start_blkaddr +
					c.devices[i].total_segments *
					c.blks_per_seg - 1;
		}
		if (c.ndevs > 1) {
			memcpy(sb->devs[i].path, c.devices[i].path, MAX_PATH_LEN);
			sb->devs[i].total_segments =
					cpu_to_le32(c.devices[i].total_segments);
		}

		c.total_segments += c.devices[i].total_segments;
	}
	set_sb(segment_count, (c.total_segments / c.segs_per_zone *
						c.segs_per_zone));
	set_sb(segment_count_ckpt, F2FS_NUMBER_OF_CHECKPOINT_PACK);

	set_sb(sit_blkaddr, get_sb(segment0_blkaddr) +
			get_sb(segment_count_ckpt) * c.blks_per_seg);

	blocks_for_sit = SIZE_ALIGN(get_sb(segment_count), SIT_ENTRY_PER_BLOCK);

	sit_segments = SEG_ALIGN(blocks_for_sit);

	set_sb(segment_count_sit, sit_segments * 2);

	set_sb(nat_blkaddr, get_sb(sit_blkaddr) + get_sb(segment_count_sit) *
			c.blks_per_seg);

	total_valid_blks_available = (get_sb(segment_count) -
			(get_sb(segment_count_ckpt) +
			get_sb(segment_count_sit))) * c.blks_per_seg;

	blocks_for_nat = SIZE_ALIGN(total_valid_blks_available,
			NAT_ENTRY_PER_BLOCK);

	if (c.large_nat_bitmap) {
		nat_segments = SEG_ALIGN(blocks_for_nat) *
						DEFAULT_NAT_ENTRY_RATIO / 100;
		set_sb(segment_count_nat, nat_segments ? nat_segments : 1);
		max_nat_bitmap_size = (get_sb(segment_count_nat) <<
						log_blks_per_seg) / 8;
		set_sb(segment_count_nat, get_sb(segment_count_nat) * 2);
	} else {
		set_sb(segment_count_nat, SEG_ALIGN(blocks_for_nat));
		max_nat_bitmap_size = 0;
	}

	/*
	 * The number of node segments should not be exceeded a "Threshold".
	 * This number resizes NAT bitmap area in a CP page.
	 * So the threshold is determined not to overflow one CP page
	 */
	sit_bitmap_size = ((get_sb(segment_count_sit) / 2) <<
				log_blks_per_seg) / 8;

	if (sit_bitmap_size > MAX_SIT_BITMAP_SIZE)
		max_sit_bitmap_size = MAX_SIT_BITMAP_SIZE;
	else
		max_sit_bitmap_size = sit_bitmap_size;

	if (c.large_nat_bitmap) {
		/* use cp_payload if free space of f2fs_checkpoint is not enough */
		if (max_sit_bitmap_size + max_nat_bitmap_size >
						MAX_BITMAP_SIZE_IN_CKPT) {
			u_int32_t diff =  max_sit_bitmap_size +
						max_nat_bitmap_size -
						MAX_BITMAP_SIZE_IN_CKPT;
			set_sb(cp_payload, F2FS_BLK_ALIGN(diff));
		} else {
			set_sb(cp_payload, 0);
		}
	} else {
		/*
		 * It should be reserved minimum 1 segment for nat.
		 * When sit is too large, we should expand cp area.
		 * It requires more pages for cp.
		 */
		if (max_sit_bitmap_size > MAX_SIT_BITMAP_SIZE_IN_CKPT) {
			max_nat_bitmap_size = CHECKSUM_OFFSET -
					sizeof(struct f2fs_checkpoint) + 1;
			set_sb(cp_payload, F2FS_BLK_ALIGN(max_sit_bitmap_size));
	        } else {
			max_nat_bitmap_size =
				CHECKSUM_OFFSET - sizeof(struct f2fs_checkpoint) + 1
				- max_sit_bitmap_size;
			set_sb(cp_payload, 0);
		}
		max_nat_segments = (max_nat_bitmap_size * 8) >> log_blks_per_seg;

		if (get_sb(segment_count_nat) > max_nat_segments)
			set_sb(segment_count_nat, max_nat_segments);

		set_sb(segment_count_nat, get_sb(segment_count_nat) * 2);
	}

	set_sb(ssa_blkaddr, get_sb(nat_blkaddr) + get_sb(segment_count_nat) *
			c.blks_per_seg);

	total_valid_blks_available = (get_sb(segment_count) -
			(get_sb(segment_count_ckpt) +
			get_sb(segment_count_sit) +
			get_sb(segment_count_nat))) *
			c.blks_per_seg;

	blocks_for_ssa = total_valid_blks_available /
				c.blks_per_seg + 1;

	set_sb(segment_count_ssa, SEG_ALIGN(blocks_for_ssa));
/* start */
#ifdef AMF_SNAPSHOT
	total_meta_segments = get_sb(segment_count_ckpt) +
		get_sb(segment_count_sit) +
		get_sb(segment_count_nat) +
		get_sb(segment_count_ssa);

	dbg_log ("total_meta_segments = %u (ckp:%u + sit:%u + nat:%u + ssa:%u)\n",
		total_meta_segments,
		le32_to_cpu(get_sb(segment_count_ckpt)),
		le32_to_cpu(get_sb(segment_count_sit)),
		le32_to_cpu(get_sb(segment_count_nat)),
		le32_to_cpu(get_sb(segment_count_ssa)));

	/* meta-log region */
	if (NR_METALOG_TIMES % 2 != 0) {
		dbg_log ("ERROR: NR_METALOG_TIMES must be even numbers = %u\n", NR_METALOG_TIMES);
		exit (-1);
	}
	

	nr_meta_logging_segments = total_meta_segments * (NR_METALOG_TIMES - 1);
	nr_meta_logging_blks = (nr_meta_logging_segments * c.blks_per_seg);	
	printf ("nr_meta_logging_segments: %u, nr_meta_logging_blks: %u (%u*%u = %u)\n", 
			nr_meta_logging_segments,
			nr_meta_logging_blks,
			nr_meta_logging_segments,
			c.blks_per_seg,
			nr_meta_logging_segments*c.blks_per_seg*4096);

	total_meta_segments = total_meta_segments*NR_METALOG_TIMES;

#else

	total_meta_segments = get_sb(segment_count_ckpt) +
		get_sb(segment_count_sit) +
		get_sb(segment_count_nat) +
		get_sb(segment_count_ssa);
#endif
/* end */

	diff = total_meta_segments % (c.segs_per_zone);
	if (diff)
		set_sb(segment_count_ssa, get_sb(segment_count_ssa) +
			(c.segs_per_zone - diff));
	total_meta_zones = ZONE_ALIGN(total_meta_segments *
						c.blks_per_seg);


	set_sb(main_blkaddr, get_sb(segment0_blkaddr) + total_meta_zones *
				c.segs_per_zone * c.blks_per_seg);

	if (c.zoned_mode) {
		/*
		 * Make sure there is enough randomly writeable
		 * space at the beginning of the disk.
		 */
		unsigned long main_blkzone = get_sb(main_blkaddr) / c.zone_blocks;

		if (c.devices[0].zoned_model == F2FS_ZONED_HM &&
				c.devices[0].nr_rnd_zones < main_blkzone) {
			MSG(0, "\tError: Device does not have enough random "
					"write zones for F2FS volume (%lu needed)\n",
					main_blkzone);
			return -1;
		}
	}

	total_zones = get_sb(segment_count) / (c.segs_per_zone) -
							total_meta_zones;

	set_sb(section_count, total_zones * c.secs_per_zone);

	set_sb(segment_count_main, get_sb(section_count) * c.segs_per_sec);


	/* Let's determine the best reserved and overprovisioned space */
	if (c.overprovision == 0)
		c.overprovision = get_best_overprovision(sb);

	if (c.overprovision == 0 || c.total_segments < F2FS_MIN_SEGMENTS ||
		(c.devices[0].total_sectors *
			c.sector_size < zone_align_start_offset) ||
		(get_sb(segment_count_main) - 2) < c.reserved_segments) {
		MSG(0, "\tError: Device size is not sufficient for F2FS volume\n");
		return -1;
	}

	c.reserved_segments =
			(2 * (100 / c.overprovision + 1) + 6)
			* c.segs_per_sec;

	uuid_generate(sb->uuid);

	/* precompute checksum seed for metadata */
	if (c.feature & cpu_to_le32(F2FS_FEATURE_INODE_CHKSUM))
		c.chksum_seed = f2fs_cal_crc32(~0, sb->uuid, sizeof(sb->uuid));

	utf8_to_utf16(sb->volume_name, (const char *)c.vol_label,
				MAX_VOLUME_NAME, strlen(c.vol_label));
	set_sb(node_ino, 1);
	set_sb(meta_ino, 2);
	set_sb(root_ino, 3);
	c.next_free_nid = 4;

	if (c.feature & cpu_to_le32(F2FS_FEATURE_QUOTA_INO)) {
		quotatype_bits = QUOTA_USR_BIT | QUOTA_GRP_BIT;
		if (c.feature & cpu_to_le32(F2FS_FEATURE_PRJQUOTA))
			quotatype_bits |= QUOTA_PRJ_BIT;
	}

	for (qtype = 0; qtype < F2FS_MAX_QUOTAS; qtype++) {
		if (!((1 << qtype) & quotatype_bits))
			continue;
		sb->qf_ino[qtype] = cpu_to_le32(c.next_free_nid++);
		MSG(0, "Info: add quota type = %u => %u\n",
					qtype, c.next_free_nid - 1);
	}

	if (c.feature & cpu_to_le32(F2FS_FEATURE_LOST_FOUND))
		c.lpf_ino = c.next_free_nid++;

	if (total_zones <= 6) {
		MSG(1, "\tError: %d zones: Need more zones "
			"by shrinking zone size\n", total_zones);
		return -1;
	}

	if (c.heap) {
		c.cur_seg[CURSEG_HOT_NODE] =
				last_section(last_zone(total_zones));
		c.cur_seg[CURSEG_WARM_NODE] = prev_zone(CURSEG_HOT_NODE);
		c.cur_seg[CURSEG_COLD_NODE] = prev_zone(CURSEG_WARM_NODE);
		c.cur_seg[CURSEG_HOT_DATA] = prev_zone(CURSEG_COLD_NODE);
		c.cur_seg[CURSEG_COLD_DATA] = 0;
		c.cur_seg[CURSEG_WARM_DATA] = next_zone(CURSEG_COLD_DATA);
	} else {
		c.cur_seg[CURSEG_HOT_NODE] = 0;
		c.cur_seg[CURSEG_WARM_NODE] = next_zone(CURSEG_HOT_NODE);
		c.cur_seg[CURSEG_COLD_NODE] = next_zone(CURSEG_WARM_NODE);
		c.cur_seg[CURSEG_HOT_DATA] = next_zone(CURSEG_COLD_NODE);
		c.cur_seg[CURSEG_COLD_DATA] =
				max(last_zone((total_zones >> 2)),
					next_zone(CURSEG_COLD_NODE));
		c.cur_seg[CURSEG_WARM_DATA] =
				max(last_zone((total_zones >> 1)),
					next_zone(CURSEG_COLD_DATA));
	}

#ifdef AMF_SNAPSHOT
		dbg_log ("config.cur_seg[CURSEG_HOT_NODE](%u) = (total_zones(%u) - 1) * config.segs_per_sec(%u) * config.secs_per_zone(%u) + "
				 "((config.secs_per_zone(%u) - 1) * config.segs_per_sec(%u))\n",
				c.cur_seg[CURSEG_HOT_NODE],
				total_zones, 
				c.segs_per_sec, 
				c.secs_per_zone,
				c.secs_per_zone,
				c.segs_per_sec);
#endif

	

	/* if there is redundancy, reassign it */
	verify_cur_segs();

	cure_extension_list();

	/* get kernel version */
	if (c.kd >= 0) {
		dev_read_version(c.version, 0, VERSION_LEN);
		get_kernel_version(c.version);
		MSG(0, "Info: format version with\n  \"%s\"\n", c.version);
	} else {
		get_kernel_uname_version(c.version);
	}

	memcpy(sb->version, c.version, VERSION_LEN);
	memcpy(sb->init_version, c.version, VERSION_LEN);

	sb->feature = c.feature;

	return 0;
}

static int f2fs_init_sit_area(void)
{
	u_int32_t blk_size, seg_size;
	u_int32_t index = 0;
	u_int64_t sit_seg_addr = 0;
	u_int8_t *zero_buf = NULL;

	blk_size = 1 << get_sb(log_blocksize);
	seg_size = (1 << get_sb(log_blocks_per_seg)) * blk_size;

	zero_buf = calloc(sizeof(u_int8_t), seg_size);
	if(zero_buf == NULL) {
		MSG(1, "\tError: Calloc Failed for sit_zero_buf!!!\n");
		return -1;
	}

	sit_seg_addr = get_sb(sit_blkaddr);
	sit_seg_addr *= blk_size;

	DBG(1, "\tFilling sit area at offset 0x%08"PRIx64"\n", sit_seg_addr);
#ifdef AMF_SNAPSHOT
		dbg_log ("\n1.----\n");
		dbg_log ("sit_start_addr: super_block.sit_blkaddr: %lu (%lu)\n", 
				le32_to_cpu(get_sb(sit_blkaddr)), 
				le32_to_cpu(sit_seg_addr));
	
		dbg_log ("sit_length(fill zeros): super_block.segment_count_sit: %d (%llu)\n", 
				get_sb(segment_count_sit),
				get_sb(segment_count_sit) * seg_size);
		
#endif

	for (index = 0; index < (get_sb(segment_count_sit) / 2); index++) {
#ifdef AMF_SNAPSHOT
		dbg_log("sit_seg_addr_blk = %u\n", sit_seg_addr/blk_size);
		struct nvm_ret ret;
		struct nvm_addr tempaddr = nvm_addr_dev2gen(nvmdev, sit_seg_addr);
		if(nvm_cmd_erase(nvmdev, &tempaddr, 1,NULL, 1, &ret)){
			MSG(1, "\tError: While zeroing out the sit area "
					"on disk!!!\n");
			free(zero_buf);
			return -1;
		}
	
#else
		if (dev_fill(zero_buf, sit_seg_addr, seg_size)) {
			MSG(1, "\tError: While zeroing out the sit area "
					"on disk!!!\n");
			free(zero_buf);
			return -1;
		}
#endif
		sit_seg_addr += seg_size;
	}

	free(zero_buf);
	return 0 ;
}

static int f2fs_init_nat_area(void)
{
	u_int32_t blk_size, seg_size;
	u_int32_t index = 0;
	u_int64_t nat_seg_addr = 0;
	u_int8_t *nat_buf = NULL;

	blk_size = 1 << get_sb(log_blocksize);
	seg_size = (1 << get_sb(log_blocks_per_seg)) * blk_size;

	nat_buf = calloc(sizeof(u_int8_t), seg_size);
	if (nat_buf == NULL) {
		MSG(1, "\tError: Calloc Failed for nat_zero_blk!!!\n");
		return -1;
	}

	nat_seg_addr = get_sb(nat_blkaddr);
	nat_seg_addr *= blk_size;

	DBG(1, "\tFilling nat area at offset 0x%08"PRIx64"\n", nat_seg_addr);
#ifdef AMF_SNAPSHOT
		dbg_log ("\n2.----\n");
		dbg_log ("nat_start_addr: super_block.nat_blkaddr: %lu (%lu)\n", 
				le32_to_cpu(get_sb(nat_blkaddr)), 
				le32_to_cpu(get_sb(nat_blkaddr) * blk_size));
	
		dbg_log ("nat_length(fill zeros): super_block.segment_count_nat: %d (%llu)\n", 
				get_sb(segment_count_nat),
				get_sb(segment_count_nat) * seg_size);
		
#endif

	for (index = 0; index < get_sb(segment_count_nat) / 2; index++) {
#ifdef AMF_SNAPSHOT
		dbg_log("nat_seg_addr_blk = %u\n",nat_seg_addr/blk_size);
		struct nvm_ret ret;
		struct nvm_addr tempaddr = nvm_addr_dev2gen(nvmdev, nat_seg_addr/blk_size);
		if(nvm_cmd_erase(nvmdev, &tempaddr, 1,NULL,1, &ret)){
			MSG(1, "\tError: While zeroing out the nat area "
					"on disk!!!\n");
			free(nat_buf);
			return -1;
		}
#else

		if (dev_fill(nat_buf, nat_seg_addr, seg_size)) {
			MSG(1, "\tError: While zeroing out the nat area "
					"on disk!!!\n");
			free(nat_buf);
			return -1;
		}
#endif

		nat_seg_addr = nat_seg_addr + (2 * seg_size);
	}

	free(nat_buf);
	return 0 ;
}

static int f2fs_write_check_point_pack(void)
{
	struct f2fs_summary_block *sum = NULL;
	struct f2fs_journal *journal;
	u_int32_t blk_size_bytes;
	u_int32_t nat_bits_bytes, nat_bits_blocks;
	unsigned char *nat_bits = NULL, *empty_nat_bits;
	u_int64_t cp_seg_blk = 0;
	u_int32_t crc = 0, flags;
	unsigned int i;
	char *cp_payload = NULL;
	char *sum_compact, *sum_compact_p;
	struct f2fs_summary *sum_entry;
	enum quota_type qtype;
	int off;
	int ret = -1;

	cp = calloc(F2FS_BLKSIZE, 1);
	if (cp == NULL) {
		MSG(1, "\tError: Calloc Failed for f2fs_checkpoint!!!\n");
		return ret;
	}

	sum = calloc(F2FS_BLKSIZE, 1);
	if (sum == NULL) {
		MSG(1, "\tError: Calloc Failed for summay_node!!!\n");
		goto free_cp;
	}

	sum_compact = calloc(F2FS_BLKSIZE, 1);
	if (sum_compact == NULL) {
		MSG(1, "\tError: Calloc Failed for summay buffer!!!\n");
		goto free_sum;
	}
	sum_compact_p = sum_compact;

	nat_bits_bytes = get_sb(segment_count_nat) << 5;
	nat_bits_blocks = F2FS_BYTES_TO_BLK((nat_bits_bytes << 1) + 8 +
						F2FS_BLKSIZE - 1);
	nat_bits = calloc(F2FS_BLKSIZE, nat_bits_blocks);
	if (nat_bits == NULL) {
		MSG(1, "\tError: Calloc Failed for nat bits buffer!!!\n");
		goto free_sum_compact;
	}

	cp_payload = calloc(F2FS_BLKSIZE, 1);
	if (cp_payload == NULL) {
		MSG(1, "\tError: Calloc Failed for cp_payload!!!\n");
		goto free_nat_bits;
	}

	/* 1. cp page 1 of checkpoint pack 1 */
	srand(time(NULL));
	cp->checkpoint_ver = cpu_to_le64(rand() | 0x1);
	set_cp(cur_node_segno[0], c.cur_seg[CURSEG_HOT_NODE]);
	set_cp(cur_node_segno[1], c.cur_seg[CURSEG_WARM_NODE]);
	set_cp(cur_node_segno[2], c.cur_seg[CURSEG_COLD_NODE]);
	set_cp(cur_data_segno[0], c.cur_seg[CURSEG_HOT_DATA]);
	set_cp(cur_data_segno[1], c.cur_seg[CURSEG_WARM_DATA]);
	set_cp(cur_data_segno[2], c.cur_seg[CURSEG_COLD_DATA]);
	for (i = 3; i < MAX_ACTIVE_NODE_LOGS; i++) {
		set_cp(cur_node_segno[i], 0xffffffff);
		set_cp(cur_data_segno[i], 0xffffffff);
	}

	set_cp(cur_node_blkoff[0], 1 + c.quota_inum + c.lpf_inum);
	set_cp(cur_data_blkoff[0], 1 + c.quota_dnum + c.lpf_dnum);
	set_cp(valid_block_count, 2 + c.quota_inum + c.quota_dnum +
			c.lpf_inum + c.lpf_dnum);
	set_cp(rsvd_segment_count, c.reserved_segments);
	set_cp(overprov_segment_count, (get_sb(segment_count_main) -
			get_cp(rsvd_segment_count)) *
			c.overprovision / 100);
	set_cp(overprov_segment_count, get_cp(overprov_segment_count) +
			get_cp(rsvd_segment_count));

	MSG(0, "Info: Overprovision ratio = %.3lf%%\n", c.overprovision);
	MSG(0, "Info: Overprovision segments = %u (GC reserved = %u)\n",
					get_cp(overprov_segment_count),
					c.reserved_segments);

	/* main segments - reserved segments - (node + data segments) */
	set_cp(free_segment_count, get_sb(segment_count_main) - 6);
	set_cp(user_block_count, ((get_cp(free_segment_count) + 6 -
			get_cp(overprov_segment_count)) * c.blks_per_seg));
	/* cp page (2), data summaries (1), node summaries (3) */
	set_cp(cp_pack_total_block_count, 6 + get_sb(cp_payload));
	flags = CP_UMOUNT_FLAG | CP_COMPACT_SUM_FLAG;
	if (get_cp(cp_pack_total_block_count) <=
			(1 << get_sb(log_blocks_per_seg)) - nat_bits_blocks)
		flags |= CP_NAT_BITS_FLAG;

	if (c.trimmed)
		flags |= CP_TRIMMED_FLAG;

	if (c.large_nat_bitmap)
		flags |= CP_LARGE_NAT_BITMAP_FLAG;

	set_cp(ckpt_flags, flags);
	set_cp(cp_pack_start_sum, 1 + get_sb(cp_payload));
	set_cp(valid_node_count, 1 + c.quota_inum + c.lpf_inum);
	set_cp(valid_inode_count, 1 + c.quota_inum + c.lpf_inum);
	set_cp(next_free_nid, c.next_free_nid);
	set_cp(sit_ver_bitmap_bytesize, ((get_sb(segment_count_sit) / 2) <<
			get_sb(log_blocks_per_seg)) / 8);

	set_cp(nat_ver_bitmap_bytesize, ((get_sb(segment_count_nat) / 2) <<
			 get_sb(log_blocks_per_seg)) / 8);

	set_cp(checksum_offset, CHECKSUM_OFFSET);

	crc = f2fs_cal_crc32(F2FS_SUPER_MAGIC, cp, CHECKSUM_OFFSET);
	*((__le32 *)((unsigned char *)cp + CHECKSUM_OFFSET)) =
							cpu_to_le32(crc);
#ifdef AMF_SNAPSHOT
dbg_log("checksum_offset = %u\n", get_cp(checksum_offset));
dbg_log("crc = %d\n", crc);
#endif
	blk_size_bytes = 1 << get_sb(log_blocksize);

	if (blk_size_bytes != F2FS_BLKSIZE) {
		MSG(1, "\tError: Wrong block size %d / %d!!!\n",
					blk_size_bytes, F2FS_BLKSIZE);
		goto free_cp_payload;
	}

	cp_seg_blk = get_sb(segment0_blkaddr);

#if defined(AMF_SNAPSHOT) && defined(AMF_META_LOGGING)
	dbg_log ("\n10.----\n");
		dbg_log ("ckp0_start_addr.orig: %u (%u)\n",cp_seg_blk, cp_seg_blk*blk_size_bytes);
	_meta_log_blkofs_2++;//2049
	DBG(1, "\tWriting main segments, cp at offset 0x%08"PRIx64"\n",
						_meta_log_blkofs_2);
	
	dbg_log ("[W] ckp0:\t" "%" PRIu64 "\t%u\t" "%" PRIu64 "\n", cp_seg_blk, F2FS_BLKSIZE/F2FS_BLKSIZE,_meta_log_blkofs_2);
	dbg_log ("%llu => %llu\n", cp_seg_blk, _meta_log_blkofs_2);

	struct nvm_ret nvmret;
	struct nvm_addr tempaddr;
	//tempaddr = nvm_addr_dev2gen(nvmdev, _meta_log_blkofs_2);
	//if(nvm_cmd_write(nvmdev, &tempaddr, 1, cp, NULL, 1, &nvmret)){
	if(nvm_ocssd_cmd_write(nvmdev, _meta_log_blkofs_2, 1, cp, &nvmret)){
		MSG(1, "\tError: While writing the cp to disk!!!\n");
		goto free_cp_payload;
	}

	for (i = 0; i < get_sb(cp_payload); i++) {
		_meta_log_blkofs_2++;
		cp_seg_blk++;
	dbg_log("\n----\n");
	dbg_log("write cp_payload at cp_seg_blk = %u ==>  _meta_log_blkofs_2=%u\n",cp_seg_blk, _meta_log_blkofs_2);
	//tempaddr = nvm_addr_dev2gen(nvmdev, _meta_log_blkofs_2);
	//if(nvm_cmd_write(nvmdev, &tempaddr, 1, cp_payload, NULL, 1, &nvmret)){
		if(nvm_ocssd_cmd_write(nvmdev, _meta_log_blkofs_2, 1, cp_payload, &nvmret)){
			MSG(1, "\tError: While zeroing out the sit bitmap area "
					"on disk!!!\n");
			goto free_cp_payload;
		}
	}
#else
	DBG(1, "\tWriting main segments, cp at offset 0x%08"PRIx64"\n",
							cp_seg_blk);
		if (dev_write_block(cp, cp_seg_blk)) {
			MSG(1, "\tError: While writing the cp to disk!!!\n");
			goto free_cp_payload;
		}
	
		for (i = 0; i < get_sb(cp_payload); i++) {
			cp_seg_blk++;
			if (dev_fill_block(cp_payload, cp_seg_blk)) {
				MSG(1, "\tError: While zeroing out the sit bitmap area "
						"on disk!!!\n");
				goto free_cp_payload;
			}
		}
#endif
	/* Prepare and write Segment summary for HOT/WARM/COLD DATA
	 *
	 * The structure of compact summary
	 * +-------------------+
	 * | nat_journal       |
	 * +-------------------+
	 * | sit_journal       |
	 * +-------------------+
	 * | hot data summary  |
	 * +-------------------+
	 * | warm data summary |
	 * +-------------------+
	 * | cold data summary |
	 * +-------------------+
	*/
	memset(sum, 0, sizeof(struct f2fs_summary_block));
	SET_SUM_TYPE((&sum->footer), SUM_TYPE_DATA);

	journal = &sum->journal;
	journal->n_nats = cpu_to_le16(1 + c.quota_inum + c.lpf_inum);
	journal->nat_j.entries[0].nid = sb->root_ino;
	journal->nat_j.entries[0].ne.version = 0;
	journal->nat_j.entries[0].ne.ino = sb->root_ino;
	journal->nat_j.entries[0].ne.block_addr = cpu_to_le32(
			get_sb(main_blkaddr) +
			get_cp(cur_node_segno[0]) * c.blks_per_seg);

	for (qtype = 0, i = 1; qtype < F2FS_MAX_QUOTAS; qtype++) {
		if (sb->qf_ino[qtype] == 0)
			continue;
		journal->nat_j.entries[i].nid = sb->qf_ino[qtype];
		journal->nat_j.entries[i].ne.version = 0;
		journal->nat_j.entries[i].ne.ino = sb->qf_ino[qtype];
		journal->nat_j.entries[i].ne.block_addr = cpu_to_le32(
				get_sb(main_blkaddr) +
				get_cp(cur_node_segno[0]) *
				c.blks_per_seg + i);
		i++;
	}

	if (c.lpf_inum) {
		journal->nat_j.entries[i].nid = cpu_to_le32(c.lpf_ino);
		journal->nat_j.entries[i].ne.version = 0;
		journal->nat_j.entries[i].ne.ino = cpu_to_le32(c.lpf_ino);
		journal->nat_j.entries[i].ne.block_addr = cpu_to_le32(
				get_sb(main_blkaddr) +
				get_cp(cur_node_segno[0]) *
				c.blks_per_seg + i);
	}

	memcpy(sum_compact_p, &journal->n_nats, SUM_JOURNAL_SIZE);
	sum_compact_p += SUM_JOURNAL_SIZE;

	memset(sum, 0, sizeof(struct f2fs_summary_block));
	/* inode sit for root */
	journal->n_sits = cpu_to_le16(6);
	journal->sit_j.entries[0].segno = cp->cur_node_segno[0];
	journal->sit_j.entries[0].se.vblocks =
				cpu_to_le16((CURSEG_HOT_NODE << 10) |
						(1 + c.quota_inum + c.lpf_inum));
	f2fs_set_bit(0, (char *)journal->sit_j.entries[0].se.valid_map);
	for (i = 1; i <= c.quota_inum; i++)
		f2fs_set_bit(i, (char *)journal->sit_j.entries[0].se.valid_map);
	if (c.lpf_inum)
		f2fs_set_bit(i, (char *)journal->sit_j.entries[0].se.valid_map);

	journal->sit_j.entries[1].segno = cp->cur_node_segno[1];
	journal->sit_j.entries[1].se.vblocks =
				cpu_to_le16((CURSEG_WARM_NODE << 10));
	journal->sit_j.entries[2].segno = cp->cur_node_segno[2];
	journal->sit_j.entries[2].se.vblocks =
				cpu_to_le16((CURSEG_COLD_NODE << 10));

	/* data sit for root */
	journal->sit_j.entries[3].segno = cp->cur_data_segno[0];
	journal->sit_j.entries[3].se.vblocks =
				cpu_to_le16((CURSEG_HOT_DATA << 10) |
						(1 + c.quota_dnum + c.lpf_dnum));
	f2fs_set_bit(0, (char *)journal->sit_j.entries[3].se.valid_map);
	for (i = 1; i <= c.quota_dnum; i++)
		f2fs_set_bit(i, (char *)journal->sit_j.entries[3].se.valid_map);
	if (c.lpf_dnum)
		f2fs_set_bit(i, (char *)journal->sit_j.entries[3].se.valid_map);

	journal->sit_j.entries[4].segno = cp->cur_data_segno[1];
	journal->sit_j.entries[4].se.vblocks =
				cpu_to_le16((CURSEG_WARM_DATA << 10));
	journal->sit_j.entries[5].segno = cp->cur_data_segno[2];
	journal->sit_j.entries[5].se.vblocks =
				cpu_to_le16((CURSEG_COLD_DATA << 10));

	memcpy(sum_compact_p, &journal->n_sits, SUM_JOURNAL_SIZE);
	sum_compact_p += SUM_JOURNAL_SIZE;

	/* hot data summary */
	sum_entry = (struct f2fs_summary *)sum_compact_p;
	sum_entry->nid = sb->root_ino;
	sum_entry->ofs_in_node = 0;

	off = 1;
	for (qtype = 0; qtype < F2FS_MAX_QUOTAS; qtype++) {
		if (sb->qf_ino[qtype] == 0)
			continue;
		int j;

		for (j = 0; j < QUOTA_DATA(qtype); j++) {
			(sum_entry + off + j)->nid = sb->qf_ino[qtype];
			(sum_entry + off + j)->ofs_in_node = cpu_to_le16(j);
		}
		off += QUOTA_DATA(qtype);
	}

	if (c.lpf_dnum) {
		(sum_entry + off)->nid = cpu_to_le32(c.lpf_ino);
		(sum_entry + off)->ofs_in_node = 0;
	}

	/* warm data summary, nothing to do */
	/* cold data summary, nothing to do */
	cp_seg_blk++;
	
#if defined(AMF_SNAPSHOT) && defined(AMF_META_LOGGING)
	_meta_log_blkofs_2++;
	DBG(1, "\tWriting Segment summary for HOT/WARM/COLD_DATA, at offset 0x%08"PRIx64"\n",
			_meta_log_blkofs_2);
	dbg_log("\n----\n");
	dbg_log("[W] Writing Segment summary for HOT/WARM/COLD_DATA, cp_seg_blk = %u -->_meta_log_blkofs_2 = %u\n",cp_seg_blk,_meta_log_blkofs_2);
	//tempaddr = nvm_addr_dev2gen(nvmdev, _meta_log_blkofs_2);
	//if(nvm_cmd_write(nvmdev, &tempaddr, 1, sum_compact, NULL, 1, &nvmret)){
	if(nvm_ocssd_cmd_write(nvmdev, _meta_log_blkofs_2, 1, sum_compact, &nvmret)){
		MSG(1, "\tError: While writing the sum_blk to disk!!!\n");
		goto free_cp_payload;
	}
	
#else	
	DBG(1, "\tWriting Segment summary for HOT/WARM/COLD_DATA, at offset 0x%08"PRIx64"\n",
			cp_seg_blk);
	if (dev_write_block(sum_compact, cp_seg_blk)) {
		MSG(1, "\tError: While writing the sum_blk to disk!!!\n");
		goto free_cp_payload;
	}
#endif

	/* Prepare and write Segment summary for HOT_NODE */
	memset(sum, 0, sizeof(struct f2fs_summary_block));
	SET_SUM_TYPE((&sum->footer), SUM_TYPE_NODE);

	sum->entries[0].nid = sb->root_ino;
	sum->entries[0].ofs_in_node = 0;
	for (qtype = i = 0; qtype < F2FS_MAX_QUOTAS; qtype++) {
		if (sb->qf_ino[qtype] == 0)
			continue;
		sum->entries[1 + i].nid = sb->qf_ino[qtype];
		sum->entries[1 + i].ofs_in_node = 0;
		i++;
	}
	if (c.lpf_inum) {
		i++;
		sum->entries[i].nid = cpu_to_le32(c.lpf_ino);
		sum->entries[i].ofs_in_node = 0;
	}
	
	cp_seg_blk++;

#if defined(AMF_SNAPSHOT) && defined(AMF_META_LOGGING)
	_meta_log_blkofs_2++;
		DBG(1, "\tWriting Segment summary for HOT_NODE, at offset 0x%08"PRIx64"\n",
				_meta_log_blkofs_2);
		dbg_log("\n----\n");
		dbg_log("[W] Writing Segment summary for HOT_NODE, at cp_seg_blk = %u --> _meta_log_blkofs_2 = %u\n",cp_seg_blk,_meta_log_blkofs_2);
		//tempaddr = nvm_addr_dev2gen(nvmdev, _meta_log_blkofs_2);
		//if(nvm_cmd_write(nvmdev, &tempaddr, 1, sum, NULL, 1, &nvmret)){
		if(nvm_ocssd_cmd_write(nvmdev, _meta_log_blkofs_2, 1, sum, &nvmret)){
			MSG(1, "\tError: While writing the sum_blk to disk!!!\n");
			goto free_cp_payload;
		}
#else	
	DBG(1, "\tWriting Segment summary for HOT_NODE, at offset 0x%08"PRIx64"\n",
			cp_seg_blk);
	if (dev_write_block(sum, cp_seg_blk)) {
		MSG(1, "\tError: While writing the sum_blk to disk!!!\n");
		goto free_cp_payload;
	}
#endif

	/* Fill segment summary for WARM_NODE to zero. */
	memset(sum, 0, sizeof(struct f2fs_summary_block));
	SET_SUM_TYPE((&sum->footer), SUM_TYPE_NODE);

	cp_seg_blk++;

#if defined(AMF_SNAPSHOT) && defined(AMF_META_LOGGING)
	_meta_log_blkofs_2++;
	DBG(1, "\tWriting Segment summary for WARM_NODE, at offset 0x%08"PRIx64"\n",
				_meta_log_blkofs_2);
	dbg_log("\n----\n");
	dbg_log("[W] Writing Segment summary for WARM_NODE,at cp_seg_blk = %u --> _meta_log_blkofs_2 = %u\n",cp_seg_blk,_meta_log_blkofs_2);
	//tempaddr = nvm_addr_dev2gen(nvmdev, _meta_log_blkofs_2);
	//if(nvm_cmd_write(nvmdev, &tempaddr, 1, sum, NULL, 1, &nvmret)){
	if(nvm_ocssd_cmd_write(nvmdev, _meta_log_blkofs_2, 1, sum, &nvmret)){
			MSG(1, "\tError: While writing the sum_blk to disk!!!\n");
			goto free_cp_payload;
		}
#else	
	DBG(1, "\tWriting Segment summary for WARM_NODE, at offset 0x%08"PRIx64"\n",
			cp_seg_blk);
	
	if (dev_write_block(sum, cp_seg_blk)) {
		MSG(1, "\tError: While writing the sum_blk to disk!!!\n");
		goto free_cp_payload;
	}
#endif

	/* Fill segment summary for COLD_NODE to zero. */
	memset(sum, 0, sizeof(struct f2fs_summary_block));
	SET_SUM_TYPE((&sum->footer), SUM_TYPE_NODE);

	cp_seg_blk++;
	
#if defined(AMF_SNAPSHOT) && defined(AMF_META_LOGGING)
	_meta_log_blkofs_2++;
	DBG(1, "\tWriting Segment summary for COLD_NODE, at offset 0x%08"PRIx64"\n",
			_meta_log_blkofs_2);
	dbg_log("\n----\n");
	dbg_log("[W] write  segment summary for cold_node, at cp_seg_blk = %u --> _meta_log_blkofs_2 = %u\n",cp_seg_blk,_meta_log_blkofs_2);
	//tempaddr = nvm_addr_dev2gen(nvmdev, _meta_log_blkofs_2);
	//if(nvm_cmd_write(nvmdev, &tempaddr, 1, sum, NULL, 1, &nvmret)){
	if(nvm_ocssd_cmd_write(nvmdev, _meta_log_blkofs_2, 1, sum, &nvmret)){
		MSG(1, "\tError: While writing the sum_blk to disk!!!\n");
		goto free_cp_payload;
	}
#else	
	DBG(1, "\tWriting Segment summary for COLD_NODE, at offset 0x%08"PRIx64"\n",
			cp_seg_blk);
	if (dev_write_block(sum, cp_seg_blk)) {
		MSG(1, "\tError: While writing the sum_blk to disk!!!\n");
		goto free_cp_payload;
	}
#endif

	/* cp page2 */
	cp_seg_blk++;
#if defined(AMF_SNAPSHOT) && defined(AMF_META_LOGGING)
	_meta_log_blkofs_2++;
		DBG(1, "\tWriting cp page2, at offset 0x%08"PRIx64"\n", _meta_log_blkofs_2);
		dbg_log("\n----\n");
		dbg_log("[W] Writing cp page2, at cp_seg_blk = %u -->_meta_log_blkofs_2 = %u\n",cp_seg_blk,_meta_log_blkofs_2);
		//tempaddr = nvm_addr_dev2gen(nvmdev, _meta_log_blkofs_2);
		//if(nvm_cmd_write(nvmdev, &tempaddr, 1, cp, NULL, 1, &nvmret)){
		if(nvm_ocssd_cmd_write(nvmdev, _meta_log_blkofs_2, 1, cp, &nvmret)){
			MSG(1, "\tError: While writing the cp to disk!!!\n");
			goto free_cp_payload;
		}
#else	
	DBG(1, "\tWriting cp page2, at offset 0x%08"PRIx64"\n", cp_seg_blk);
	if (dev_write_block(cp, cp_seg_blk)) {
		MSG(1, "\tError: While writing the cp to disk!!!\n");
		goto free_cp_payload;
	}
#endif
	/* write NAT bits, if possible */
	if (flags & CP_NAT_BITS_FLAG) {
		uint32_t i;

		*(__le64 *)nat_bits = get_cp_crc(cp);
		empty_nat_bits = nat_bits + 8 + nat_bits_bytes;
		memset(empty_nat_bits, 0xff, nat_bits_bytes);
		test_and_clear_bit_le(0, empty_nat_bits);

		/* write the last blocks in cp pack */
		cp_seg_blk = get_sb(segment0_blkaddr) + (1 <<
				get_sb(log_blocks_per_seg)) - nat_bits_blocks;

		DBG(1, "\tWriting NAT bits pages, at offset 0x%08"PRIx64"\n",
					cp_seg_blk);
		
#if defined(AMF_SNAPSHOT) && defined(AMF_META_LOGGING)
		dbg_log("\n----\n");
		dbg_log("[W] Writing NAT bits pages, at ");
		for (i = 0; i < nat_bits_blocks; i++) {
			_meta_log_blkofs_2++;
			dbg_log("cp_seg_blk+i = %u -->_meta_log_blkofs_2 = %u\n",cp_seg_blk+i,_meta_log_blkofs_2);
		//tempaddr = nvm_addr_dev2gen(nvmdev, _meta_log_blkofs_2);
		//if(nvm_cmd_write(nvmdev, &tempaddr, 1, nat_bits + i *	F2FS_BLKSIZE, NULL, 1, &nvmret)){
		if(nvm_ocssd_cmd_write(nvmdev, _meta_log_blkofs_2, 1, nat_bits + i * F2FS_BLKSIZE, &nvmret)){
				MSG(1, "\tError: write NAT bits to disk!!!\n");
				goto free_cp_payload;
			}
		}
#else
		for (i = 0; i < nat_bits_blocks; i++) {
			if (dev_write_block(nat_bits + i *
						F2FS_BLKSIZE, cp_seg_blk + i)) {
				MSG(1, "\tError: write NAT bits to disk!!!\n");
				goto free_cp_payload;
			}
		}
#endif
	}

	/* cp page 1 of check point pack 2
	 * Initiatialize other checkpoint pack with version zero
	 */
	cp->checkpoint_ver = 0;

	crc = f2fs_cal_crc32(F2FS_SUPER_MAGIC, cp, CHECKSUM_OFFSET);
	*((__le32 *)((unsigned char *)cp + CHECKSUM_OFFSET)) =
							cpu_to_le32(crc);

	cp_seg_blk = get_sb(segment0_blkaddr) + c.blks_per_seg;
	
#if defined(AMF_SNAPSHOT) && defined(AMF_META_LOGGING)
	_meta_log_blkofs_2++;
	DBG(1, "\tWriting cp page 1 of checkpoint pack 2, at offset 0x%08"PRIx64"\n",
				cp_seg_blk);
	dbg_log("\n---\n");
	dbg_log("[W] Writing cp page 1 of checkpoint pack 2 at cp_seg_blk = %u --> _meta_log_blkofs_2 = %u\n",cp_seg_blk,_meta_log_blkofs_2);
	//tempaddr = nvm_addr_dev2gen(nvmdev, _meta_log_blkofs_2);
	//if(nvm_cmd_write(nvmdev, &tempaddr, 1, cp, NULL, 1, &nvmret)){
	if(nvm_ocssd_cmd_write(nvmdev, _meta_log_blkofs_2, 1, cp, &nvmret)){
		MSG(1, "\tError: While writing the cp to disk!!!\n");
		goto free_cp_payload;
	}
	dbg_log("\n----\n");
	dbg_log("[W] writing cp payload at ");
	for (i = 0; i < get_sb(cp_payload); i++) {
		_meta_log_blkofs_2++;
		cp_seg_blk++;
		dbg_log(" cp_seg_blk = %u --> _meta_log_blkofs_2 = %u\n", cp_seg_blk, _meta_log_blkofs_2);
		//tempaddr = nvm_addr_dev2gen(nvmdev, _meta_log_blkofs_2);
		//if(nvm_cmd_write(nvmdev, &tempaddr, 1, cp_payload, NULL, 1, &nvmret)){
		if(nvm_ocssd_cmd_write(nvmdev, _meta_log_blkofs_2, 1, cp_payload, &nvmret)){
			MSG(1, "\tError: While zeroing out the sit bitmap area "
					"on disk!!!\n");
			goto free_cp_payload;
		}
	}
#else	
	DBG(1, "\tWriting cp page 1 of checkpoint pack 2, at offset 0x%08"PRIx64"\n",
				cp_seg_blk);
	if (dev_write_block(cp, cp_seg_blk)) {
		MSG(1, "\tError: While writing the cp to disk!!!\n");
		goto free_cp_payload;
	}
	for (i = 0; i < get_sb(cp_payload); i++) {
		cp_seg_blk++;
		if (dev_fill_block(cp_payload, cp_seg_blk)) {
			MSG(1, "\tError: While zeroing out the sit bitmap area "
					"on disk!!!\n");
			goto free_cp_payload;
		}
	}
#endif

	

	/* cp page 2 of check point pack 2 */
	cp_seg_blk += (le32_to_cpu(cp->cp_pack_total_block_count) -
						get_sb(cp_payload) - 1);

#if defined(AMF_SNAPSHOT) && defined(AMF_META_LOGGING)
	_meta_log_blkofs_2++;
	DBG(1, "\tWriting cp page 2 of checkpoint pack 2, at offset 0x%08"PRIx64"\n",
				cp_seg_blk);
	dbg_log("\n----\n");
	dbg_log("[W] Writing cp page 2 of checkpoint pack 2 at cp_seg_blk = %u --> _meta_log_blkofs_2 = %u\n",cp_seg_blk, _meta_log_blkofs_2);
	//tempaddr = nvm_addr_dev2gen(nvmdev, _meta_log_blkofs_2);
	//if(nvm_cmd_write(nvmdev, &tempaddr, 1, cp, NULL, 1, &nvmret)){
	if(nvm_ocssd_cmd_write(nvmdev, _meta_log_blkofs_2, 1, cp, &nvmret)){
		MSG(1, "\tError: While writing the cp to disk!!!\n");
		goto free_cp_payload;
	}
#else	
		DBG(1, "\tWriting cp page 2 of checkpoint pack 2, at offset 0x%08"PRIx64"\n",
					cp_seg_blk);
		if (dev_write_block(cp, cp_seg_blk)) {
			MSG(1, "\tError: While writing the cp to disk!!!\n");
			goto free_cp_payload;
		}
#endif
	ret = 0;

free_cp_payload:
	free(cp_payload);
free_nat_bits:
	free(nat_bits);
free_sum_compact:
	free(sum_compact);
free_sum:
	free(sum);
free_cp:
	free(cp);
	return ret;
}

static int f2fs_write_super_block(void)
{//基本没变
	int index;
	u_int8_t *zero_buff;

	zero_buff = calloc(F2FS_BLKSIZE, 1);

	memcpy(zero_buff + F2FS_SUPER_OFFSET, sb, sizeof(*sb));

#ifdef AMF_SNAPSHOT
	dbg_log ("\n----\n");
	dbg_log ("superblock_start_addr: %u\n", 0);

	DBG(1, "\tWriting super block, at offset 0x%08x\n", 0);
	struct nvm_ret ret;
	struct nvm_addr tempaddr;
	for (index = 0; index < 2; index++) {
		dbg_log ("[W] super_blk: %u\t%u\t%u\n", (index * F2FS_BLKSIZE)/F2FS_BLKSIZE, F2FS_BLKSIZE/F2FS_BLKSIZE, 
									(index * F2FS_BLKSIZE + F2FS_BLKSIZE)/F2FS_BLKSIZE);		
		//tempaddr = nvm_addr_dev2gen(nvmdev, index);
		//if(nvm_cmd_write(nvmdev, &tempaddr, 1, zero_buff, NULL, 1, &ret)){
		if(nvm_ocssd_cmd_write(nvmdev, index, 1, zero_buff, &ret)){
			MSG(1, "\tError: While while writing supe_blk "
					"on disk!!! index : %d\n", index);
			free(zero_buff);
			return -1;
		}
		/*
		if (dev_write_block(zero_buff, index)) {
				MSG(1, "\tError: While while writing supe_blk "
						"on disk!!! index : %d\n", index);
				free(zero_buff);
				return -1;
			}
		*/
	}
#else
	DBG(1, "\tWriting super block, at offset 0x%08x\n", 0);
		for (index = 0; index < 2; index++) {
			if (dev_write_block(zero_buff, index)) {
				MSG(1, "\tError: While while writing supe_blk "
						"on disk!!! index : %d\n", index);
				free(zero_buff);
				return -1;
			}
		}

#endif
	free(zero_buff);
	return 0;
}

#ifndef WITH_ANDROID
static int f2fs_discard_obsolete_dnode(void)
{
	struct f2fs_node *raw_node;
	u_int64_t next_blkaddr = 0, offset;
	u64 end_blkaddr = (get_sb(segment_count_main) <<
			get_sb(log_blocks_per_seg)) + get_sb(main_blkaddr);
	u_int64_t start_inode_pos = get_sb(main_blkaddr);
	u_int64_t last_inode_pos;

	if (c.zoned_mode)
		return 0;

	raw_node = calloc(sizeof(struct f2fs_node), 1);
	if (!raw_node)
		return -1;

	/* avoid power-off-recovery based on roll-forward policy */
	offset = get_sb(main_blkaddr);
	offset += c.cur_seg[CURSEG_WARM_NODE] * c.blks_per_seg;

	last_inode_pos = start_inode_pos +
		c.cur_seg[CURSEG_HOT_NODE] * c.blks_per_seg + c.quota_inum + c.lpf_inum;
#ifdef AMF_SNAPSHOT
	dbg_log("\n7.----\n");
	dbg_log("offset = main_blkaddr +  c.cur_seg[CURSEG_WARM_NODE] * c.blks_per_seg = %u\n ",offset);
#endif

	do {
		if (offset < get_sb(main_blkaddr) || offset >= end_blkaddr)
			break;

		if (dev_read_block(raw_node, offset)) {
			MSG(1, "\tError: While traversing direct node!!!\n");
			free(raw_node);
			return -1;
		}

		next_blkaddr = le32_to_cpu(raw_node->footer.next_blkaddr);
		memset(raw_node, 0, F2FS_BLKSIZE);

		DBG(1, "\tDiscard dnode, at offset 0x%08"PRIx64"\n", offset);
#ifdef AMF_SNAPSHOT	
	dbg_log("Discard dnode(raw_node), at offset = %u \n",offset);
#endif
		if (dev_write_block(raw_node, offset)) {
			MSG(1, "\tError: While discarding direct node!!!\n");
			free(raw_node);
			return -1;
		}
		offset = next_blkaddr;
		/* should avoid recursive chain due to stale data */
		if (offset >= start_inode_pos || offset <= last_inode_pos)
			break;
	} while (1);

	free(raw_node);
	return 0;
}
#endif

static int f2fs_write_root_inode(void)
{
	struct f2fs_node *raw_node = NULL;
	u_int64_t blk_size_bytes, data_blk_nor;
	u_int64_t main_area_node_seg_blk_offset = 0;

	raw_node = calloc(F2FS_BLKSIZE, 1);
	if (raw_node == NULL) {
		MSG(1, "\tError: Calloc Failed for raw_node!!!\n");
		return -1;
	}

	raw_node->footer.nid = sb->root_ino;
	raw_node->footer.ino = sb->root_ino;
	raw_node->footer.cp_ver = cpu_to_le64(1);
	raw_node->footer.next_blkaddr = cpu_to_le32(
			get_sb(main_blkaddr) +
			c.cur_seg[CURSEG_HOT_NODE] *
			c.blks_per_seg + 1);

	raw_node->i.i_mode = cpu_to_le16(0x41ed);
	if (c.lpf_ino)
		raw_node->i.i_links = cpu_to_le32(3);
	else
		raw_node->i.i_links = cpu_to_le32(2);
	raw_node->i.i_uid = cpu_to_le32(getuid());
	raw_node->i.i_gid = cpu_to_le32(getgid());

	blk_size_bytes = 1 << get_sb(log_blocksize);
	raw_node->i.i_size = cpu_to_le64(1 * blk_size_bytes); /* dentry */
	raw_node->i.i_blocks = cpu_to_le64(2);

	raw_node->i.i_atime = cpu_to_le32(time(NULL));
	raw_node->i.i_atime_nsec = 0;
	raw_node->i.i_ctime = cpu_to_le32(time(NULL));
	raw_node->i.i_ctime_nsec = 0;
	raw_node->i.i_mtime = cpu_to_le32(time(NULL));
	raw_node->i.i_mtime_nsec = 0;
	raw_node->i.i_generation = 0;
	raw_node->i.i_xattr_nid = 0;
	raw_node->i.i_flags = 0;
	raw_node->i.i_current_depth = cpu_to_le32(1);
	raw_node->i.i_dir_level = DEF_DIR_LEVEL;

	if (c.feature & cpu_to_le32(F2FS_FEATURE_EXTRA_ATTR)) {
		raw_node->i.i_inline = F2FS_EXTRA_ATTR;
		raw_node->i.i_extra_isize =
				cpu_to_le16(F2FS_TOTAL_EXTRA_ATTR_SIZE);
	}

	if (c.feature & cpu_to_le32(F2FS_FEATURE_PRJQUOTA))
		raw_node->i.i_projid = cpu_to_le32(F2FS_DEF_PROJID);

	if (c.feature & cpu_to_le32(F2FS_FEATURE_INODE_CRTIME)) {
		raw_node->i.i_crtime = cpu_to_le32(time(NULL));
		raw_node->i.i_crtime_nsec = 0;
	}

	data_blk_nor = get_sb(main_blkaddr) +
		c.cur_seg[CURSEG_HOT_DATA] * c.blks_per_seg;
	raw_node->i.i_addr[get_extra_isize(raw_node)] = cpu_to_le32(data_blk_nor);

	raw_node->i.i_ext.fofs = 0;
	raw_node->i.i_ext.blk_addr = 0;
	raw_node->i.i_ext.len = 0;

	if (c.feature & cpu_to_le32(F2FS_FEATURE_INODE_CHKSUM))
		raw_node->i.i_inode_checksum =
			cpu_to_le32(f2fs_inode_chksum(raw_node));




	main_area_node_seg_blk_offset = get_sb(main_blkaddr);
	main_area_node_seg_blk_offset += c.cur_seg[CURSEG_HOT_NODE] *
					c.blks_per_seg;


	DBG(1, "\tWriting root inode (hot node), %x %x %x at offset 0x%08"PRIu64"\n",
			get_sb(main_blkaddr),
			c.cur_seg[CURSEG_HOT_NODE],
			c.blks_per_seg, main_area_node_seg_blk_offset);


			
			

#ifdef AMF_SNAPSHOT
				dbg_log ("\n3.----\n");
				dbg_log ("main_start_addr: %u (%u)\n", 
				le32_to_cpu(get_sb(main_blkaddr)),
				le32_to_cpu(get_sb(main_blkaddr)) * blk_size_bytes);
				dbg_log (" * hot_node_start_addr: %u(=%u+%u*%u) (%u)\n",
						(le32_to_cpu(get_sb(main_blkaddr)) + c.cur_seg[CURSEG_HOT_NODE] * c.blks_per_seg),
						le32_to_cpu(get_sb(main_blkaddr)),
						c.cur_seg[CURSEG_HOT_NODE],
						c.blks_per_seg,
						(le32_to_cpu(get_sb(main_blkaddr)) + c.cur_seg[CURSEG_HOT_NODE] * c.blks_per_seg) * blk_size_bytes);
				dbg_log ("[W] raw_node(root_ino):\t" "%" PRIu64 "\t%u\t" "%" PRIu64 "\n", 
							main_area_node_seg_blk_offset, 
							F2FS_BLKSIZE/F2FS_BLKSIZE, 
							(main_area_node_seg_blk_offset));//不知道为什么要除以4096??
				dbg_log("在main_blkaddr+c.cur_seg[CURSEG_HOT_NODE] *c.blks_per_seg后写入raw_node\n");

	struct nvm_ret ret;
	
	
	if (nvm_ocssd_cmd_write(nvmdev, main_area_node_seg_blk_offset, 1, raw_node, &ret)) {
		MSG(1, "\tError: While writing the raw_node to disk!!!\n");
		free(raw_node);
		return -1;
	}


#else
	if (dev_write_block(raw_node, main_area_node_seg_blk_offset)) {
		MSG(1, "\tError: While writing the raw_node to disk!!!\n");
		free(raw_node);
		return -1;
	}
#endif

	free(raw_node);
	return 0;
}

static int f2fs_write_default_quota(int qtype, unsigned int blkaddr,
						__le32 raw_id)
{
	char *filebuf = calloc(F2FS_BLKSIZE, 2);
	int file_magics[] = INITQMAGICS;
	struct v2_disk_dqheader ddqheader;
	struct v2_disk_dqinfo ddqinfo;
	struct v2r1_disk_dqblk dqblk;

	if (filebuf == NULL) {
		MSG(1, "\tError: Calloc Failed for filebuf!!!\n");
		return -1;
	}

	/* Write basic quota header */
	ddqheader.dqh_magic = cpu_to_le32(file_magics[qtype]);
	/* only support QF_VFSV1 */
	ddqheader.dqh_version = cpu_to_le32(1);

	memcpy(filebuf, &ddqheader, sizeof(ddqheader));

	/* Fill Initial quota file content */
	ddqinfo.dqi_bgrace = cpu_to_le32(MAX_DQ_TIME);
	ddqinfo.dqi_igrace = cpu_to_le32(MAX_IQ_TIME);
	ddqinfo.dqi_flags = cpu_to_le32(0);
	ddqinfo.dqi_blocks = cpu_to_le32(QT_TREEOFF + 5);
	ddqinfo.dqi_free_blk = cpu_to_le32(0);
	ddqinfo.dqi_free_entry = cpu_to_le32(5);

	memcpy(filebuf + V2_DQINFOOFF, &ddqinfo, sizeof(ddqinfo));

	filebuf[1024] = 2;
	filebuf[2048] = 3;
	filebuf[3072] = 4;
	filebuf[4096] = 5;

	filebuf[5120 + 8] = 1;

	dqblk.dqb_id = raw_id;
	dqblk.dqb_pad = cpu_to_le32(0);
	dqblk.dqb_ihardlimit = cpu_to_le64(0);
	dqblk.dqb_isoftlimit = cpu_to_le64(0);
	if (c.lpf_ino)
		dqblk.dqb_curinodes = cpu_to_le64(2);
	else
		dqblk.dqb_curinodes = cpu_to_le64(1);
	dqblk.dqb_bhardlimit = cpu_to_le64(0);
	dqblk.dqb_bsoftlimit = cpu_to_le64(0);
	if (c.lpf_ino)
		dqblk.dqb_curspace = cpu_to_le64(8192);
	else
		dqblk.dqb_curspace = cpu_to_le64(4096);
	dqblk.dqb_btime = cpu_to_le64(0);
	dqblk.dqb_itime = cpu_to_le64(0);

	memcpy(filebuf + 5136, &dqblk, sizeof(struct v2r1_disk_dqblk));

	/* Write two blocks */

#ifdef AMF_SNAPSHOT
	dbg_log("\n4.----\n");
	dbg_log("写入quota file content, blkaddr = %u, blkaddr+1 = %u\n",blkaddr, blkaddr+1);
	struct nvm_ret ret;
	struct nvm_addr tempaddr,tempaddr2;
	//tempaddr = nvm_addr_dev2gen(nvmdev, blkaddr);
	//tempaddr2 = nvm_addr_dev2gen(nvmdev, blkaddr+1);
	//if(nvm_cmd_write(nvmdev, &tempaddr, 1, filebuf, NULL, 1, &ret) || nvm_cmd_write(nvmdev, &tempaddr2, 1, filebuf + F2FS_BLKSIZE, NULL, 1, &ret)){
	if(nvm_ocssd_cmd_write(nvmdev, blkaddr, 1, filebuf, &ret) || nvm_ocssd_cmd_write(nvmdev, blkaddr+1, 1, filebuf + F2FS_BLKSIZE, &ret)){
		MSG(1, "\tError: While writing the quota_blk to disk!!!\n");
				free(filebuf);
				return -1;

	}
	
#else
	if (dev_write_block(filebuf, blkaddr) ||
	    dev_write_block(filebuf + F2FS_BLKSIZE, blkaddr + 1)) {
		MSG(1, "\tError: While writing the quota_blk to disk!!!\n");
		free(filebuf);
		return -1;
	}
#endif
	DBG(1, "\tWriting quota data, at offset %08x, %08x\n",
					blkaddr, blkaddr + 1);
	free(filebuf);
	c.quota_dnum += QUOTA_DATA(qtype);
	return 0;
}

static int f2fs_write_qf_inode(int qtype)
{
	struct f2fs_node *raw_node = NULL;
	u_int64_t data_blk_nor;
	u_int64_t main_area_node_seg_blk_offset = 0;
	__le32 raw_id;
	int i;

	raw_node = calloc(F2FS_BLKSIZE, 1);
	if (raw_node == NULL) {
		MSG(1, "\tError: Calloc Failed for raw_node!!!\n");
		return -1;
	}

	raw_node->footer.nid = sb->qf_ino[qtype];
	raw_node->footer.ino = sb->qf_ino[qtype];
	raw_node->footer.cp_ver = cpu_to_le64(1);
	raw_node->footer.next_blkaddr = cpu_to_le32(
			get_sb(main_blkaddr) +
			c.cur_seg[CURSEG_HOT_NODE] *
			c.blks_per_seg + 1 + qtype + 1);

	raw_node->i.i_mode = cpu_to_le16(0x8180);
	raw_node->i.i_links = cpu_to_le32(1);
	raw_node->i.i_uid = cpu_to_le32(getuid());
	raw_node->i.i_gid = cpu_to_le32(getgid());

	raw_node->i.i_size = cpu_to_le64(1024 * 6); /* Hard coded */
	raw_node->i.i_blocks = cpu_to_le64(1 + QUOTA_DATA(qtype));

	raw_node->i.i_atime = cpu_to_le32(time(NULL));
	raw_node->i.i_atime_nsec = 0;
	raw_node->i.i_ctime = cpu_to_le32(time(NULL));
	raw_node->i.i_ctime_nsec = 0;
	raw_node->i.i_mtime = cpu_to_le32(time(NULL));
	raw_node->i.i_mtime_nsec = 0;
	raw_node->i.i_generation = 0;
	raw_node->i.i_xattr_nid = 0;
	raw_node->i.i_flags = FS_IMMUTABLE_FL;
	raw_node->i.i_current_depth = cpu_to_le32(1);
	raw_node->i.i_dir_level = DEF_DIR_LEVEL;

	if (c.feature & cpu_to_le32(F2FS_FEATURE_EXTRA_ATTR)) {
		raw_node->i.i_inline = F2FS_EXTRA_ATTR;
		raw_node->i.i_extra_isize =
				cpu_to_le16(F2FS_TOTAL_EXTRA_ATTR_SIZE);
	}

	if (c.feature & cpu_to_le32(F2FS_FEATURE_PRJQUOTA))
		raw_node->i.i_projid = cpu_to_le32(F2FS_DEF_PROJID);

	data_blk_nor = get_sb(main_blkaddr) +
		c.cur_seg[CURSEG_HOT_DATA] * c.blks_per_seg + 1;

	for (i = 0; i < qtype; i++)
		if (sb->qf_ino[i])
			data_blk_nor += QUOTA_DATA(i);
	if (qtype == 0)
		raw_id = raw_node->i.i_uid;
	else if (qtype == 1)
		raw_id = raw_node->i.i_gid;
	else if (qtype == 2)
		raw_id = raw_node->i.i_projid;
	else
		ASSERT(0);

	/* write two blocks */
	if (f2fs_write_default_quota(qtype, data_blk_nor, raw_id)) {
		free(raw_node);
		return -1;
	}

	for (i = 0; i < QUOTA_DATA(qtype); i++)
		raw_node->i.i_addr[get_extra_isize(raw_node) + i] =
					cpu_to_le32(data_blk_nor + i);
	raw_node->i.i_ext.fofs = 0;
	raw_node->i.i_ext.blk_addr = 0;
	raw_node->i.i_ext.len = 0;

	if (c.feature & cpu_to_le32(F2FS_FEATURE_INODE_CHKSUM))
		raw_node->i.i_inode_checksum =
			cpu_to_le32(f2fs_inode_chksum(raw_node));

	main_area_node_seg_blk_offset = get_sb(main_blkaddr);
	main_area_node_seg_blk_offset += c.cur_seg[CURSEG_HOT_NODE] *
					c.blks_per_seg + qtype + 1;

	DBG(1, "\tWriting quota inode (hot node), %x %x %x at offset 0x%08"PRIu64"\n",
			get_sb(main_blkaddr),
			c.cur_seg[CURSEG_HOT_NODE],
			c.blks_per_seg, main_area_node_seg_blk_offset);
#ifdef AMF_SNAPSHOT
	dbg_log("\n5.----\n");
	dbg_log("qtype = %u\n",qtype);
	dbg_log("将quota inode写入main_area_node_seg_blk_offset = c.cur_seg[CURSEG_HOT_NODE] * c.blks_per_seg + qtype + 1 = %u\n", main_area_node_seg_blk_offset);
	struct nvm_ret ret;
	//struct nvm_addr tempaddr = nvm_addr_dev2gen(nvmdev, main_area_node_seg_blk_offset);
	//if(nvm_cmd_write(nvmdev, &tempaddr, 1, raw_node, NULL, 1, &ret))
	if(nvm_ocssd_cmd_write(nvmdev,main_area_node_seg_blk_offset,1,raw_node, &ret))
	{
		MSG(1, "\tError: While writing the raw_node to disk!!!\n");
		free(raw_node);
		return -1;

	}
#else
	if (dev_write_block(raw_node, main_area_node_seg_blk_offset)) {
		MSG(1, "\tError: While writing the raw_node to disk!!!\n");
		free(raw_node);
		return -1;
	}
#endif
	free(raw_node);
	c.quota_inum++;
	return 0;
}

static int f2fs_update_nat_root(void)
{
	struct f2fs_nat_block *nat_blk = NULL;
	u_int64_t nat_seg_blk_offset = 0;
	enum quota_type qtype;
	int i;

	nat_blk = calloc(F2FS_BLKSIZE, 1);
	if(nat_blk == NULL) {
		MSG(1, "\tError: Calloc Failed for nat_blk!!!\n");
		return -1;
	}

	/* update quota */
	for (qtype = i = 0; qtype < F2FS_MAX_QUOTAS; qtype++) {
		if (sb->qf_ino[qtype] == 0)
			continue;
		nat_blk->entries[sb->qf_ino[qtype]].block_addr =
				cpu_to_le32(get_sb(main_blkaddr) +
				c.cur_seg[CURSEG_HOT_NODE] *
				c.blks_per_seg + i + 1);
		nat_blk->entries[sb->qf_ino[qtype]].ino = sb->qf_ino[qtype];
		i++;
	}

	/* update root */
#ifdef AMF_SNAPSHOT
		dbg_log ("\n----\n");
		dbg_log ("super_block.root_ino = %u\n", sb->root_ino);
#endif

	nat_blk->entries[get_sb(root_ino)].block_addr = cpu_to_le32(
		get_sb(main_blkaddr) +
		c.cur_seg[CURSEG_HOT_NODE] * c.blks_per_seg);
	nat_blk->entries[get_sb(root_ino)].ino = sb->root_ino;

	/* update node nat */
#ifdef AMF_SNAPSHOT
		dbg_log ("super_block.node_ino = %u\n", sb->node_ino);
#endif

	nat_blk->entries[get_sb(node_ino)].block_addr = cpu_to_le32(1);
	nat_blk->entries[get_sb(node_ino)].ino = sb->node_ino;

	/* update meta nat */
#ifdef AMF_SNAPSHOT
		dbg_log ("super_block.meta_ino = %u\n", sb->meta_ino);
#endif

	nat_blk->entries[get_sb(meta_ino)].block_addr = cpu_to_le32(1);
	nat_blk->entries[get_sb(meta_ino)].ino = sb->meta_ino;

	nat_seg_blk_offset = get_sb(nat_blkaddr);
#if defined(AMF_SNAPSHOT) && defined(AMF_META_LOGGING)
	DBG(1, "\tWriting nat root, at offset 0x%08"PRIx64"\n",
					nat_seg_blk_offset);
	dbg_log("\n8.----\n");
	dbg_log("Writing nat root, at offset 0x%08"PRIx64"\n", nat_seg_blk_offset);// 4096
	dbg_log ("[W] nat_blk_set0:\t" "%" PRIu64 "\t%u\t" "%" PRIu64 "\n",nat_seg_blk_offset, 
							F2FS_BLKSIZE/F2FS_BLKSIZE, _meta_log_blkofs_2);
	struct nvm_ret ret;
	struct nvm_addr tempaddr = nvm_addr_dev2gen(nvmdev, nat_seg_blk_offset);
	//if (nvm_cmd_write(nvmdev, &tempaddr, 1, nat_blk, NULL, 1, &ret)) {//因为要异地更新，所以首先第一个写入的是2048
	if(nvm_ocssd_cmd_write(nvmdev, nat_seg_blk_offset, 1, nat_blk, &ret)){
		MSG(1, "\tError: While writing the nat_blk set0 to disk!\n");
		free(nat_blk);
		return -1;
	}
#else
	DBG(1, "\tWriting nat root, at offset 0x%08"PRIx64"\n",
					nat_seg_blk_offset);
	if (dev_write_block(nat_blk, nat_seg_blk_offset)) {
		MSG(1, "\tError: While writing the nat_blk set0 to disk!\n");
		free(nat_blk);
		return -1;
	}
#endif
	free(nat_blk);
	return 0;
}

static block_t f2fs_add_default_dentry_lpf(void)
{
	struct f2fs_dentry_block *dent_blk;
	uint64_t data_blk_offset;

	dent_blk = calloc(F2FS_BLKSIZE, 1);
	if (dent_blk == NULL) {
		MSG(1, "\tError: Calloc Failed for dent_blk!!!\n");
		return 0;
	}

	dent_blk->dentry[0].hash_code = 0;
	dent_blk->dentry[0].ino = cpu_to_le32(c.lpf_ino);
	dent_blk->dentry[0].name_len = cpu_to_le16(1);
	dent_blk->dentry[0].file_type = F2FS_FT_DIR;
	memcpy(dent_blk->filename[0], ".", 1);

	dent_blk->dentry[1].hash_code = 0;
	dent_blk->dentry[1].ino = sb->root_ino;
	dent_blk->dentry[1].name_len = cpu_to_le16(2);
	dent_blk->dentry[1].file_type = F2FS_FT_DIR;
	memcpy(dent_blk->filename[1], "..", 2);

	test_and_set_bit_le(0, dent_blk->dentry_bitmap);
	test_and_set_bit_le(1, dent_blk->dentry_bitmap);

	data_blk_offset = get_sb(main_blkaddr);
	data_blk_offset += c.cur_seg[CURSEG_HOT_DATA] * c.blks_per_seg +
		1 + c.quota_dnum;

	DBG(1, "\tWriting default dentry lost+found, at offset 0x%08"PRIx64"\n",
			data_blk_offset);
#ifdef AMF_SNAPSHOT
	dbg_log("\n----\n");
	dbg_log("f2fs_add_default_dentry_lpf, data_blk_offset = %u\n",data_blk_offset);	
	dbg_log("写入main_blkaddr + c.cur_seg[CURSEG_HOT_DATA] * c.blks_per_seg + 1 + c.quota_dnum\n ");
	struct nvm_ret ret;
	//struct nvm_addr tempaddr = nvm_addr_dev2gen(nvmdev, data_blk_offset);
	//if(nvm_cmd_write(nvmdev, &tempaddr, 1, dent_blk, NULL, 1, &ret)){
	if(nvm_ocssd_cmd_write(nvmdev, data_blk_offset, 1, dent_blk, &ret)){
		MSG(1, "\tError While writing the dentry_blk to disk!!!\n");
		free(dent_blk);
		return 0;
	}
#else
	if (dev_write_block(dent_blk, data_blk_offset)) {
		MSG(1, "\tError While writing the dentry_blk to disk!!!\n");
		free(dent_blk);
		return 0;
	}
#endif

	free(dent_blk);
	c.lpf_dnum++;
	return data_blk_offset;
}

static int f2fs_write_lpf_inode(void)
{
	struct f2fs_node *raw_node;
	u_int64_t blk_size_bytes, main_area_node_seg_blk_offset;
	block_t data_blk_nor;
	int err = 0;

	ASSERT(c.lpf_ino);

	raw_node = calloc(F2FS_BLKSIZE, 1);
	if (raw_node == NULL) {
		MSG(1, "\tError: Calloc Failed for raw_node!!!\n");
		return -1;
	}

	raw_node->footer.nid = cpu_to_le32(c.lpf_ino);
	raw_node->footer.ino = raw_node->footer.nid;
	raw_node->footer.cp_ver = cpu_to_le64(1);
	raw_node->footer.next_blkaddr = cpu_to_le32(
			get_sb(main_blkaddr) +
			c.cur_seg[CURSEG_HOT_NODE] * c.blks_per_seg +
			1 + c.quota_inum + 1);

	raw_node->i.i_mode = cpu_to_le16(0x41c0); /* 0700 */
	raw_node->i.i_links = cpu_to_le32(2);
	raw_node->i.i_uid = cpu_to_le32(getuid());
	raw_node->i.i_gid = cpu_to_le32(getgid());

	blk_size_bytes = 1 << get_sb(log_blocksize);
	raw_node->i.i_size = cpu_to_le64(1 * blk_size_bytes);
	raw_node->i.i_blocks = cpu_to_le64(2);

	raw_node->i.i_atime = cpu_to_le32(time(NULL));
	raw_node->i.i_atime_nsec = 0;
	raw_node->i.i_ctime = cpu_to_le32(time(NULL));
	raw_node->i.i_ctime_nsec = 0;
	raw_node->i.i_mtime = cpu_to_le32(time(NULL));
	raw_node->i.i_mtime_nsec = 0;
	raw_node->i.i_generation = 0;
	raw_node->i.i_xattr_nid = 0;
	raw_node->i.i_flags = 0;
	raw_node->i.i_pino = le32_to_cpu(sb->root_ino);
	raw_node->i.i_namelen = le32_to_cpu(strlen(LPF));
	memcpy(raw_node->i.i_name, LPF, strlen(LPF));
	raw_node->i.i_current_depth = cpu_to_le32(1);
	raw_node->i.i_dir_level = DEF_DIR_LEVEL;

	if (c.feature & cpu_to_le32(F2FS_FEATURE_EXTRA_ATTR)) {
		raw_node->i.i_inline = F2FS_EXTRA_ATTR;
		raw_node->i.i_extra_isize =
			cpu_to_le16(F2FS_TOTAL_EXTRA_ATTR_SIZE);
	}

	if (c.feature & cpu_to_le32(F2FS_FEATURE_PRJQUOTA))
		raw_node->i.i_projid = cpu_to_le32(F2FS_DEF_PROJID);

	if (c.feature & cpu_to_le32(F2FS_FEATURE_INODE_CRTIME)) {
		raw_node->i.i_crtime = cpu_to_le32(time(NULL));
		raw_node->i.i_crtime_nsec = 0;
	}

	data_blk_nor = f2fs_add_default_dentry_lpf();
	if (data_blk_nor == 0) {
		MSG(1, "\tError: Failed to add default dentries for lost+found!!!\n");
		err = -1;
		goto exit;
	}
	raw_node->i.i_addr[get_extra_isize(raw_node)] = cpu_to_le32(data_blk_nor);

	if (c.feature & cpu_to_le32(F2FS_FEATURE_INODE_CHKSUM))
		raw_node->i.i_inode_checksum =
			cpu_to_le32(f2fs_inode_chksum(raw_node));

	main_area_node_seg_blk_offset = get_sb(main_blkaddr);
	main_area_node_seg_blk_offset += c.cur_seg[CURSEG_HOT_NODE] *
		c.blks_per_seg + c.quota_inum + 1;

	DBG(1, "\tWriting lost+found inode (hot node), %x %x %x at offset 0x%08"PRIu64"\n",
			get_sb(main_blkaddr),
			c.cur_seg[CURSEG_HOT_NODE],
			c.blks_per_seg, main_area_node_seg_blk_offset);
#ifdef AMF_SNAPSHOT
	dbg_log("\n6.----\n");
	dbg_log("c.quota_inum = %u\n",c.quota_inum);
	dbg_log("Writing lost+found inode (hot node)，即raw_node,到main_area_node_seg_blk_offset = main_blkaddr + c.cur_seg[CURSEG_HOT_NODE] * \
			c.blks_per_seg + c.quota_inum + 1 = %u\n",main_area_node_seg_blk_offset);
	struct nvm_ret ret;
	//struct nvm_addr tempaddr = nvm_addr_dev2gen(nvmdev, main_area_node_seg_blk_offset);
	//if(nvm_cmd_write(nvmdev, &tempaddr, 1, raw_node, NULL, 1, &ret)){
	if(nvm_ocssd_cmd_write(nvmdev, main_area_node_seg_blk_offset, 1, raw_node, &ret)){
		MSG(1, "\tError: While writing the raw_node to disk!!!\n");
		err = -1;
		goto exit;

	}
#else

	if (dev_write_block(raw_node, main_area_node_seg_blk_offset)) {
		MSG(1, "\tError: While writing the raw_node to disk!!!\n");
		err = -1;
		goto exit;
	}
#endif

	c.lpf_inum++;
exit:
	free(raw_node);
	return err;
}

static int f2fs_add_default_dentry_root(void)
{
	struct f2fs_dentry_block *dent_blk = NULL;
	u_int64_t data_blk_offset = 0;

	dent_blk = calloc(F2FS_BLKSIZE, 1);
	if(dent_blk == NULL) {
		MSG(1, "\tError: Calloc Failed for dent_blk!!!\n");
		return -1;
	}

	dent_blk->dentry[0].hash_code = 0;
	dent_blk->dentry[0].ino = sb->root_ino;
	dent_blk->dentry[0].name_len = cpu_to_le16(1);
	dent_blk->dentry[0].file_type = F2FS_FT_DIR;
	memcpy(dent_blk->filename[0], ".", 1);

	dent_blk->dentry[1].hash_code = 0;
	dent_blk->dentry[1].ino = sb->root_ino;
	dent_blk->dentry[1].name_len = cpu_to_le16(2);
	dent_blk->dentry[1].file_type = F2FS_FT_DIR;
	memcpy(dent_blk->filename[1], "..", 2);

	/* bitmap for . and .. */
	test_and_set_bit_le(0, dent_blk->dentry_bitmap);
	test_and_set_bit_le(1, dent_blk->dentry_bitmap);

	if (c.lpf_ino) {
		int len = strlen(LPF);
		f2fs_hash_t hash = f2fs_dentry_hash((unsigned char *)LPF, len);

		dent_blk->dentry[2].hash_code = cpu_to_le32(hash);
		dent_blk->dentry[2].ino = cpu_to_le32(c.lpf_ino);
		dent_blk->dentry[2].name_len = cpu_to_le16(len);
		dent_blk->dentry[2].file_type = F2FS_FT_DIR;
		memcpy(dent_blk->filename[2], LPF, F2FS_SLOT_LEN);

		memcpy(dent_blk->filename[3], LPF + F2FS_SLOT_LEN,
				len - F2FS_SLOT_LEN);

		test_and_set_bit_le(2, dent_blk->dentry_bitmap);
		test_and_set_bit_le(3, dent_blk->dentry_bitmap);
	}

	data_blk_offset = get_sb(main_blkaddr);
	data_blk_offset += c.cur_seg[CURSEG_HOT_DATA] *
				c.blks_per_seg;

#ifdef AMF_SNAPSHOT
	dbg_log ("\n9.----\n");
	dbg_log ("main_start_addr: %u (%u)\n",
		le32_to_cpu(get_sb(main_blkaddr)),
		le32_to_cpu(get_sb(main_blkaddr)) * 4096);
	dbg_log (" * hot_data_start_addr: %u (%u)\n",
		(le32_to_cpu(get_sb(main_blkaddr)) + c.cur_seg[CURSEG_HOT_DATA] * c.blks_per_seg),
		(le32_to_cpu(get_sb(main_blkaddr)) + c.cur_seg[CURSEG_HOT_DATA] * c.blks_per_seg) * 4096);

	dbg_log ("[W] dentry_blk:\t" "%" PRIu64 "\n", data_blk_offset);
	dbg_log("写入root_inode的dentry到main_blkaddr+c.cur_seg[CURSEG_HOT_DATA]*c.blks_per_seg中\n");

	struct nvm_ret ret;
	//struct nvm_addr tempaddr = nvm_addr_dev2gen(nvmdev, data_blk_offset);
	//if (nvm_cmd_write(nvmdev, &tempaddr, 1, dent_blk, NULL, 1, &ret)) {
	if(nvm_ocssd_cmd_write(nvmdev, data_blk_offset, 1, dent_blk, &ret)){
			MSG(1, "\tError: While writing the dentry_blk to disk!!!\n");
			free(dent_blk);
			return -1;
		}
			
#else
	DBG(1, "\tWriting default dentry root, at offset 0x%08"PRIx64"\n",
				data_blk_offset);
	if (dev_write_block(dent_blk, data_blk_offset)) {
		MSG(1, "\tError: While writing the dentry_blk to disk!!!\n");
		free(dent_blk);
		return -1;
	}
#endif

	free(dent_blk);
	return 0;
}

static int f2fs_create_root_dir(void)
{
	enum quota_type qtype;
	int err = 0;

	err = f2fs_write_root_inode();
	if (err < 0) {
		MSG(1, "\tError: Failed to write root inode!!!\n");
		goto exit;
	}

	for (qtype = 0; qtype < F2FS_MAX_QUOTAS; qtype++)  {
		if (sb->qf_ino[qtype] == 0)
			continue;
		err = f2fs_write_qf_inode(qtype);
		if (err < 0) {
			MSG(1, "\tError: Failed to write quota inode!!!\n");
			goto exit;
		}
	}

	if (c.feature & cpu_to_le32(F2FS_FEATURE_LOST_FOUND)) {
		err = f2fs_write_lpf_inode();
		if (err < 0) {
			MSG(1, "\tError: Failed to write lost+found inode!!!\n");
			goto exit;
		}
	}

#ifndef WITH_ANDROID
	err = f2fs_discard_obsolete_dnode();
	if (err < 0) {
		MSG(1, "\tError: Failed to discard obsolete dnode!!!\n");
		goto exit;
	}
#endif

	err = f2fs_update_nat_root();
	if (err < 0) {
		MSG(1, "\tError: Failed to update NAT for root!!!\n");
		goto exit;
	}

	err = f2fs_add_default_dentry_root();
	if (err < 0) {
		MSG(1, "\tError: Failed to add default dentries for root!!!\n");
		goto exit;
	}
exit:
	if (err)
		MSG(1, "\tError: Could not create the root directory!!!\n");

	return err;
}

int f2fs_format_device(void)
{
	int err = 0;

#if defined(AMF_SNAPSHOT) && defined(AMF_META_LOGGING)
			/* For AMF, two sectors are reserved for check points.
			 * The metalog begins after two check points */
			_meta_log_blkofs = 
				(c.segs_per_sec * c.blks_per_seg) * 
				(NR_SUPERBLK_SECS + NR_MAPPING_SECS);//4 * 512 = 2048
			_meta_log_blkofs_2 = _meta_log_blkofs;// 2048
			dbg_log ("_meta_log_blkofs = %llu \n", _meta_log_blkofs);
			dbg_log ("_meta_log_byteofs = %llu \n", _meta_log_blkofs_2);

						
			nvmdev = nvm_dev_open(nvm_dev_path);//打开nvm设备
			if(!nvmdev)
				perror("error :nvm_dev_open\n");
			//nvm_dev_pr(nvmdev);
			nvmgeo = nvm_dev_get_geo(nvmdev);


			NVM_GEO_MIN_WRITE_PAGE = nvmgeo->nplanes * nvmgeo->nsectors;
			nvm_buf_w_nbytes = NVM_GEO_MIN_WRITE_PAGE * nvmgeo->sector_nbytes;
			pmode = nvmgeo->nplanes;
			nvm_write_addrs = (struct nvm_addr*)malloc(sizeof(struct nvm_addr) * NVM_GEO_MIN_WRITE_PAGE);
			
			dbg_log("NVM_GEO_MIN_WRITE_PAGE = %d, pmode = %d\n", NVM_GEO_MIN_WRITE_PAGE, pmode);
			

			/*
			//测试读写
			struct nvm_addr addr1;
			char *buf_w = NULL,*buf_r = NULL;
			size_t buf_w_nbytes, buf_r_nbytes;
			int ret;
			const int naddrs = nvmgeo->nplanes * nvmgeo->nsectors;
			printf("naddrs = %d\n",naddrs);
			
			buf_w_nbytes = naddrs * nvmgeo->sector_nbytes;
			buf_r_nbytes = nvmgeo->sector_nbytes;
			
			
			addr1 = nvm_addr_dev2gen(nvmdev, 550);
			nvm_addr_pr(addr1);//0x0000 0001 0000 0000
			
			buf_w = nvm_buf_alloc(nvmgeo, buf_w_nbytes);
			buf_r = nvm_buf_alloc(nvmgeo, buf_r_nbytes);
			if (!buf_w) {
				printf("error: nvm_buf_alloc\n");
				
			}
			if (!buf_r) {
				printf("error:nvm_buf_alloc");
				
			}
			
			//nvm_buf_fill(buf_w, buf_w_nbytes);
			memset(buf_r, 0, buf_r_nbytes);
			
			//err = nvm_addr_erase(nvmdev, &addr1, 1, 1, &ret);
			//err = nvm_addr_write(nvmdev, &addr1, 1, buf_w, NULL, 1, &ret);
			err = nvm_addr_read(nvmdev, &addr1, 1, buf_r, NULL, 1, &ret);
			//printf("buf_w = %s\n",buf_w);
			printf("buf_r = %s\n",buf_r);
			struct amf_map_blk* ptr = (struct amf_map_blk*)buf_r;
			printf("ptr->magic = %d,ptr->index = %d,ptr->mapping[0] = %d\n",ptr->magic,ptr->index,ptr->mapping[0]);
			*/
			

			
#endif

	err= f2fs_prepare_super_block();
	if (err < 0) {
		MSG(0, "\tError: Failed to prepare a super block!!!\n");
		goto exit;
	}

	if (c.trim) {
		err = f2fs_trim_devices();
		if (err < 0) {
			MSG(0, "\tError: Failed to trim whole device!!!\n");
			goto exit;
		}
	}

	err = f2fs_init_sit_area();
	if (err < 0) {
		MSG(0, "\tError: Failed to Initialise the SIT AREA!!!\n");
		goto exit;
	}

	err = f2fs_init_nat_area();
	if (err < 0) {
		MSG(0, "\tError: Failed to Initialise the NAT AREA!!!\n");
		goto exit;
	}

	err = f2fs_create_root_dir();
	if (err < 0) {
		MSG(0, "\tError: Failed to create the root directory!!!\n");
		goto exit;
	}

	err = f2fs_write_check_point_pack();
	if (err < 0) {
		MSG(0, "\tError: Failed to write the check point pack!!!\n");
		goto exit;
	}

	err = f2fs_write_super_block();
	if (err < 0) {
		MSG(0, "\tError: Failed to write the Super Block!!!\n");
		goto exit;
	}

#ifdef AMF_SNAPSHOT
		err = f2fs_write_snapshot();
		if (err < 0) {
			MSG(0, "\tError: Failed to write the snapshot!!!\n");
			goto exit;
		}

		/*
		//测试读写
					struct nvm_addr addr1;
					char *buf_w = NULL,*buf_r = NULL;
					size_t buf_w_nbytes, buf_r_nbytes;
					struct nvm_ret ret;
					const int naddrs = nvmgeo->nplanes * nvmgeo->nsectors;
					printf("naddrs = %d\n",naddrs);
					
					buf_w_nbytes = naddrs * nvmgeo->sector_nbytes;
					buf_r_nbytes = nvmgeo->sector_nbytes;
					
					
					addr1 = nvm_addr_dev2gen(nvmdev, 541);
					nvm_addr_pr(addr1);//0x0000 0001 0000 0000
					
					buf_w = nvm_buf_alloc(nvmgeo, buf_w_nbytes);
					buf_r = nvm_buf_alloc(nvmgeo, buf_r_nbytes);
					if (!buf_w) {
						printf("error: nvm_buf_alloc\n");
						
					}
					if (!buf_r) {
						printf("error:nvm_buf_alloc");
						
					}
					
					//nvm_buf_fill(buf_w, buf_w_nbytes);
					memset(buf_r, 0, buf_r_nbytes);
					
					//err = nvm_addr_erase(nvmdev, &addr1, 1, 1, &ret);
					//err = nvm_addr_write(nvmdev, &addr1, 1, buf_w, NULL, 1, &ret);
					err = nvm_addr_read(nvmdev, &addr1, 1, buf_r, NULL, 1, &ret);
					printf("err = %d\n",err);
					printf("ret.status = 0x%x, ret.result = 0x%x\n",ret.status, ret.result);
					//printf("buf_w = %s\n",buf_w);
					printf("buf_r = %s\n",buf_r);
					struct amf_map_blk* ptr = (struct amf_map_blk*)buf_r;
					printf("ptr->magic = %d,ptr->index = %d,ptr->mapping[0] = %d\n",ptr->magic,ptr->index,ptr->mapping[0]);
		*/
#endif


exit:
	if (err){
		MSG(0, "\tError: Could not format the device!!!\n");		
	}
#ifdef AMF_SNAPSHOT
		
	
	if(nvm_buf_w != NULL){
		nvm_ocssd_cmd_write(nvmdev, 0, 0, NULL, NULL);
		nvm_buf_free(nvmdev, nvm_buf_w);
	}

	free(nvm_write_addrs);	
	nvm_dev_close(nvmdev);
#endif

	return err;
}


#ifdef AMF_SNAPSHOT

size_t nvm_buf_diff_qrk(char *expected, char *actual, size_t nbytes,
			size_t nbytes_oob,
			size_t nbytes_qrk)
{
	size_t diff = 0,i;

	for (i = 0; i < nbytes; ++i) {
		if ((i % nbytes_oob) < nbytes_qrk)
			continue;

		if (expected[i] != actual[i])
			++diff;
	}

	return diff;
}

int f2fs_write_snapshot(void)
{
	/*__le32* ptr_mapping_table = NULL;*/
	u_int32_t nr_segment_count_meta = 0;
	u_int32_t nr_mapping_entries = 0;
	u_int32_t nr_map_blks = 0;

	u_int32_t loop = 0;
	u_int32_t segment0_blkaddr = 0;

	struct amf_map_blk* ptr_map_blks;

	/* (1) obtain the number of mapping entries */
	nr_segment_count_meta = 
		le32_to_cpu(sb->segment_count_ckpt) + 
		le32_to_cpu(sb->segment_count_sit) + 
		le32_to_cpu(sb->segment_count_nat) +
		le32_to_cpu(sb->segment_count_ssa);

	nr_mapping_entries = 
		nr_segment_count_meta * c.blks_per_seg;

	nr_map_blks = nr_mapping_entries / 1020;
	if (nr_mapping_entries % 1020 != 0) {
		nr_map_blks++;
	}


	dbg_log ("\n----\n");
	dbg_log ("nr_segment_count_meta: %u\n", nr_segment_count_meta);
	dbg_log ("nr_mapping_entries: %u (%u Bytes)\n", nr_mapping_entries, nr_mapping_entries * sizeof (__le32));
	dbg_log ("nr_map_blks: %u\n", nr_map_blks);//29

	/* (2) create the mapping table */
	if ((ptr_map_blks = (struct amf_map_blk*)malloc (
			sizeof (struct amf_map_blk) * nr_map_blks)) == NULL) {
		dbg_log ("\tError: errors occur allocating memory space for the map-blk table\n");
		return -1;
	} 
	for (loop = 0; loop < nr_map_blks; loop++) {
		ptr_map_blks[loop].magic = cpu_to_le32 (0xEF);
		ptr_map_blks[loop].ver = cpu_to_le32(0);
		ptr_map_blks[loop].index = cpu_to_le32 (loop * 1020);
		/*ptr_map_blks[loop].len = cpu_to_le32(1020);*/
		ptr_map_blks[loop].dirty = cpu_to_le32(0);
		memset (ptr_map_blks[loop].mapping, (__le32)-1, sizeof (__le32) * 1020);
	}

	/* (3) initialize the mapping table */
	segment0_blkaddr = le32_to_cpu (sb->segment0_blkaddr);
	dbg_log ("segment0_blkaddr: %u\n", segment0_blkaddr);

	/* set some default entries (logical <= physical) */


		unsigned long long ofs = _meta_log_blkofs;//2048
		unsigned long long map_ofs;

		map_ofs = ofs+2048-segment0_blkaddr;//为什么是加2048？？
		ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020] = cpu_to_le32(ofs+0);
	//	dbg_log("map_ofs = %u \n", map_ofs);
	//	dbg_log("ptr_map_blks[%u].mapping[%u] = %u\n",map_ofs/1020,map_ofs%1020,ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020]);
		
		map_ofs = ofs+0-segment0_blkaddr;
		ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020] = cpu_to_le32(ofs+1);
		//dbg_log("map_ofs = %u \n", map_ofs);
		//dbg_log("ptr_map_blks[%u].mapping[%u] = %u\n",map_ofs/1020,map_ofs%1020,ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020]);

		
		map_ofs = ofs+1-segment0_blkaddr;
		ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020] = cpu_to_le32(ofs+2);
		//dbg_log("map_ofs = %u \n", map_ofs);
		//dbg_log("ptr_map_blks[%u].mapping[%u] = %u\n",map_ofs/1020,map_ofs%1020,ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020]);

		
		map_ofs = ofs+2-segment0_blkaddr;
		ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020] = cpu_to_le32(ofs+3);
		//dbg_log("map_ofs = %u \n", map_ofs);
		//dbg_log("ptr_map_blks[%u].mapping[%u] = %u\n",map_ofs/1020,map_ofs%1020,ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020]);
		
		map_ofs = ofs+3-segment0_blkaddr;
		ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020] = cpu_to_le32(ofs+4);
		//dbg_log("map_ofs = %u \n", map_ofs);
		//dbg_log("ptr_map_blks[%u].mapping[%u] = %u\n",map_ofs/1020,map_ofs%1020,ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020]);
		
		map_ofs = ofs+4-segment0_blkaddr;
		ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020] = cpu_to_le32(ofs+5);
		//dbg_log("map_ofs = %u \n", map_ofs);
		//dbg_log("ptr_map_blks[%u].mapping[%u] = %u\n",map_ofs/1020,map_ofs%1020,ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020]);

		
		map_ofs = ofs+5-segment0_blkaddr;
		ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020] = cpu_to_le32(ofs+6);
		//dbg_log("map_ofs = %u \n", map_ofs);
		//dbg_log("ptr_map_blks[%u].mapping[%u] = %u\n",map_ofs/1020,map_ofs%1020,ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020]);
		
		map_ofs = ofs+6-segment0_blkaddr;
		ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020] = cpu_to_le32(ofs+7);
		//dbg_log("map_ofs = %u \n", map_ofs);
		//dbg_log("ptr_map_blks[%u].mapping[%u] = %u\n",map_ofs/1020,map_ofs%1020,ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020]);
		
		map_ofs = ofs+7-segment0_blkaddr;
		ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020] = cpu_to_le32(ofs+8);
		//dbg_log("map_ofs = %u \n", map_ofs);
		//dbg_log("ptr_map_blks[%u].mapping[%u] = %u\n",map_ofs/1020,map_ofs%1020,ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020]);
		
		map_ofs = ofs+512-segment0_blkaddr;
		ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020] = cpu_to_le32(ofs+9);
		//dbg_log("map_ofs = %u \n", map_ofs);
		//dbg_log("ptr_map_blks[%u].mapping[%u] = %u\n",map_ofs/1020,map_ofs%1020,ptr_map_blks[map_ofs/1020].mapping[map_ofs%1020]);

		//ptr_mapping_table[ofs+2048-segment0_blkaddr] = cpu_to_le32(ofs+0);
		//ptr_mapping_table[ofs+0-segment0_blkaddr] = cpu_to_le32(ofs+1);
		//ptr_mapping_table[ofs+1-segment0_blkaddr] = cpu_to_le32(ofs+2);
		//ptr_mapping_table[ofs+2-segment0_blkaddr] = cpu_to_le32(ofs+3);
		//ptr_mapping_table[ofs+3-segment0_blkaddr] = cpu_to_le32(ofs+4);
		/*ptr_mapping_table[ofs+4-segment0_blkaddr] = cpu_to_le32(ofs+5);*/
		//ptr_mapping_table[ofs+5-segment0_blkaddr] = cpu_to_le32(ofs+6);
		/*ptr_mapping_table[ofs+6-segment0_blkaddr] = cpu_to_le32(ofs+7);*/
		/*ptr_mapping_table[ofs+7-segment0_blkaddr] = cpu_to_le32(ofs+8);*/
		/*ptr_mapping_table[ofs+512-segment0_blkaddr] = cpu_to_le32(ofs+9);*/


	/* (4) write the mapping table & summary table */
	//_mapping_byteofs = ((c.segs_per_sec * c.blks_per_seg) * NR_SUPERBLK_SECS) * F2FS_BLKSIZE; 

	_mapping_byteofs = ((c.segs_per_sec * c.blks_per_seg) * NR_SUPERBLK_SECS); 
	struct nvm_ret ret;
	//struct nvm_addr tempaddr;
	for (loop = 0; loop < nr_map_blks; loop++) {//将mapping写入设备
		dbg_log("[W] mapping_table:\t%u\t%u\n", _mapping_byteofs, F2FS_BLKSIZE);
		//tempaddr = nvm_addr_dev2gen(nvmdev, _mapping_byteofs);
		//if (nvm_cmd_write(nvmdev, &tempaddr, 1, (__le32*)(ptr_map_blks + loop), NULL, 1, &ret)) {
	struct amf_map_blk * ptr = (ptr_map_blks+loop);
		dbg_log("ptr->magic = %d,ptr->ver = %d,ptr->index = %d,ptr->mapping[0] = %d \n", ptr->magic,ptr->ver, ptr->index, ptr->mapping[0]);
	
		if(nvm_ocssd_cmd_write(nvmdev, _mapping_byteofs, 1, (__le32*)(ptr_map_blks + loop), &ret)){
			MSG(0, "\tError: While writing the mapping table to disk!!!\n");
			return -1;
		}

	/*	if (dev_write_block ((__le32*)(ptr_map_blks + loop), _mapping_byteofs)) {
			MSG(0, "\tError: While writing the mapping table to disk!!!\n");
			return -1;
		}
	*/
		
		_mapping_byteofs ++;
	}
	
/*
	struct amf_map_blk * ptr = (ptr_map_blks+0);
		dbg_log("ptr->magic = %d,ptr->ver = %d,ptr->index = %d,ptr->mapping[0] = %d \n", ptr->magic,ptr->ver, ptr->index, ptr->mapping[0]);
		
		                 
		struct amf_map_blk* ptrread;
		if ((ptrread = (struct amf_map_blk*)malloc (
			sizeof (struct amf_map_blk) * 1)) == NULL) {
		dbg_log ("\tError: errors occur allocating memory space for the map-blk table\n");
		return -1;
		} 
		memset (ptrread, (__le32)0, sizeof (struct amf_map_blk));
		//if (dev_read_block(ptrread,_mapping_byteofs)) {
		//	MSG(0, "\tError: While writing the mapping table to disk!!!\n");
		//	return -1;
		//}
		_mapping_byteofs = ((c.segs_per_sec * c.blks_per_seg) * NR_SUPERBLK_SECS);
		dbg_log("_mapping_byteofs = %d\n",_mapping_byteofs);
		struct nvm_addr temp = nvm_addr_dev2gen(nvmdev, _mapping_byteofs);
		struct nvm_ret tempret;
		int res = nvm_cmd_read(nvmdev, &temp, 1, ptrread, NULL, 1, &tempret);
		if(res < 0){
			dbg_log("res = %d\n",res);
		}
		
		dbg_log("ptrread->magic=%u,ptrread->index=%u,ptrread->mapping[0]=%u\n",ptrread->magic,ptrread->index,ptrread->mapping[1]);
		free(ptrread);
*/



/*测试读写接口
	char *buf_w = NULL, *buf_r = NULL;
	const int naddrs = nvmgeo->nplanes * nvmgeo->nsectors;
	struct nvm_addr addrs[naddrs];
	struct nvm_addr blk_addr = { .val = 0 };
	ssize_t res;
	size_t buf_w_nbytes, buf_r_nbytes;
	int pmode = NVM_FLAG_PMODE_SNGL;
	int failed = 1;
	int i;
	
	buf_w_nbytes = naddrs * nvmgeo->sector_nbytes;
	buf_r_nbytes = nvmgeo->sector_nbytes;

	buf_w = nvm_buf_alloc(nvmdev, buf_w_nbytes, NULL); 
	nvm_buf_fill(buf_w, buf_w_nbytes);

	buf_r = nvm_buf_alloc(nvmdev, buf_r_nbytes, NULL);
	
		for ( i = 0; i < naddrs; ++i) {
			addrs[i].ppa = blk_addr.ppa;

			addrs[i].g.sec = i % nvmgeo->nsectors;
			addrs[i].g.pl = (i / nvmgeo->nsectors) % nvmgeo->nplanes;
		}
		res = nvm_cmd_write(nvmdev, addrs, naddrs, buf_w,
				    NULL, pmode, &ret);


	size_t pl,sec;
	
		for (  pl = 0; pl < nvmgeo->nplanes; ++pl) {
			for ( sec = 0; sec < nvmgeo->nsectors; ++sec) {
				struct nvm_addr addr;
				size_t buf_diff = 0;

				int bw_offset = sec * nvmgeo->sector_nbytes + \
						pl * nvmgeo->nsectors * \
						nvmgeo->sector_nbytes;
				
				addr.ppa = blk_addr.ppa;
				addr.g.pl = pl;
				addr.g.sec = sec;

				memset(buf_r, 0, buf_r_nbytes);
		

				res = nvm_cmd_read(nvmdev, &addr, 1, buf_r,
						     NULL, pmode, &ret);

				buf_diff = nvm_buf_diff_qrk(buf_r,
							    buf_w + bw_offset,
							    buf_r_nbytes,
							    nvmgeo->g.meta_nbytes,
							    4);
				dbg_log("bur_r = %s\n", buf_r);
				if (buf_diff)
					dbg_log("Read failure: buffer mismatch");
			}
		}
	
	nvm_buf_free(nvmdev, buf_r);
	nvm_buf_free(nvmdev, buf_w);
					
*/
	

	/* (5) free the memory space */
	free (ptr_map_blks);
	/*free (ptr_mapping_table);*/

	return 0;
}
#endif

