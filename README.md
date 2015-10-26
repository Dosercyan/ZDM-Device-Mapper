
## Introduction

Device Mapper for Zoned based devices: ZDM for short.

This project aims to present a traditional block device for Host Aware and
Host Managed drives.

## Architecture

ZDM treats a zoned device as a collection of 1024 zones [256GiB], referred to internally as 'megazones'. The last megazone may be less than 1024 zones in size. Each megazone reserves a minimum 8 zones for meta data and over-provisioning [less than 1% of a disk].

Device trim [aka discard] support is enabled by default. It is recommended to increase the over-provision ratio when discard support is disabled.

The initial implementation focuses on drives with same sized zones of 256MB which is 65536 4k blocks. In future the zone size of 256MB will be relaxed to allow any size of zone as long as they are all the same.
Internally all addressing is on 4k boundaries. Currently a 4k PAGE_SIZE is assumed. Architectures with 8k (or other) PAGE_SIZE values have not been tested and are likely broken at the moment.

Host Managed drives should work if the zone type at the start of the partition is Conventional, or Preferred.

## Software Requirements

  - Current Linux Kernel (4.2) with ZDM patches
  - Recommended: sg3utils (1.41 or later)

## Caveat Emptor - Warning

  - ZDM software is a work in progress, subject to change without notice and is provided without warranty for any particular intended purpose beyond testin and reference. The ZDM may become unstable, resulting in system crash or hang, including the possibiliy of data loss.  No responsibility for lost data or system damage is assummed by the creators or distributors of ZDM software.  Users explicitly assume all risk associated with running the ZDM software.

## Current restrictions/assumptions

  - Zone size (256MiB).
  - 4k page / block size.
  - Host Aware, Conventional
  - Host Managed w/partition starting on a Conventional, or Preferred zone type.
  - Currently 1 GiB of RAM per drive is recommeneded.

## Userspace utilities
  - zdm-tools: zdmadm, zdm-status, zdm-zones ...
  - zbc/zac tools (sd_* tools)

## Typical Setup

  - Reset all WPs on drive:
```
      sg_reset_wp --all /dev/sdX
```
or
```
      sd_reset_wp -1 /dev/sdX
```
or
```
      sd_reset_wp ata -1 /dev/sdX
```

  - Partition the drive to start the partition at a WP boundary.
```
      parted /dev/sdX
      mklabel gpt
      mkpart primary 256MiB 7452GiB
```

  - Place ZDM drive mapper on /dev/sdX
```
      zdmadm -c /dev/sdX1
```

  - Format:
```
      mkfs -t ext4 -E discard /dev/mapper/zdm_sdX1
```
or
```
      mkfs -t ext4 -b 4096 -g 32768 -G 32 \
        -E offset=0,num_backup_sb=0,packed_meta_blocks=1,discard \
        -O flex_bg,extent,sparse_super2 /dev/mapper/zdm_sdX1
```

  - Mounting the filesystem.
```
      mount -o discard /dev/mapper/zdm_sdX1 /mnt/zdm_sdX1
```
 
Building:
  - Normal kernel build with CONFIG_DM_ZONED and CONFIG_BLK_ZONED_CTRL enabled.

## Standards Versions Supported

ZAC/ZBC standards are still being developed. Changes to the command set and
command interface can be expected before the final public release.

## License

ZDM is distributed under the terms of GPL v2 or any later version.

ZDM and all and all utilities here in are distributed "as is," without technical
support, and WITHOUT ANY WARRANTY, without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. Along with ZDM, you should
have received a copy of the GNU General Public License.
If not, please see http://www.gnu.org/licenses/.

## Contact and Bug Reports

 - Adrian Palmer [adrian.palmer@seagate.com](mailto:adrian.palmer@seagate.com)
 
## Observations and Known Issues

 - Issue
    * System: 	Dual 6 core Xeon 64 GB RAM LSI HBA
    * Software:	Ubuntu 14.10 (GNU/Linux 4.2.0-rc8-zdm x86_64)
    * File system: ext4 ``mkfs -E discard`` and  ``mount -o discard``
    * Test load: Python 2.7 file create, appending writes, close. OS deletes No file reads.
    * Results: No data corruption detected. Kenrel panic with heavy write/delete load with file system > 75% full.

 - Issue
    * System: 	Dual 6 core Xeon 64 GB RAM LSI HBA
    * Software:	Ubuntu 14.10 (GNU/Linux 4.2.0-rc8-zdm x86_64)
    * File system: ext4 ``mkfs -E discard`` and  ``mount -o discard``
    * Test: LevelDB PUT/GET
    * Method: LeveDB Python binding.  Sequential PUTs, random GETs. No DELs
    * Results: No data corruption, no errors.
    * Overall data throughput slightly (15%) better than 4TB desktop SATA. See table.
|-|zdm|desktop|delta|
|total bytes put/get in 8000 seconds|252969405|221489766|1.142126833|
|median put MB/sec|37.237|30.073|1.238220331|
|average put MB/sec|38.13009984|30.08739469|1.267311451|
|median get MB/sec|101.048|97.415|1.037294051|
|average get MB/sec|1054.253523|1355.445637|0.7777910782|

 - Issue 
    * System: SDS-6037R-E1R16L using onboard LSI/Avago 2308 HBA chipset
    * Test: Single drive with ext4 running multiple 50GB file writes, compares, and deletes.
    * Result: Observed no degradation in performance or miscompares.

 - Issue 
    * System: SDS-6037R-E1R16L using onboard LSI/Avago 2308 HBA chipset
    * Test: RAID 5 across 4 drives with ZDM
    * Result: Observed degrading performance and occasional miscompare. 
    * Notes: ZDM is currently not optimized for the complexity of RAID5. Furthermore, it appears that one drive is stressed more during writes to the filesystem than the other 3 drives.

## Changes from Initial Release

  - ZDM #91
    * Kernel from v4.2 tag is pre-patched.
    * Added version number to track alpha/beta releases.
    * Numerous bug fixes and better stability.
    * Much improved GC / zone compaction handling.
    * Patch set is cleaner and better defined.
    * Patches provided to apply cleanly against 4.1 (stable), 4.2, and 4.3.

