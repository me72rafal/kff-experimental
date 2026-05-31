/*
 * FAT directory sorting.
 *
 * This file uses FatFs internals and must be built in the same translation
 * unit as ff.c.
 *
 * Scope:
 * - sorts the current FatFs directory
 * - puts directories before files
 * - sorts names alphabetically: punctuation, spaces, and letter case are
 *   ignored for the primary order; when names have the same primary order,
 *   spaces come after non-spaces and byte order breaks remaining ties
 * - caller supplies a FATSORT_WORK_SIZE work buffer and a
 *   FATSORT_MOVE_WORK_SIZE move scratch buffer
 * - caller controls cancellation by stopping f_sortnext() calls and closing
 * - uses bounded batch selection of sortable directory-entry blocks
 * - preserves deleted entries as holes
 * - treats volume labels, dot entries, and malformed LFN chains as barriers
 * - ANSI/OEM LFN mode only; Unicode LFN mode is not supported
 * - FAT12/FAT16/FAT32 only; exFAT is not supported
 *
 * Note: This is not crash-safe. Power must not be interrupted during sorting.
 */

#include <string.h>

#if !FF_USE_LFN
#error "fatsort requires FF_USE_LFN enabled"
#endif

#if FF_LFN_UNICODE
#error "fatsort expects ANSI/OEM filenames (FF_LFN_UNICODE == 0)"
#endif

#if FF_FS_EXFAT
#error "fatsort does not support exFAT"
#endif

#define FATSORT_CANDIDATE_WORK	(FATSORT_WORK_SIZE - FATSORT_WORK_SCRATCH_SIZE)

#if FATSORT_DIR_ENTRY_SIZE != SZDIRE
#error "FATSORT_DIR_ENTRY_SIZE must match FatFs directory entry size"
#endif

/*
 * FatFs internals expected from ff.c.
 */
static FRESULT move_window(FATFS *fs, LBA_t sect);
static FRESULT sync_window(FATFS *fs);
static FRESULT dir_sdi(DIR *dp, DWORD ofs);
static FRESULT dir_next(DIR *dp, int stretch);
static FRESULT dir_read(DIR *dp, int vol);
static void get_fileinfo(DIR *dp, FILINFO *fno);

typedef enum
{
	FATSORT_END,
	FATSORT_BARRIER,
	FATSORT_HOLE,
	FATSORT_BLOCK
} FATSORT_KIND;

typedef enum
{
	FATSORT_PHASE_OUT,
	FATSORT_PHASE_SCAN,
	FATSORT_PHASE_ROTATE
} FATSORT_PHASE;

static unsigned char fatsort_next_primary(const char **p)
{
	unsigned char c;

	while ((c = *(*p)++) != 0) {
		if ((unsigned char)(c - '0') <= 9) return c;
		c &= ~('a' - 'A');
		if ((unsigned char)(c - 'A') <= 'Z' - 'A') return c;
	}
	return 0;
}

static int fatsort_cmp_name(const FILINFO *a, const FILINFO *b)
{
	const char *sa = a->fname;
	const char *sb = b->fname;
	const char *pa = sa;
	const char *pb = sb;
	unsigned char ca;
	unsigned char cb;

	do {
		ca = fatsort_next_primary(&pa);
		cb = fatsort_next_primary(&pb);
		if (ca != cb) return ca - cb;
	} while (ca && cb);

	if (ca != cb) return ca - cb;
	while (*sa && *sa == *sb) {
		sa++;
		sb++;
	}
	if (*sa == ' ' && *sb != ' ') return 1;
	if (*sb == ' ' && *sa != ' ') return -1;
	return strcmp(sa, sb);
}

static int fatsort_cmp_item(const FSORTDIR_ITEM *a, const FSORTDIR_ITEM *b)
{
	int a_dir = (a->fno.fattrib & AM_DIR) != 0;
	int b_dir = (b->fno.fattrib & AM_DIR) != 0;

	if (a_dir != b_dir) return b_dir - a_dir;
	return fatsort_cmp_name(&a->fno, &b->fno);
}

