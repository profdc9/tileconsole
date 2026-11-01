/* tilefs configuration of FatFs R0.16 (written for tilefs; the FatFs files beside this one are ChaN's, unmodified).
 * FatFs is reached through tilefs, which does its own path handling, handle tables and busy checks, so the options that
 * duplicate those are off. Long file names and exFAT are on, formatting is on, there is no malloc and no locking. */
#ifndef FFCONF_DEF
#define FFCONF_DEF	80386	/* Revision ID */

#define FF_FS_READONLY	0
#define FF_FS_MINIMIZE	0
#define FF_USE_FIND		0
#define FF_USE_MKFS		1
#define FF_USE_FASTSEEK	0
#define FF_USE_EXPAND	0
#define FF_USE_CHMOD	0
#define FF_USE_LABEL	0
#define FF_USE_FORWARD	0
#define FF_USE_STRFUNC	0
#define FF_PRINT_LLI	0
#define FF_PRINT_FLOAT	0
#define FF_STRF_ENCODE	0

#define FF_CODE_PAGE	437
#define FF_USE_LFN		1	/* a static buffer: no malloc, one call into FatFs at a time */
#define FF_MAX_LFN		255
#define FF_LFN_UNICODE	0
#define FF_LFN_BUF		255
#define FF_SFN_BUF		12
#define FF_FS_RPATH		0
#define FF_PATH_DEPTH	10

#define FF_VOLUMES		2	/* drives "0:" and "1:": the fatfs backend gives each mount a drive */
#define FF_STR_VOLUME_ID	0
#define FF_MULTI_PARTITION	0
#define FF_MIN_SS		512
#define FF_MAX_SS		512
#define FF_LBA64		0
#define FF_MIN_GPT		0x10000000
#define FF_USE_TRIM		0
#define FF_FS_TINY		0
#define FF_FS_EXFAT		1
#define FF_FS_NORTC		0	/* get_fattime() is in fs_fat.c and takes its time from a hook */
#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2026
#define FF_FS_CRTIME	0
#define FF_FS_NOFSINFO	0

#define FF_FS_LOCK		0
#define FF_FS_REENTRANT	0
#define FF_FS_TIMEOUT	1000

#endif /* FFCONF_DEF */
