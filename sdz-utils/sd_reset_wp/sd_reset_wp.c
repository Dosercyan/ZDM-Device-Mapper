/*
 * Linux Kernel ioctl tools for ZAC/ZBC devices.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include "is_mounted.h"
#include "sdtools.h"

#define PGM_NAME "sd_reset_wp"

int do_reset_wp_ioctl(const char * pathname, uint64_t lba, int do_ata)
{
	int rc = -4;
        int fd = open(pathname, O_RDWR);
        if (fd != -1) {
		uint64_t iolba = lba;
		fprintf(stderr, "ioctl: %s\n", pathname );

		if (do_ata) {
			iolba |= 1;
		} else {
			iolba &= ~1ul;
		}

		rc = ioctl(fd, SCSI_IOCTL_RESET_WP, iolba);
		if (rc != -1) {
			fprintf(stderr, "reset wp on %" PRIx64 "%s okay\n",
				lba, (lba == ~0ul) ? " (all)" : "" );
		} else {
			fprintf(stderr, "ERR: %d -> %s\n\n", errno, strerror(errno));
		}
                close(fd);
        } else {
                fprintf(stderr, "%s\n\n", strerror(errno));
        }

	return rc;
}

/*
 *
 */
int main(int argc, char *argv[])
{
        uint64_t lba = 0;
        char * fname = NULL;
	int ii;
	int do_ata = 0;
	char buf[80];
	int rc;
	int flags;

	for (ii = 1; ii < argc; ii++) {
		if (0 == strcmp(argv[ii], "ata") ) {
			do_ata = 1;
		} else {
			if (0 == strcmp(argv[ii], "~0")) {
				lba = ~0ul;
			} else if (0 == strcmp(argv[ii], "~1")) {
				lba = ~1ul;
			} else {
				char * endptr ;
				uint64_t tmp = strtoull(argv[ii], &endptr, 0);
				if (0 == *endptr) {
					lba = tmp;
				} else if (!fname) {
					fname = argv[ii];
				}
			}
		}
	}

	if (argc == 1 || !fname) {
		printf(PGM_NAME " %d.%d\n",
		       sdtools_VERSION_MAJOR, sdtools_VERSION_MINOR );
		printf("Usage:\n"
		       "  " PGM_NAME " [ata] <lba> <device>\n\n"
                       "where:\n"
		       "   ata will cause ATA ZAC commands to be used instead of SCSI ZBC.\n"
		       "   lba can be ~0 or ~1 for to reset all WPs\n\n"
                       "examples:\n\n"
		       "  sd_reset_wp ata -1 /dev/sdn\n"
		       "     reset all wps on /dev/sdn using ATA ZAC commands\n"
		       "  sd_reset_wp -1 /dev/sdn\n"
		       "     reset all wps on /dev/sdn using SCSI ZBC commands\n"
		       "  sd_reset_wp 0x80000 /dev/sdn\n"
		       "     reset wp on the zone starting at lba 0x80000 on /dev/sdn\n");
		return 1;
	}

	rc = is_anypart_mounted(fname, &flags, buf, sizeof(buf));
	if (0 == rc) {
		if (0 == flags) {
			rc = do_reset_wp_ioctl(fname, lba, do_ata);
		} else {
			fprintf(stderr, "Locked: %s: Flags %x\n", buf, flags );
		}
	} else {
		fprintf(stderr, "'is mounted' check failed: %d\n", rc );
	}
	return rc;
}