static FRESULT fatsort_read_entries(FSORTDIR *sort, DWORD ofs, UINT nent, BYTE *buf)
{
	FRESULT res;
	UINT i;
	DIR *dp = &sort->dir;

	res = dir_sdi(dp, ofs);
	if (res != FR_OK) return res;

	for (i = 0; i < nent; i++) {
		res = move_window(dp->obj.fs, dp->sect);
		if (res != FR_OK) return res;

		memcpy(buf + (DWORD)i * SZDIRE, dp->dir, SZDIRE);
		sort->scanned_entries++;
		if (i + 1 < nent) {
			res = dir_next(dp, 0);
			if (res != FR_OK) return res;
		}
	}
	return FR_OK;
}

static FRESULT fatsort_write_entries(FSORTDIR *sort, DWORD ofs, UINT nent, const BYTE *buf)
{
	FRESULT res;
	UINT i;
	DIR *dp = &sort->dir;

	res = dir_sdi(dp, ofs);
	if (res != FR_OK) return res;

	for (i = 0; i < nent; i++) {
		res = move_window(dp->obj.fs, dp->sect);
		if (res != FR_OK) return res;

		memcpy(dp->dir, buf + (DWORD)i * SZDIRE, SZDIRE);
		dp->obj.fs->wflag = 1;
		sort->sorted_entries++;
		if (i + 1 < nent) {
			res = dir_next(dp, 0);
			if (res != FR_OK) return res;
		}
	}
	return FR_OK;
}

static FRESULT fatsort_bad_lfn_barrier(DIR *dj, DWORD start, FSORTDIR_ITEM *item)
{
	FRESULT res;
	BYTE et, attr;

	for (;;) {
		res = dir_next(dj, 0);
		if (res != FR_OK) break;
		res = move_window(dj->obj.fs, dj->sect);
		if (res != FR_OK) return res;

		et = dj->dir[DIR_Name];
		attr = dj->dir[DIR_Attr] & AM_MASK;

		if (et == 0 || attr != AM_LFN) break;
	}

	item->kind = FATSORT_BARRIER;
	item->ofs = start;
	item->nent = (UINT)((dj->dptr - start) / SZDIRE + 1);
	return FR_OK;
}

static FRESULT fatsort_scan(DIR *dp, DWORD ofs, FSORTDIR_ITEM *item)
{
	FRESULT res;
	BYTE et, attr;
	DWORD start = ofs;

	memset(item, 0, sizeof(*item));
	item->ofs = ofs;

	if (dp->sect != 0 && ofs == dp->dptr + SZDIRE) {
		res = dir_next(dp, 0);
	} else {
		res = dir_sdi(dp, ofs);
	}
	if (res == FR_INT_ERR || res == FR_NO_FILE) {
		item->kind = FATSORT_END;
		return FR_OK;
	}
	if (res != FR_OK) return res;
	res = move_window(dp->obj.fs, dp->sect);
	if (res != FR_OK) return res;

	et = dp->dir[DIR_Name];
	attr = dp->dir[DIR_Attr] & AM_MASK;

	if (et == 0) {
		item->kind = FATSORT_END;
		return FR_OK;
	}

	if (et == DDEM) {
		item->kind = FATSORT_HOLE;
		item->nent = 1;
		return FR_OK;
	}

	if (et == '.' || ((attr & ~AM_ARC) == AM_VOL)) {
		item->kind = FATSORT_BARRIER;
		item->nent = 1;
		return FR_OK;
	}

	if (attr == AM_LFN) {
		if (!(et & LLEF)) return fatsort_bad_lfn_barrier(dp, start, item);

		res = dir_read(dp, 0);
		if (res == FR_NO_FILE) return fatsort_bad_lfn_barrier(dp, start, item);
		if (res != FR_OK) return res;
		if (dp->blk_ofs != start) return fatsort_bad_lfn_barrier(dp, start, item);

		item->ofs = dp->blk_ofs;
		item->nent = (UINT)((dp->dptr - dp->blk_ofs) / SZDIRE + 1);
	} else {
		item->nent = 1;
		dp->blk_ofs = 0xFFFFFFFF;
	}

	item->kind = FATSORT_BLOCK;
	get_fileinfo(dp, &item->fno);
	return FR_OK;
}

