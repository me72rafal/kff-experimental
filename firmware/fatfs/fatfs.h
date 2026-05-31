#include "ff.h"

/*
 * Sort the current FatFs directory.
 *
 * Usage:
 * 1. Allocate FSORTDIR, a FATSORT_WORK_SIZE byte work buffer, and a
 *    FATSORT_MOVE_WORK_SIZE byte move scratch buffer. FSORTDIR does not need
 *    to be cleared by the caller.
 * 2. Call f_sortdir().
 * 3. If f_sortdir() returns FR_OK, call f_sortnext() until sort->done is
 *    non-zero or an error is returned.
 * 4. Between f_sortnext() calls, the caller may update the UI from
 *    sort->scanned_entries and sort->sorted_entries.
 * 5. Always call f_sortclose() after a successful f_sortdir(), including after
 *    completion, cancellation, or an f_sortnext() error.
 *
 * The work buffer and move scratch buffer passed to f_sortdir() must stay
 * allocated at the same addresses until f_sortclose() returns.
 * The first FATSORT_WORK_SCRATCH_SIZE bytes of the work buffer and all
 * FATSORT_MOVE_WORK_SIZE bytes of the move buffer are temporary scratch used
 * only while f_sortnext() is running. The caller may reuse the contents of
 * those scratch areas between f_sortnext() calls, but must not free either
 * buffer or pass storage that can be relocated before f_sortclose() returns.
 * The rest of the work buffer is private sorter state until f_sortclose()
 * returns.
 *
 * To cancel, stop calling f_sortnext() and then call f_sortclose().
 * Cancellation does not undo completed sort steps. The directory may be
 * partially sorted and can be sorted again later.
 * f_sortclose() closes the FatFs DIR object and flushes pending directory
 * writes.
 *
 * Caller-visible state:
 * - sort->done: non-zero when no more sort steps are needed
 * - sort->scanned_entries: raw FAT directory entries scanned or read
 * - sort->sorted_entries: raw FAT directory entries sorted
 */

#define FATSORT_WORK_SIZE (64UL * 1024UL)
#define FATSORT_DIR_ENTRY_SIZE 32
#define FATSORT_MAX_LFN 20
#define FATSORT_MAX_BLOCK (FATSORT_MAX_LFN + 1)
#define FATSORT_MOVE_MAX_BLOCK 1024
#define FATSORT_MOVE_WORK_SIZE (FATSORT_MOVE_MAX_BLOCK * FATSORT_DIR_ENTRY_SIZE)

typedef struct
{
	BYTE selected[FATSORT_MAX_BLOCK * FATSORT_DIR_ENTRY_SIZE];
} FSORTDIR_SCRATCH;

#define FATSORT_WORK_SCRATCH_SIZE ((UINT)sizeof(FSORTDIR_SCRATCH))

typedef struct
{
	BYTE kind;
	DWORD ofs;
	UINT nent;
	FILINFO fno;
} FSORTDIR_ITEM;

typedef struct
{
	DIR dir;
	BYTE *work;
	BYTE *move_work;
	DWORD scanned_entries;
	DWORD sorted_entries;
	DWORD out_ofs;
	DWORD scan_ofs;
	UINT candidate_count;
	UINT candidate_capacity;
	UINT commit_index;
	FSORTDIR_ITEM prev;
	FSORTDIR_ITEM current;
	BYTE phase;
	BYTE done;
	BYTE run_sorted;
} FSORTDIR;

FRESULT f_sortdir(FSORTDIR *sort, BYTE *work, BYTE *move_work);
FRESULT f_sortnext(FSORTDIR *sort);
FRESULT f_sortclose(FSORTDIR *sort);