static BYTE *fatsort_candidate_slot(FSORTDIR *sort, UINT index)
{
	return sort->work + FATSORT_WORK_SCRATCH_SIZE + (DWORD)index * sizeof(FSORTDIR_ITEM);
}

static void fatsort_get_candidate(FSORTDIR *sort, UINT index, FSORTDIR_ITEM *item)
{
	memcpy(item, fatsort_candidate_slot(sort, index), sizeof(*item));
}

static void fatsort_set_candidate(FSORTDIR *sort, UINT index, const FSORTDIR_ITEM *item)
{
	memcpy(fatsort_candidate_slot(sort, index), item, sizeof(*item));
}

static void fatsort_insert_candidate(FSORTDIR *sort, const FSORTDIR_ITEM *item)
{
	UINT pos = sort->candidate_count;
	FSORTDIR_ITEM tmp;

	while (pos > 0) {
		fatsort_get_candidate(sort, pos - 1, &tmp);
		if (fatsort_cmp_item(&tmp, item) <= 0) break;
		fatsort_set_candidate(sort, pos, &tmp);
		pos--;
	}

	fatsort_set_candidate(sort, pos, item);
	sort->candidate_count++;
}

static void fatsort_consider_candidate(FSORTDIR *sort, const FSORTDIR_ITEM *item)
{
	FSORTDIR_ITEM largest;

	if (sort->candidate_count < sort->candidate_capacity) {
		fatsort_insert_candidate(sort, item);
		return;
	}

	fatsort_get_candidate(sort, sort->candidate_count - 1, &largest);
	if (fatsort_cmp_item(item, &largest) >= 0) return;

	sort->candidate_count--;
	fatsort_insert_candidate(sort, item);
}

static void fatsort_update_candidate_offsets(FSORTDIR *sort, const FSORTDIR_ITEM *selected, DWORD old_out_ofs)
{
	UINT i;
	FSORTDIR_ITEM item;
	DWORD selected_size = (DWORD)selected->nent * SZDIRE;

	for (i = sort->commit_index; i < sort->candidate_count; i++) {
		fatsort_get_candidate(sort, i, &item);
		if (item.ofs == selected->ofs) {
			item.ofs = old_out_ofs;
			fatsort_set_candidate(sort, i, &item);
		} else if (item.ofs >= old_out_ofs && item.ofs < selected->ofs) {
			item.ofs += selected_size;
			fatsort_set_candidate(sort, i, &item);
		}
	}
}

static FRESULT fatsort_move_right(FSORTDIR *sort, DWORD src_ofs, DWORD dst_ofs, DWORD nent, BYTE *work, UINT work_entries)
{
	FRESULT res;

	while (nent > 0) {
		UINT chunk = nent > work_entries ? work_entries : (UINT)nent;
		DWORD first = nent - chunk;

		res = fatsort_read_entries(sort, src_ofs + first * SZDIRE, chunk, work);
		if (res != FR_OK) return res;

		res = fatsort_write_entries(sort, dst_ofs + first * SZDIRE, chunk, work);
		if (res != FR_OK) return res;

		nent = first;
	}

	return FR_OK;
}

static FRESULT fatsort_rotate_span(FSORTDIR *sort, const FSORTDIR_ITEM *selected)
{
	FRESULT res;
	DIR *dp = &sort->dir;
	FSORTDIR_SCRATCH *scratch = (FSORTDIR_SCRATCH *)sort->work;
	DWORD move_nent;
	DWORD old_out_ofs = sort->out_ofs;

	res = fatsort_read_entries(sort, selected->ofs, selected->nent, scratch->selected);
	if (res != FR_OK) return res;

	move_nent = (selected->ofs - old_out_ofs) / SZDIRE;
	res = fatsort_move_right(sort, old_out_ofs, old_out_ofs + (DWORD)selected->nent * SZDIRE, move_nent, sort->move_work, FATSORT_MOVE_MAX_BLOCK);
	if (res != FR_OK) return res;

	res = fatsort_write_entries(sort, old_out_ofs, selected->nent, scratch->selected);
	if (res != FR_OK) return res;

	fatsort_update_candidate_offsets(sort, selected, old_out_ofs);
	return sync_window(dp->obj.fs);
}

FRESULT f_sortdir(FSORTDIR *sort, BYTE *work, BYTE *move_work)
{
	FRESULT res;

	if (sort == 0 || work == 0 || move_work == 0) return FR_INVALID_PARAMETER;

	memset(sort, 0, sizeof(*sort));

	res = f_opendir(&sort->dir, "");
	if (res != FR_OK) return res;

	sort->work = work;
	sort->move_work = move_work;
	sort->candidate_capacity = FATSORT_CANDIDATE_WORK / sizeof(FSORTDIR_ITEM);
	sort->phase = FATSORT_PHASE_OUT;
	sort->run_sorted = 1;
	return FR_OK;
}

__attribute__((noinline)) FRESULT f_sortnext(FSORTDIR *sort)
{
	FRESULT res;
	FSORTDIR_ITEM item;
	DWORD next_ofs;

	if (sort == 0) return FR_INVALID_PARAMETER;
	if (sort->dir.obj.fs == 0) return FR_INVALID_OBJECT;
	if (sort->done) return FR_OK;

	if (sort->phase == FATSORT_PHASE_OUT) {
		sort->scan_ofs = sort->out_ofs;
		sort->candidate_count = 0;
		sort->run_sorted = 1;
		sort->prev.kind = FATSORT_END;
		sort->phase = FATSORT_PHASE_SCAN;
	}

	if (sort->phase == FATSORT_PHASE_SCAN) {
		res = fatsort_scan(&sort->dir, sort->scan_ofs, &item);
		if (res != FR_OK) return res;

		sort->scanned_entries += item.nent ? item.nent : 1;

		if (item.kind == FATSORT_END || item.kind == FATSORT_BARRIER) {
			if (sort->candidate_count == 0) {
				if (item.kind == FATSORT_END) {
					sort->done = 1;
				} else {
					sort->out_ofs = item.ofs + (DWORD)item.nent * SZDIRE;
					sort->phase = FATSORT_PHASE_OUT;
				}
				return FR_OK;
			}

			if (sort->run_sorted) {
				sort->out_ofs = item.ofs;
				sort->phase = FATSORT_PHASE_OUT;
				return FR_OK;
			}

			sort->commit_index = 0;
			sort->phase = FATSORT_PHASE_ROTATE;
			return FR_OK;
		}

		next_ofs = item.ofs + (DWORD)item.nent * SZDIRE;
		sort->scan_ofs = next_ofs;

		if (item.kind == FATSORT_BLOCK) {
			if (sort->prev.kind == FATSORT_BLOCK && fatsort_cmp_item(&sort->prev, &item) > 0) {
				sort->run_sorted = 0;
			}
			sort->prev = item;
			fatsort_consider_candidate(sort, &item);
		}

		return FR_OK;
	}

	while (sort->commit_index < sort->candidate_count) {
		fatsort_get_candidate(sort, sort->commit_index, &sort->current);

		if (sort->current.ofs == sort->out_ofs) {
			sort->out_ofs += (DWORD)sort->current.nent * SZDIRE;
			sort->commit_index++;
			continue;
		}

		res = fatsort_rotate_span(sort, &sort->current);
		if (res != FR_OK) return res;
		sort->out_ofs += (DWORD)sort->current.nent * SZDIRE;
		sort->commit_index++;
		return FR_OK;
	}

	sort->phase = FATSORT_PHASE_OUT;
	return FR_OK;
}

FRESULT f_sortclose(FSORTDIR *sort)
{
	FRESULT res;
	FRESULT close_res;

	if (sort == 0) return FR_INVALID_PARAMETER;
	if (sort->dir.obj.fs == 0) return FR_INVALID_OBJECT;

	res = sync_window(sort->dir.obj.fs);
	close_res = f_closedir(&sort->dir);
	sort->done = 1;

	if (res == FR_OK && close_res != FR_OK) res = close_res;
	return res;
}
