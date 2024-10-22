// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ktime.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "conv_ftl.h"

#define prp_address_offset(prp, offset) \
	(page_address(pfn_to_page(prp >> PAGE_SHIFT) + offset) + (prp & ~PAGE_MASK))
#define prp_address(prp) prp_address_offset(prp, 0)
static inline bool last_pg_in_wordline(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	return (ppa->g.pg % spp->pgs_per_oneshotpg) == (spp->pgs_per_oneshotpg - 1);
}

static bool should_gc(struct conv_ftl *conv_ftl)
{
	return (conv_ftl->lm.free_line_cnt <= conv_ftl->cp.gc_thres_lines);
}

static inline bool should_gc_high(struct conv_ftl *conv_ftl)
{
	return conv_ftl->lm.free_line_cnt <= conv_ftl->cp.gc_thres_lines_high;
}

static inline struct ppa get_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	NVMEV_ASSERT(conv_ftl != NULL);
	return conv_ftl->maptbl[lpn];
}

static inline void set_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
	NVMEV_ASSERT(lpn < conv_ftl->ssd->sp.tt_pgs);
	conv_ftl->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint64_t pgidx;

	NVMEV_DEBUG_VERBOSE("%s: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", __func__,
			ppa->g.ch, ppa->g.lun, ppa->g.pl, ppa->g.blk, ppa->g.pg);

	pgidx = ppa->g.ch * spp->pgs_per_ch + ppa->g.lun * spp->pgs_per_lun +
		ppa->g.pl * spp->pgs_per_pl + ppa->g.blk * spp->pgs_per_blk + ppa->g.pg;

	NVMEV_ASSERT(pgidx < spp->tt_pgs);

	return pgidx;
}

static inline uint64_t get_rmap_ent(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(conv_ftl, ppa);

	return conv_ftl->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(conv_ftl, ppa);

	conv_ftl->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
	return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
	return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
	((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
	return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
	((struct line *)a)->pos = pos;
}

#ifdef FDP_SIMULATOR
/**
 * victim_ru_cmp_pri - Compare the priority of two reclaim units.
 * @next: The priority of the next reclaim unit.
 * @curr: The priority of the current reclaim unit.
 *
 * This function compares the priorities of two reclaim units, returning 
 * a positive value if the next reclaim unit has a higher priority than 
 * the current one.
 *
 * Return: 1 if next's priority is higher than curr's, 0 otherwise.
 */
static inline int victim_ru_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
	return (next > curr);
}


/**
 * victim_ru_get_pri - Get the priority of a reclaim unit.
 * @a: Pointer to the reclaim unit.
 *
 * This function retrieves the priority of a given reclaim unit.
 * The priority is stored as a member of the reclaim_unit structure.
 * We use valid page count (vpc) as a priority of reclaim unit.
 *
 * Return: The priority of the reclaim unit.
 */
static inline pqueue_pri_t victim_ru_get_pri(void *a)
{
	return ((struct reclaim_unit *)a)->vpc;
}

/**
 * victim_ru_set_pri - Set the valid page count (vpc) of a reclaim unit.
 * @a: Pointer to the reclaim unit.
 * @pri: The valid page count to be assigned.
 *
 * This function sets the valid page count (vpc) of a given reclaim unit to the 
 * provided value. The valid page count is stored as a member of the reclaim_unit structure.
 */
static inline void victim_ru_set_pri(void *a, pqueue_pri_t pri)
{
	((struct reclaim_unit *)a)->vpc = pri;
}
/**
 * victim_ru_get_pos - Get the position of a reclaim unit in the queue.
 * @a: Pointer to the reclaim unit.
 *
 * This function retrieves the position (pos) of the reclaim unit in 
 * the priority queue. The position is stored as a member of the reclaim_unit structure.
 *
 * Return: The position of the reclaim unit in the queue.
 */
static inline size_t victim_ru_get_pos(void *a)
{
	return ((struct reclaim_unit *)a)->pos;
}

/**
 * victim_ru_set_pos - Set the position of a reclaim unit in the queue.
 * @a: Pointer to the reclaim unit.
 * @pos: The position to be assigned.
 *
 * This function sets the position (pos) of the reclaim unit in the 
 * priority queue to the provided value. The position is stored as a 
 * member of the reclaim_unit structure.
 */
static inline void victim_ru_set_pos(void *a, size_t pos)
{
	((struct reclaim_unit *)a)->pos = pos;
}
#endif //FDP_SIMULATOR

static inline void consume_write_credit(struct conv_ftl *conv_ftl)
{
	conv_ftl->wfc.write_credits--;
}

static void foreground_gc(struct conv_ftl *conv_ftl);

static inline void check_and_refill_write_credit(struct conv_ftl *conv_ftl)
{
	struct write_flow_control *wfc = &(conv_ftl->wfc);
	if (wfc->write_credits <= 0) {
		foreground_gc(conv_ftl);

		wfc->write_credits += wfc->credits_to_refill;
	}
}

#ifdef FDP_SIMULATOR
static void fdp_foreground_gc(struct fdp_ftl *fdp_ftl);

/**
 * fdp_check_and_refill_write_credit - Check and refill write credits for flow control.
 * @fdp_ftl: Pointer to the Flash Translation Layer (FTL) structure.
 *
 * This function checks if the current write credits in the write flow control (wfc) are
 * depleted. If the write credits are zero or negative, it triggers foreground garbage collection
 * (GC) by calling `fdp_foreground_gc()`. After garbage collection, the function refills the
 * write credits by adding the number of credits specified by `credits_to_refill` to the current
 * write credits.
 *
 * This mechanism helps manage write operations by ensuring that sufficient write credits
 * are available, triggering garbage collection to reclaim space when necessary.
 */
static inline void fdp_check_and_refill_write_credit(struct fdp_ftl *fdp_ftl)
{
	struct write_flow_control *wfc = &(fdp_ftl->wfc);
	if (wfc->write_credits <= 0) {
		fdp_foreground_gc(fdp_ftl);

		wfc->write_credits += wfc->credits_to_refill;
	}
}

static void init_fdp_lines(struct fdp_ftl *fdp_ftl)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct line_mgmt *lm = &fdp_ftl->lm;
	struct line *line;
	int i;

	lm->tt_lines = spp->blks_per_pl;
	NVMEV_ASSERT(lm->tt_lines == spp->tt_lines);
	lm->lines = vmalloc(sizeof(struct line) * lm->tt_lines);

	INIT_LIST_HEAD(&lm->free_line_list);
	INIT_LIST_HEAD(&lm->full_line_list);

	lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri, victim_line_get_pri,
					 victim_line_set_pri, victim_line_get_pos,
					 victim_line_set_pos);

	lm->free_line_cnt = 0;
	for (i = 0; i < lm->tt_lines; i++) {
		lm->lines[i] = (struct line){
			.id = i,
			.ipc = 0,
			.vpc = 0,
			.pos = 0,
			.entry = LIST_HEAD_INIT(lm->lines[i].entry),
		};

		/* initialize all the lines as free lines */
		list_add_tail(&lm->lines[i].entry, &lm->free_line_list);
		lm->free_line_cnt++;
	}

	NVMEV_ASSERT(lm->free_line_cnt == lm->tt_lines);
	lm->victim_line_cnt = 0;
	lm->full_line_cnt = 0;
}

static void remove_fdp_lines(struct fdp_ftl *fdp_ftl)
{
	pqueue_free(fdp_ftl->lm.victim_line_pq);
	vfree(fdp_ftl->lm.lines);
}

static struct line *get_next_free_line(struct conv_ftl *conv_ftl);

/**
 * prepare_ru_write_pointer - Initialize the write pointer for a reclaim unit.
 * @fdp_ftl: Pointer to the Flash Translation Layer (FTL) structure.
 * @ru: Pointer to the reclaim unit.
 *
 * This function prepares the write pointer for the specified reclaim unit (ru).
 * It fetches the next available line using the `get_next_free_line()` function
 * and assigns that line to the reclaim unit's write pointer (wp).
 * 
 * The write pointer is initialized with default values for channel (ch), 
 * logical unit number (lun), page (pg), plane (pl), and block (blk).
 */
static void prepare_ru_write_pointer(struct fdp_ftl *fdp_ftl, struct reclaim_unit *ru) {
	struct line *curline = get_next_free_line((struct conv_ftl *) fdp_ftl);
	struct write_pointer *wp = &ru->wp;

	NVMEV_ASSERT(wp);
	NVMEV_ASSERT(curline);

	*wp = (struct write_pointer){
		.curline = curline,
		.ch = 0,
		.lun = 0,
		.pg = 0,
		.blk = curline->id,
		.pl = 0,
	};

}

/**
 * init_reclaim_group - Initialize a group of reclaim units within the FTL.
 * @fdp_ftl: Pointer to the Flash Translation Layer (FTL) structure.
 *
 * This function initializes the reclaim groups within the FTL.
 * For each reclaim group, it allocates memory for reclaim units, sets up the
 * free and full reclaim unit lists, initializes priority queue for victim reclaim units,
 * and prepares each reclaim unit with default values, including setting up write pointers.
 *
 * Reclaim units are grouped in structures that manage their states (free, full, victim).
 */
static void init_reclaim_group(struct fdp_ftl *fdp_ftl)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	int i;

	for (i = 0; i < RG_PER_FTL; i++) {
		struct reclaim_group_mgmt *rgm = &fdp_ftl->rgm[i];
		struct line *line;
		int j;

		rgm->tt_ru = RU_PER_RG; 
		rgm->ru_entries = vmalloc(sizeof(struct reclaim_unit) * rgm->tt_ru);

		INIT_LIST_HEAD(&rgm->free_ru_list);
		INIT_LIST_HEAD(&rgm->full_ru_list);

		// Need for fix
		rgm->victim_ru_pq = pqueue_init(rgm->tt_ru, victim_ru_cmp_pri, victim_ru_get_pri,
						 victim_ru_set_pri, victim_ru_get_pos,
						 victim_ru_set_pos);

		// Need for fix
		rgm->free_ru_cnt = 0;
		for (j = 0; j < rgm->tt_ru; j++) {
			struct reclaim_unit *ru = &rgm->ru_entries[j];
			ru->rp.start_ch = 0;
			ru->rp.nchs_per_ru = spp->ru_nchs;
			ru->rp.luns_per_ru = spp->luns_per_ch;
			ru->rp.blks_per_ru = spp->tt_blks / RECLAIM_UNITS;

			ru->id = i * rgm->tt_ru + j;
			ru->ipc = 0;
			ru->vpc = 0;
			ru->pos = 0;
			ru->ref_id = -1;
			ru->blks = 0;
			ru->ruamw = ru->rp.blks_per_ru * 512 * 1024;


			prepare_ru_write_pointer(fdp_ftl, ru);
			LIST_HEAD_INIT(ru->entry);

			/* initialize all the lines as free lines */
			list_add_tail(&rgm->ru_entries[j].entry, &rgm->free_ru_list);
			rgm->free_ru_cnt++;
		}

		NVMEV_ASSERT(rgm->free_ru_cnt == rgm->tt_ru);
		rgm->victim_ru_cnt = 0;
		rgm->full_ru_cnt = 0;
	}
}

/**
 * remove_reclaim_group - Clean up and remove reclaim groups from the FTL.
 * @fdp_ftl: Pointer to the Flash Translation Layer (FTL) structure.
 *
 * This function deallocates memory used by the reclaim groups in the FTL.
 * It frees the priority queue for victim reclaim units and the memory allocated
 * for reclaim unit entries.
 */
static void remove_reclaim_group(struct fdp_ftl *fdp_ftl)
{
	int i;
	for (i = 0; i < RG_PER_FTL; i++) {
		struct reclaim_group_mgmt *rgm = &fdp_ftl->rgm[i];
		pqueue_free(rgm->victim_ru_pq);
		vfree(rgm->ru_entries);
	}
}
#endif //FDP_SIMULATOR

static void init_lines(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *line;
	int i;

	lm->tt_lines = spp->blks_per_pl;
	NVMEV_ASSERT(lm->tt_lines == spp->tt_lines);
	lm->lines = vmalloc(sizeof(struct line) * lm->tt_lines);

	INIT_LIST_HEAD(&lm->free_line_list);
	INIT_LIST_HEAD(&lm->full_line_list);

	lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri, victim_line_get_pri,
					 victim_line_set_pri, victim_line_get_pos,
					 victim_line_set_pos);

	lm->free_line_cnt = 0;
	for (i = 0; i < lm->tt_lines; i++) {
		lm->lines[i] = (struct line){
			.id = i,
			.ipc = 0,
			.vpc = 0,
			.pos = 0,
			.entry = LIST_HEAD_INIT(lm->lines[i].entry),
		};

		/* initialize all the lines as free lines */
		list_add_tail(&lm->lines[i].entry, &lm->free_line_list);
		lm->free_line_cnt++;
	}

	NVMEV_ASSERT(lm->free_line_cnt == lm->tt_lines);
	lm->victim_line_cnt = 0;
	lm->full_line_cnt = 0;
}

#ifdef FDP_SIMULATOR
static void init_fdp_write_flow_control(struct fdp_ftl *fdp_ftl)
{
	struct write_flow_control *wfc = &(fdp_ftl->wfc);
	struct ssdparams *spp = &fdp_ftl->ssd->sp;

	wfc->write_credits = spp->pgs_per_line;
	wfc->credits_to_refill = spp->pgs_per_line;
}


static struct reclaim_unit *get_next_free_ru(struct fdp_ftl *fdp_ftl, uint16_t rg_id)
{
	struct reclaim_group_mgmt *rgm = &fdp_ftl->rgm[rg_id];
	struct reclaim_unit *cur_ru = list_first_entry_or_null(&rgm->free_ru_list, struct reclaim_unit, entry);

	if (!cur_ru) {
		NVMEV_ERROR("No free RU left in VIRT !!!!\n");
		return NULL;
	}

	list_del_init(&cur_ru->entry);
	rgm->free_ru_cnt--;
	//NVMEV_INFO("%s: free_line_cnt %d\n", __func__, lm->free_line_cnt);
	return cur_ru;
}
#endif //FDP_SIMULATOR

static void init_write_flow_control(struct conv_ftl *conv_ftl)
{
	struct write_flow_control *wfc = &(conv_ftl->wfc);
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	wfc->write_credits = spp->pgs_per_line;
	wfc->credits_to_refill = spp->pgs_per_line;
}

static inline void check_addr(int a, int max)
{
	NVMEV_ASSERT(a >= 0 && a < max);
}

static void remove_lines(struct conv_ftl *conv_ftl)
{
	pqueue_free(conv_ftl->lm.victim_line_pq);
	vfree(conv_ftl->lm.lines);
}

static struct line *get_next_free_line(struct conv_ftl *conv_ftl)
{
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);

	if (!curline) {
		NVMEV_ERROR("No free line left in VIRT !!!!\n");
		return NULL;
	}

	list_del_init(&curline->entry);
	lm->free_line_cnt--;
	//NVMEV_INFO("%s: free_line_cnt %d\n", __func__, lm->free_line_cnt);
	return curline;
}

#ifdef FDP_SIMULATOR
static struct reclaim_unit *get_next_free_ru(struct fdp_ftl *fdp_ftl, uint32_t phnd_id, int32_t io_type)
{
	struct reclaim_group_mgmt *rgm = __get_ftl_rgm(fdp_ftl, phnd_id, io_type);

	struct reclaim_unit *cur_ru = list_first_entry_or_null(&rgm->free_ru_list, struct reclaim_unit, entry);

	if (!cur_ru) {
		NVMEV_ERROR("No free RU left in VIRT !!!!\n");
		return NULL;
	}

	list_del_init(&cur_ru->entry);
	rgm->free_ru_cnt--;
	//NVMEV_INFO("%s: free_line_cnt %d\n", __func__, lm->free_line_cnt);
	return curline;
}
#endif //FDP_SIMULATOR


static struct write_pointer *__get_wp(struct conv_ftl *ftl, uint32_t io_type)
{
	if (io_type == USER_IO) {
		return &ftl->wp;
	} else if (io_type == GC_IO) {
		return &ftl->gc_wp;
	}

	NVMEV_ASSERT(0);
	return NULL;
}



static void prepare_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	struct write_pointer *wp = __get_wp(conv_ftl, io_type);
	struct line *curline = get_next_free_line(conv_ftl);

	NVMEV_ASSERT(wp);
	NVMEV_ASSERT(curline);

	/* wp->curline is always our next-to-write super-block */
	*wp = (struct write_pointer){
		.curline = curline,
		.ch = 0,
		.lun = 0,
		.pg = 0,
		.blk = curline->id,
		.pl = 0,
	};
}


static void advance_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct write_pointer *wpp = __get_wp(conv_ftl, io_type);

	NVMEV_DEBUG_VERBOSE("current wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n",
			wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg);

	check_addr(wpp->pg, spp->pgs_per_blk);
	wpp->pg++;
	if ((wpp->pg % spp->pgs_per_oneshotpg) != 0) {
		//NVMEV_INFO("[NoFreeLine] %s() goto out 1\n", __func__);
		goto out;
	}

	wpp->pg -= spp->pgs_per_oneshotpg;
	check_addr(wpp->ch, spp->nchs);
	wpp->ch++;
	if (wpp->ch != spp->nchs) {
		//NVMEV_INFO("[NoFreeLine] %s() goto out 2\n", __func__);
		goto out;
	}

	wpp->ch = 0;
	check_addr(wpp->lun, spp->luns_per_ch);
	wpp->lun++;
	/* in this case, we should go to next lun */
	if (wpp->lun != spp->luns_per_ch) {
		//NVMEV_INFO("[NoFreeLine] %s() goto out 3\n", __func__);
		goto out;
	}

	wpp->lun = 0;
	/* go to next wordline in the block */
	wpp->pg += spp->pgs_per_oneshotpg;
	if (wpp->pg != spp->pgs_per_blk) {
		//NVMEV_INFO("[NoFreeLine] %s() goto out 4\n", __func__);
		goto out;
	}

	wpp->pg = 0;
	/* move current line to {victim,full} line list */
	if (wpp->curline->vpc == spp->pgs_per_line) {
		/* all pgs are still valid, move to full line list */
		NVMEV_ASSERT(wpp->curline->ipc == 0);
		list_add_tail(&wpp->curline->entry, &lm->full_line_list);
		lm->full_line_cnt++;
		NVMEV_DEBUG_VERBOSE("wpp: move line to full_line_list\n");
		//NVMEV_INFO("[NoFreeLine] %s() full_line_cnt++ %d\n", __func__, lm->full_line_cnt);
	} else {
		NVMEV_ASSERT(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
		/* there must be some invalid pages in this line */
		NVMEV_ASSERT(wpp->curline->ipc > 0);
		pqueue_insert(lm->victim_line_pq, wpp->curline);
		lm->victim_line_cnt++;
		//NVMEV_INFO("[NoFreeLine] %s() victim_line_cnt++ %d\n", __func__,lm->victim_line_cnt);
	}
	/* current line is used up, pick another empty line */
	check_addr(wpp->blk, spp->blks_per_pl);
	wpp->curline = get_next_free_line(conv_ftl);
#ifdef BUG_FIX
	if(wpp->curline == NULL)  {
		NVMEV_INFO("wpp: There is no free line left in VIRT\n");
		return;
	}
#endif //BUG_FIX

	NVMEV_DEBUG_VERBOSE("wpp: got new clean line %d\n", wpp->curline->id);

	wpp->blk = wpp->curline->id;
	check_addr(wpp->blk, spp->blks_per_pl);

	/* make sure we are starting from page 0 in the super block */
	NVMEV_ASSERT(wpp->pg == 0);
	NVMEV_ASSERT(wpp->lun == 0);
	NVMEV_ASSERT(wpp->ch == 0);
	/* TODO: assume # of pl_per_lun is 1, fix later */
	NVMEV_ASSERT(wpp->pl == 0);
out:
	NVMEV_DEBUG_VERBOSE("advanced wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d (curline %d)\n",
			wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg, wpp->curline->id);
}

#ifdef FDP_SIMULATOR

static struct write_pointer *__get_ru_wp(struct reclaim_unit *ru)
{
	NVMEV_ASSERT(ru != NULL);
	return &ru->wp;
}

static struct reclaim_unit **__get_ruh_rup(struct reclaim_unit_handle *ruh, uint32_t io_type)
{
	NVMEV_ASSERT(ruh != NULL);
	if (io_type == USER_IO) {
		return &ruh->ru[ruh->ru_idx];
	} else if (io_type == GC_IO) {
		return &ruh->gc_ru[ruh->ru_idx];
	}
	NVMEV_ASSERT(0);
	return NULL;
}

static int *__get_ruh_ru_idx(struct reclaim_unit_handle *ruh, uint32_t io_type)
{
	NVMEV_ASSERT(ruh != NULL);
	if (io_type == USER_IO) {
		return &ruh->ru_idx;
	} else if (io_type == GC_IO) {
		return &ruh->gc_ru_idx;
	}
	NVMEV_ASSERT(0);
	return NULL;
}


static struct reclaim_unit *__get_ruh_ru(struct reclaim_unit_handle *ruh, uint32_t io_type)
{
	NVMEV_ASSERT(ruh != NULL);
	if (io_type == USER_IO) {
		return ruh->ru[ruh->ru_idx];
	} else if (io_type == GC_IO) {
		return ruh->gc_ru[ruh->gc_ru_idx];
	}
	NVMEV_ASSERT(0);
	return NULL;
}

static struct reclaim_unit **__get_ruh_rupp(struct reclaim_unit_handle *ruh, uint32_t io_type)
{
	NVMEV_ASSERT(ruh != NULL);
	if (io_type == USER_IO) {
		return &ruh->ru[ruh->ru_idx];
	} else if (io_type == GC_IO) {
		return &ruh->gc_ru[ruh->gc_ru_idx];
	}
	NVMEV_ASSERT(0);
	return NULL;
}

static struct reclaim_group_mgmt *__get_ftl_rgm(struct fdp_ftl *fdp_ftl, uint16_t phnd_id, uint32_t io_type)
{
	struct reclaim_unit_handle *ruh = __get_ftl_ruh(fdp_ftl, phnd_id);
	int * ru_idx  = __get_ruh_ru_idx(ruh, io_type);

	return fdp_ftl->rgm[*ru_idx];

}

static struct reclaim_unit_handle *__get_ftl_ruh(struct fdp_ftl *fdp_ftl, uint16_t phnd_id)
{
	struct reclaim_unit_handle *ruh = __get_ftl_ruh(fdp_ftl, phnd_id);
	return ruh;

}

static struct reclaim_unit *__get_ftl_ru(struct fdp_ftl *fdp_ftl, uint16_t phnd_id , uint32_t io_type)
{
	struct reclaim_unit_handle *ruh = __get_ftl_ruh(fdp_ftl, phnd_id);
	return __get_ruh_ru(ruh, io_type);
}

static struct write_pointer *__get_ftl_wp(struct fdp_ftl *fdp_ftl, uint16_t phnd_id , uint32_t io_type)
{
	struct reclaim_unit *ru = __get_ftl_ru(fdp_ftl, phnd_id, io_type);
	return __get_ru_wp(ru);
}



static void rotate_ruh_next_ru_pointer(struct reclaim_unit_handle *ruh, uint32_t io_type) 
{
	NVMEV_ASSERT(ruh != NULL);
	int *ru_idx = __get_ruh_ru_idx(ruh, io_type);
	struct reclaim_unit *rup = __get_ruh_ru(ruh, io_type);

	rup->ruamw -= 512;
	*ru_idx = (*ru_idx + 1) % RG_PER_FTL;
}


/*
static void prepare_fdp_write_pointer(struct fdp_ftl *fdp_ftl, uint32_t io_type)
{
	int rg_idx;
	for (rg_idx = 0; rg_idx < RG_PER_FTL; rg_idx++) {
		int ru_idx;
		for (ru_idx = 0; ru_idx < RU_PER_RG; ru_idx++) {
			struct write_pointer *wp = __get_ru_wp(&fdp_ftl->rgm[rg_idx].ru[ru_idx], io_type);
			// TODO
			// Have to understand about free_line
			struct line *curline = get_fdp_next_free_line(fdp_ftl);

			NVMEV_ASSERT(wp);
			NVMEV_ASSERT(curline);

			*wp = (struct write_pointer){
				.curline = curline,
				.ch = 0,
				.lun = 0,
				.pg = 0,
				.blk = curline->id,
				.pl = 0,
			};
		}
	}

}
*/

static void advance_fdp_ru_pointer(struct fdp_ftl *fdp_ftl, uint32_t phnd_id, uint32_t io_type) {

	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct line_mgmt *lm = &fdp_ftl->lm;

	struct reclaim_unit_handle *ruh = __get_ftl_ruh(fdp_ftl, phnd_id);
	struct reclaim_unit *rup = __get_ruh_ru(ruh, io_type);
	struct write_pointer *wpp = &rup->wp;

	rup->ulc++;
	if (rup->ulc != spp->lines_per_ru) {
		/* current line is used up, pick another empty line */
		check_addr(wpp->blk, spp->blks_per_pl);

		wpp->curline = get_next_free_line((struct conv_ftl *)fdp_ftl);
		wpp->blk = wpp->curline->id;
		check_addr(wpp->blk, spp->blks_per_pl);
		goto out;
	}

	/* initialize write pointer */
	wpp->curline = NULL;
	wpp->blk = 0;

	int *ru_idx = __get_ruh_ru_idx(ruh, io_type);
	struct reclaim_unit **rupp = __get_ruh_rupp(ruh, io_type);
	*rupp = get_next_free_ru(fdp_ftl, *ru_idx);
out:

	NVMEV_DEBUG_VERBOSE("advanced rupp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d (curline %d)\n",
			wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg, wpp->curline->id);

}

static void advance_fdp_write_pointer(struct fdp_ftl *fdp_ftl, uint32_t phnd_id, uint32_t io_type)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct line_mgmt *lm = &fdp_ftl->lm;

	struct reclaim_unit_handle *ruh = fdp_ftl->phndls->phnd[phnd_id].ruh;

	struct reclaim_unit *rup = __get_ftl_ru(fdp_ftl, phnd_id, io_type);
	struct write_pointer *wpp = &rup->wp;

	/*
	NVMEV_INFO("current wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n",
			wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg);
			*/

	check_addr(wpp->pg, spp->pgs_per_blk);
	wpp->pg++;
	if ((wpp->pg % spp->pgs_per_oneshotpg) != 0) {
		//NVMEV_INFO("[NoFreeLine] %s() goto out 1\n", __func__);
		goto out;
	}

	wpp->pg -= spp->pgs_per_oneshotpg;
	check_addr(wpp->ch, spp->ru_nchs);
	wpp->ch++;
	if (wpp->ch != spp->ru_nchs) {
		//NVMEV_INFO("[NoFreeLine] %s() goto out 2\n", __func__);
		goto out;
	}

	wpp->ch = 0;
	check_addr(wpp->lun, spp->luns_per_ch);
	wpp->lun++;
	/* in this case, we should go to next lun */
	if (wpp->lun != spp->luns_per_ch) {
		//NVMEV_INFO("[NoFreeLine] %s() goto out 3\n", __func__);
		goto out;
	}

	wpp->lun = 0;
	/* go to next wordline in the block */
	wpp->pg += spp->pgs_per_oneshotpg;
	if (wpp->pg != spp->pgs_per_blk) {
		//NVMEV_INFO("[NoFreeLine] %s() goto out 4\n", __func__);
		goto out;
	}

	wpp->pg = 0;
	/* move current line to {victim,full} line list */
	if (wpp->curline->vpc == spp->pgs_per_line) {
		/* all pgs are still valid, move to full line list */
		NVMEV_ASSERT(wpp->curline->ipc == 0);
		list_add_tail(&wpp->curline->entry, &lm->full_line_list);
		lm->full_line_cnt++;
		NVMEV_DEBUG_VERBOSE("wpp: move line to full_line_list\n");
		//NVMEV_INFO("[NoFreeLine] %s() full_line_cnt++ %d\n", __func__, lm->full_line_cnt);
	} else {
		NVMEV_ASSERT(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
		/* there must be some invalid pages in this line */
		NVMEV_ASSERT(wpp->curline->ipc > 0);
		pqueue_insert(lm->victim_line_pq, wpp->curline);
		lm->victim_line_cnt++;
		//NVMEV_INFO("[NoFreeLine] %s() victim_line_cnt++ %d\n", __func__,lm->victim_line_cnt);
	}


	/* make sure we are starting from page 0 in the super block */
	NVMEV_ASSERT(wpp->pg == 0);
	NVMEV_ASSERT(wpp->lun == 0);
	NVMEV_ASSERT(wpp->ch == 0);
	/* TODO: assume # of pl_per_lun is 1, fix later */
	NVMEV_ASSERT(wpp->pl == 0);

	advance_fdp_ru_pointer(fdp_ftl, phnd_id, io_type);

out:
	rotate_ruh_next_ru_pointer(ruh, io_type);

	NVMEV_DEBUG_VERBOSE("advanced wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d (curline %d)\n",
			wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg, wpp->curline->id);
}

static struct ppa get_fdp_new_page(struct fdp_ftl *fdp_ftl, uint32_t phnd_id, uint32_t io_type)
{
	struct ppa ppa;
	struct write_pointer *wp = __get_ruh_wp(fdp_ftl->phndls->phnd[phnd_id].ruh, io_type);

	ppa.ppa = 0;
	ppa.g.ch = wp->ch;
	ppa.g.lun = wp->lun;
	ppa.g.pg = wp->pg;
	ppa.g.blk = wp->blk;
	ppa.g.pl = wp->pl;

	NVMEV_ASSERT(ppa.g.pl == 0);

	return ppa;
}

#endif //FDP_SIMULATOR


static struct ppa get_new_page(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	struct ppa ppa;
	struct write_pointer *wp = __get_wp(conv_ftl, io_type);

	ppa.ppa = 0;
	ppa.g.ch = wp->ch;
	ppa.g.lun = wp->lun;
	ppa.g.pg = wp->pg;
	ppa.g.blk = wp->blk;
	ppa.g.pl = wp->pl;

	NVMEV_ASSERT(ppa.g.pl == 0);

	return ppa;
}

static void init_maptbl(struct conv_ftl *conv_ftl)
{
	int i;
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	conv_ftl->maptbl = vmalloc(sizeof(struct ppa) * spp->tt_pgs);
	for (i = 0; i < spp->tt_pgs; i++) {
		conv_ftl->maptbl[i].ppa = UNMAPPED_PPA;
	}
}

static void remove_maptbl(struct conv_ftl *conv_ftl)
{
	vfree(conv_ftl->maptbl);
}

static void init_rmap(struct conv_ftl *conv_ftl)
{
	int i;
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	conv_ftl->rmap = vmalloc(sizeof(uint64_t) * spp->tt_pgs);
	for (i = 0; i < spp->tt_pgs; i++) {
		conv_ftl->rmap[i] = INVALID_LPN;
	}
}

static void remove_rmap(struct conv_ftl *conv_ftl)
{
	vfree(conv_ftl->rmap);
}

#ifdef FDP_SIMULATOR
static void init_fdp_maptbl(struct fdp_ftl *fdp_ftl)
{
	int i;
	struct ssdparams *spp = &fdp_ftl->ssd->sp;

	fdp_ftl->maptbl = vmalloc(sizeof(struct ppa) * spp->tt_pgs);
	for (i = 0; i < spp->tt_pgs; i++) {
		fdp_ftl->maptbl[i].ppa = UNMAPPED_PPA;
	}
}
static void remove_fdp_maptbl(struct fdp_ftl *fdp_ftl)
{
	vfree(fdp_ftl->maptbl);
}

static void init_fdp_rmap(struct fdp_ftl *fdp_ftl)
{
	int i;
	struct ssdparams *spp = &fdp_ftl->ssd->sp;

	fdp_ftl->rmap = vmalloc(sizeof(uint64_t) * spp->tt_pgs);
	for (i = 0; i < spp->tt_pgs; i++) {
		fdp_ftl->rmap[i] = INVALID_LPN;
	}
}

static void remove_fdp_rmap(struct fdp_ftl *fdp_ftl)
{
	vfree(fdp_ftl->rmap);
}

/*
static void init_fdp_ru(struct fdp_ftl *fdp_ftl)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;

	int rg_idx;
	for (rg_idx = 0; rg_idx < RG_PER_FTL; rg_idx++) {
		fdp_ftl->rgm[rg_idx].id = SSD_PARTITIONS * fdp_ftl->id + rg_idx;
		fdp_ftl->rgm[rg_idx].used_ru = 0;

		int ru_idx;
		for (ru_idx = 0; ru_idx < RU_PER_RG; ru_idx++) {
			// Initialize all Reclaim Unit
			struct fdp_reclaim_unit *ru = &fdp_ftl->rg[rg_idx].ru[ru_idx];

			// RU params initialize
			ru->rp.start_ch = 0;
			ru->rp.nchs_per_ru = spp->ru_nchs;
			ru->rp.luns_per_ru = spp->luns_per_ch;
			ru->rp.blks_per_ru = spp->tt_blks / RECLAIM_UNITS;

			ru->id = RU_PER_RG * rg_idx + ru_idx;
			ru->ruamw = ru->rp.blks_per_ru * 512 * 1024;
			ru->blks = 0;
			ru->ref_cnt = 0;
			ru->ruh = NULL;
			ru->rg = &fdp_ftl->rg[rg_idx];
		}
	}
}


static void init_fdp_placement(struct fdp_ftl *fdp_ftl)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	int phndls_size_bytes = sizeof(struct fdp_placement_handle_list) +
		spp->nphndls * sizeof(struct fdp_placement_handle);
	struct fdp_placement_handle_list *phndls = kmalloc(phndls_size_bytes, GFP_KERNEL);


	init_fdp_ru(fdp_ftl);
	phndls->nphndls = spp->nphndls;

	int p_idx;
	for (p_idx = 0; p_idx < phndls->nphndls; p_idx++) {
		struct fdp_reclaim_unit_handle *ruh = kmalloc(sizeof(struct fdp_reclaim_unit_handle), GFP_KERNEL);

		int rg_idx;
		for (rg_idx = 0; rg_idx < RG_PER_FTL; rg_idx++) {
			// Initialize Reclaim Unit Handle
			ruh->ru[rg_idx] = &fdp_ftl->rg[rg_idx].ru[p_idx];
			ruh->ru[rg_idx]->ruh = ruh;
			ruh->ru[rg_idx]->ref_cnt++;
		}

		ruh->id = p_idx;
		phndls->phnd[p_idx].ruh = ruh;
		phndls->phnd[p_idx].id = p_idx;
	}

	fdp_ftl->phndls = phndls;

}
*/

static void remove_fdp_placement(struct fdp_ftl *fdp_ftl)
{
	struct placement_handle_list *phndls = fdp_ftl->phndls;

	int p_idx;
	for (p_idx = 0; p_idx < phndls->nphndls; p_idx++) {
		struct fdp_reclaim_unit_handle *ruh = phndls->phnd[p_idx].ruh;
		int rg_idx;
		for (rg_idx = 0; rg_idx < RG_PER_FTL; rg_idx++) {
			// Initialize Reclaim Unit Handle
			ruh->ru[rg_idx]->ref_cnt--;
			ruh->ru[rg_idx]->ruh = NULL;
			ruh->ru[rg_idx] = NULL;
		}
		kfree (ruh);
	}
	kfree(phndls);
}

static void fdp_init_ftl(struct fdp_ftl *fdp_ftl, int ftl_id, struct convparams *cpp, struct ssd *ssd)
{

	fdp_ftl->id = ftl_id;

	/*copy convparams*/
	fdp_ftl->cp = *cpp;

	fdp_ftl->ssd = ssd;

	/* initialize maptbl */
	init_fdp_maptbl(fdp_ftl); // mapping table

	/* initialize rmap */
	init_fdp_rmap(fdp_ftl); // reverse mapping table (?)

	/* initialize all the lines */
	init_fdp_lines(fdp_ftl);

	/* initialize all the rg and ru */
	init_reclaim_group(fdp_ftl);

	/* initialize placement in FTL */
	// init_fdp_placement(fdp_ftl);

	/* initialize write pointer, this is how we allocate new pages for writes */
	//prepare_fdp_write_pointer(fdp_ftl, USER_IO);
	//prepare_fdp_write_pointer(fdp_ftl, GC_IO);

	init_fdp_write_flow_control(fdp_ftl);

	NVMEV_INFO("Init FTL instance with %d channels (%ld pages)\n", fdp_ftl->ssd->sp.nchs,
		   fdp_ftl->ssd->sp.tt_pgs);

	return;
}

static void fdp_remove_ftl(struct fdp_ftl *fdp_ftl)
{
	remove_fdp_lines(fdp_ftl);
	remove_rmap((struct conv_ftl *) fdp_ftl);
	remove_maptbl((struct conv_ftl *) fdp_ftl);
	remove_reclaim_group(fdp_ftl);
	remove_fdp_placement(fdp_ftl);

}
#endif

static void conv_init_ftl(struct conv_ftl *conv_ftl, struct convparams *cpp, struct ssd *ssd)
{
	/*copy convparams*/
	conv_ftl->cp = *cpp;

	conv_ftl->ssd = ssd;

	/* initialize maptbl */
	init_maptbl(conv_ftl); // mapping table

	/* initialize rmap */
	init_rmap(conv_ftl); // reverse mapping table (?)

	/* initialize all the lines */
	init_lines(conv_ftl);

	/* initialize write pointer, this is how we allocate new pages for writes */
	prepare_write_pointer(conv_ftl, USER_IO);
	prepare_write_pointer(conv_ftl, GC_IO);

	init_write_flow_control(conv_ftl);

	NVMEV_INFO("Init FTL instance with %d channels (%ld pages)\n", conv_ftl->ssd->sp.nchs,
		   conv_ftl->ssd->sp.tt_pgs);

	return;
}

static void conv_remove_ftl(struct conv_ftl *conv_ftl)
{
	remove_lines(conv_ftl);
	remove_rmap(conv_ftl);
	remove_maptbl(conv_ftl);
}

static void conv_init_params(struct convparams *cpp)
{
	cpp->op_area_pcent = OP_AREA_PERCENT;
	cpp->gc_thres_lines = 2; /* Need only two lines.(host write, gc)*/
	cpp->gc_thres_lines_high = 2; /* Need only two lines.(host write, gc)*/
	cpp->enable_gc_delay = 1;
	cpp->pba_pcent = (int)((1 + cpp->op_area_pcent) * 100);
}


#ifdef FDP_SIMULATOR

void fdp_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
uint32_t cpu_nr_dispatcher, struct nvmev_ns_host_sw_specified *host_spec)
{
	struct ssdparams spp;
	struct convparams cpp;
	struct fdp_ftl *fdp_ftls;
	struct ssd *ssd;
	uint32_t i;
	const uint32_t nr_parts = SSD_PARTITIONS;

	ssd_init_fdp_params(&spp, size, nr_parts, host_spec);
	conv_init_params(&cpp);

	fdp_ftls = kmalloc(sizeof(struct fdp_ftl) * nr_parts, GFP_KERNEL);

	for (i = 0; i < nr_parts; i++) {
		ssd = kmalloc(sizeof(struct ssd), GFP_KERNEL);
		ssd_init(ssd, &spp, cpu_nr_dispatcher);
		fdp_init_ftl(&fdp_ftls[i], i, &cpp, ssd);
	}

	/* PCIe, Write buffer are shared by all instances*/
	for (i = 1; i < nr_parts; i++) {
		kfree(fdp_ftls[i].ssd->pcie->perf_model);
		kfree(fdp_ftls[i].ssd->pcie);
		kfree(fdp_ftls[i].ssd->write_buffer);

		fdp_ftls[i].ssd->pcie = fdp_ftls[0].ssd->pcie;
		fdp_ftls[i].ssd->write_buffer = fdp_ftls[0].ssd->write_buffer;
	}

	ns->id = id;
	ns->csi = NVME_CSI_NVM;
	ns->nr_parts = nr_parts;
	ns->ftls = (void *)fdp_ftls;
	ns->size = (uint64_t)((size * 100) / cpp.pba_pcent);
	ns->mapped = mapped_addr;
	/*register io command handler*/
	ns->proc_io_cmd = conv_proc_nvme_io_cmd;

	NVMEV_INFO("FTL physical space: %lld, logical space: %lld (physical/logical * 100 = %d)\n",
		   size, ns->size, cpp.pba_pcent);

	return;
}
#endif //FDP_SIMULATOR

void conv_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			 uint32_t cpu_nr_dispatcher)
{
	struct ssdparams spp;
	struct convparams cpp;
	struct conv_ftl *conv_ftls;
	struct ssd *ssd;
	uint32_t i;
	const uint32_t nr_parts = SSD_PARTITIONS;

	ssd_init_params(&spp, size, nr_parts);
	conv_init_params(&cpp);

	conv_ftls = kmalloc(sizeof(struct conv_ftl) * nr_parts, GFP_KERNEL);

	for (i = 0; i < nr_parts; i++) {
		ssd = kmalloc(sizeof(struct ssd), GFP_KERNEL);
		ssd_init(ssd, &spp, cpu_nr_dispatcher);
		conv_init_ftl(&conv_ftls[i], &cpp, ssd);
	}

	/* PCIe, Write buffer are shared by all instances*/
	for (i = 1; i < nr_parts; i++) {
		kfree(conv_ftls[i].ssd->pcie->perf_model);
		kfree(conv_ftls[i].ssd->pcie);
		kfree(conv_ftls[i].ssd->write_buffer);

		conv_ftls[i].ssd->pcie = conv_ftls[0].ssd->pcie;
		conv_ftls[i].ssd->write_buffer = conv_ftls[0].ssd->write_buffer;
	}

	ns->id = id;
	ns->csi = NVME_CSI_NVM;
	ns->nr_parts = nr_parts;
	ns->ftls = (void *)conv_ftls;
	ns->size = (uint64_t)((size * 100) / cpp.pba_pcent);
	ns->mapped = mapped_addr;
	/*register io command handler*/
	ns->proc_io_cmd = conv_proc_nvme_io_cmd;

	NVMEV_INFO("FTL physical space: %lld, logical space: %lld (physical/logical * 100 = %d)\n",
		   size, ns->size, cpp.pba_pcent);

	return;
}


void conv_remove_namespace(struct nvmev_ns *ns)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	const uint32_t nr_parts = SSD_PARTITIONS;
	uint32_t i;

	/* PCIe, Write buffer are shared by all instances*/
	for (i = 1; i < nr_parts; i++) {
		/*
		 * These were freed from conv_init_namespace() already.
		 * Mark these NULL so that ssd_remove() skips it.
		 */
		conv_ftls[i].ssd->pcie = NULL;
		conv_ftls[i].ssd->write_buffer = NULL;
	}

	for (i = 0; i < nr_parts; i++) {
		conv_remove_ftl(&conv_ftls[i]);
		ssd_remove(conv_ftls[i].ssd);
		kfree(conv_ftls[i].ssd);
	}

	kfree(conv_ftls);
	ns->ftls = NULL;
}

static inline bool valid_ppa(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	int ch = ppa->g.ch;
	int lun = ppa->g.lun;
	int pl = ppa->g.pl;
	int blk = ppa->g.blk;
	int pg = ppa->g.pg;
	//int sec = ppa->g.sec;

	if (ch < 0 || ch >= spp->nchs)
		return false;
	if (lun < 0 || lun >= spp->luns_per_ch)
		return false;
	if (pl < 0 || pl >= spp->pls_per_lun)
		return false;
	if (blk < 0 || blk >= spp->blks_per_pl)
		return false;
	if (pg < 0 || pg >= spp->pgs_per_blk)
		return false;

	return true;
}

static inline bool valid_lpn(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	return (lpn < conv_ftl->ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
	return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct line *get_line(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	return &(conv_ftl->lm.lines[ppa->g.blk]);
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	bool was_full_line = false;
	struct line *line;

	/* update corresponding page status */
	pg = get_pg(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(pg->status == PG_VALID);
	pg->status = PG_INVALID;

	/* update corresponding block status */
	blk = get_blk(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
	blk->ipc++;
	NVMEV_ASSERT(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
	blk->vpc--;

	/* update corresponding line status */
	line = get_line(conv_ftl, ppa);
	NVMEV_ASSERT(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
	//NVMEV_INFO("[NoFreeLine] %s() line->vpc %d spp->pgs_per_line %d\n", __func__,line->vpc, spp->pgs_per_line);
	if (line->vpc == spp->pgs_per_line) {
		NVMEV_ASSERT(line->ipc == 0);
		was_full_line = true;
	}
	line->ipc++;
	NVMEV_ASSERT(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
	/* Adjust the position of the victime line in the pq under over-writes */
	if (line->pos) {
		/* Note that line->vpc will be updated by this call */
		pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
	} else {
		line->vpc--;
	}

	if (was_full_line) {
		/* move line: "full" -> "victim" */
		list_del_init(&line->entry);
		lm->full_line_cnt--;
		//NVMEV_INFO("[NoFreeLine] %s() full_line_cnt-- %d\n", __func__,lm->full_line_cnt);
		pqueue_insert(lm->victim_line_pq, line);
		lm->victim_line_cnt++;
		//NVMEV_INFO("[NoFreeLine] %s() victim_line_cnt++ %d\n", __func__,lm->victim_line_cnt);
		//NVMEV_INFO("[NoFreeLine] %s() full_line -> victim_line\n", __func__);
	}
}

static void mark_page_valid(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	struct line *line;

	/* update page status */
	pg = get_pg(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(pg->status == PG_FREE);
	pg->status = PG_VALID;

	/* update corresponding block status */
	blk = get_blk(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(blk->vpc >= 0 && blk->vpc < spp->pgs_per_blk);
	blk->vpc++;

	/* update corresponding line status */
	line = get_line(conv_ftl, ppa);
	NVMEV_ASSERT(line->vpc >= 0 && line->vpc < spp->pgs_per_line);
	line->vpc++;
}

static void mark_block_free(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_block *blk = get_blk(conv_ftl->ssd, ppa);
	struct nand_page *pg = NULL;
	int i;

	for (i = 0; i < spp->pgs_per_blk; i++) {
		/* reset page status */
		pg = &blk->pg[i];
		NVMEV_ASSERT(pg->nsecs == spp->secs_per_pg);
		pg->status = PG_FREE;
	}

	/* reset block status */
	NVMEV_ASSERT(blk->npgs == spp->pgs_per_blk);
	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt++;
}

static void gc_read_page(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	/* advance conv_ftl status, we don't care about how long it takes */
	if (cpp->enable_gc_delay) {
		struct nand_cmd gcr = {
			.type = GC_IO,
			.cmd = NAND_READ,
			.stime = 0,
			.xfer_size = spp->pgsz,
			.interleave_pci_dma = false,
			.ppa = ppa,
		};
		ssd_advance_nand(conv_ftl->ssd, &gcr);
	}
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct conv_ftl *conv_ftl, struct ppa *old_ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct ppa new_ppa;
	uint64_t lpn = get_rmap_ent(conv_ftl, old_ppa);

	NVMEV_ASSERT(valid_lpn(conv_ftl, lpn));
	new_ppa = get_new_page(conv_ftl, GC_IO);
	/* update maptbl */
	set_maptbl_ent(conv_ftl, lpn, &new_ppa);
	/* update rmap */
	set_rmap_ent(conv_ftl, lpn, &new_ppa);

	mark_page_valid(conv_ftl, &new_ppa);

	/* need to advance the write pointer here */
	advance_write_pointer(conv_ftl, GC_IO);

	if (cpp->enable_gc_delay) {
		struct nand_cmd gcw = {
			.type = GC_IO,
			.cmd = NAND_NOP,
			.stime = 0,
			.interleave_pci_dma = false,
			.ppa = &new_ppa,
		};
		if (last_pg_in_wordline(conv_ftl, &new_ppa)) {
			gcw.cmd = NAND_WRITE;
			gcw.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
		}

		ssd_advance_nand(conv_ftl->ssd, &gcw);
	}

	/* advance per-ch gc_endtime as well */
#if 0
	new_ch = get_ch(conv_ftl, &new_ppa);
	new_ch->gc_endtime = new_ch->next_ch_avail_time;

	new_lun = get_lun(conv_ftl, &new_ppa);
	new_lun->gc_endtime = new_lun->next_lun_avail_time;
#endif

	return 0;
}
#ifdef FDP_SIMULATOR
/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t fdp_gc_write_page(struct fdp_ftl *fdp_ftl, uint32_t phnd_id, struct ppa *old_ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct ppa new_ppa;
	uint64_t lpn = get_rmap_ent((struct conv_ftl *) fdp_ftl, old_ppa);

	NVMEV_ASSERT(valid_lpn((struct conv_ftl *) fdp_ftl, lpn));

	new_ppa = get_fdp_new_page(fdp_ftl, phnd_id, GC_IO);

	/* update maptbl */
	set_maptbl_ent((struct conv_ftl *) fdp_ftl, lpn, &new_ppa);
	/* update rmap */
	set_rmap_ent((struct conv_ftl *) fdp_ftl, lpn, &new_ppa);

	mark_page_valid((struct conv_ftl *) fdp_ftl, &new_ppa);

	/* need to advance the write pointer here */
	advance_fdp_write_pointer(conv_ftl, phnd_id, GC_IO);

	if (cpp->enable_gc_delay) {
		struct nand_cmd gcw = {
			.type = GC_IO,
			.cmd = NAND_NOP,
			.stime = 0,
			.interleave_pci_dma = false,
			.ppa = &new_ppa,
		};
		if (last_pg_in_wordline(conv_ftl, &new_ppa)) {
			gcw.cmd = NAND_WRITE;
			gcw.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
		}

		ssd_advance_nand(conv_ftl->ssd, &gcw);
	}
	return 0;
}

static struct reclaim_unit *select_victim_ru(struct fdp_ftl *fdp_ftl, int gc_rg_idx, bool force)
{
	struct ssdparams *spp = &fdp_ftl->ssd->sp;

	struct line_mgmt *lm = &fdp_ftl->lm;
	struct line *victim_line = NULL;

	victim_line = pqueue_peek(lm->victim_line_pq);
	if (!victim_line) {
		//NVMEV_INFO("[NoFreeLine] %s() victim_line_pq is NULL \n", __func__);
		return NULL;
	}

	if (!force && (victim_line->vpc > (spp->pgs_per_line / 8))) {
		//NVMEV_INFO("[NoFreeLine] %s() vpc > pgs_per_line \n", __func__);
		return NULL;
	}

	pqueue_pop(lm->victim_line_pq);
	victim_line->pos = 0;
	lm->victim_line_cnt--;
	//NVMEV_INFO("[NoFreeLine] %s() victim_line_cnt-- %d\n", __func__,lm->victim_line_cnt);

	/* victim_line is a danggling node now */
	return victim_line;
}
#endif //FDP_SIMULATOR

static struct line *select_victim_line(struct conv_ftl *conv_ftl, bool force)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *victim_line = NULL;

	victim_line = pqueue_peek(lm->victim_line_pq);
	if (!victim_line) {
		//NVMEV_INFO("[NoFreeLine] %s() victim_line_pq is NULL \n", __func__);
		return NULL;
	}

	if (!force && (victim_line->vpc > (spp->pgs_per_line / 8))) {
		//NVMEV_INFO("[NoFreeLine] %s() vpc > pgs_per_line \n", __func__);
		return NULL;
	}

	pqueue_pop(lm->victim_line_pq);
	victim_line->pos = 0;
	lm->victim_line_cnt--;
	//NVMEV_INFO("[NoFreeLine] %s() victim_line_cnt-- %d\n", __func__,lm->victim_line_cnt);

	/* victim_line is a danggling node now */
	return victim_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0;
	int pg;

	for (pg = 0; pg < spp->pgs_per_blk; pg++) {
		ppa->g.pg = pg;
		pg_iter = get_pg(conv_ftl->ssd, ppa);
		/* there shouldn't be any free page in victim blocks */
		NVMEV_ASSERT(pg_iter->status != PG_FREE);
		if (pg_iter->status == PG_VALID) {
			gc_read_page(conv_ftl, ppa);
			/* delay the maptbl update until "write" happens */
			gc_write_page(conv_ftl, ppa);
			cnt++;
		}
	}

	NVMEV_ASSERT(get_blk(conv_ftl->ssd, ppa)->vpc == cnt);
}

/* here ppa identifies the block we want to clean */
static void clean_one_flashpg(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0, i = 0;
	uint64_t completed_time = 0;
	struct ppa ppa_copy = *ppa;

	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(conv_ftl->ssd, &ppa_copy);
		/* there shouldn't be any free page in victim blocks */
		NVMEV_ASSERT(pg_iter->status != PG_FREE);
		if (pg_iter->status == PG_VALID)
			cnt++;

		ppa_copy.g.pg++;
	}

	ppa_copy = *ppa;

	if (cnt <= 0)
		return;

	if (cpp->enable_gc_delay) {
		struct nand_cmd gcr = {
			.type = GC_IO,
			.cmd = NAND_READ,
			.stime = 0,
			.xfer_size = spp->pgsz * cnt,
			.interleave_pci_dma = false,
			.ppa = &ppa_copy,
		};
		completed_time = ssd_advance_nand(conv_ftl->ssd, &gcr);
	}

	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(conv_ftl->ssd, &ppa_copy);

		/* there shouldn't be any free page in victim blocks */
		if (pg_iter->status == PG_VALID) {
			/* delay the maptbl update until "write" happens */
			gc_write_page(conv_ftl, &ppa_copy);
		}

		ppa_copy.g.pg++;
	}
}

static void mark_line_free(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *line = get_line(conv_ftl, ppa);
	line->ipc = 0;
	line->vpc = 0;
	/* move this line to free line list */
	list_add_tail(&line->entry, &lm->free_line_list);
	lm->free_line_cnt++;
	//NVMEV_INFO("[NoFreeLine] %s free_line_cnt++ %d \n", __func__, lm->free_line_cnt);
}

static int do_gc(struct conv_ftl *conv_ftl, bool force)
{
	struct line *victim_line = NULL;
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct ppa ppa;
	int flashpg;

	victim_line = select_victim_line(conv_ftl, force);
	if (!victim_line) {
		//NVMEV_INFO("[NoFreeLine] %s victiom_line is NULL \n", __func__);
		return -1;
	}

	ppa.g.blk = victim_line->id;
	NVMEV_DEBUG_VERBOSE("GC-ing line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d\n", ppa.g.blk,
		    victim_line->ipc, victim_line->vpc, conv_ftl->lm.victim_line_cnt,
		    conv_ftl->lm.full_line_cnt, conv_ftl->lm.free_line_cnt);

	conv_ftl->wfc.credits_to_refill = victim_line->ipc;

	/* copy back valid data */
	for (flashpg = 0; flashpg < spp->flashpgs_per_blk; flashpg++) {
		int ch, lun;

		ppa.g.pg = flashpg * spp->pgs_per_flashpg;
		for (ch = 0; ch < spp->nchs; ch++) {
			for (lun = 0; lun < spp->luns_per_ch; lun++) {
				struct nand_lun *lunp;

				ppa.g.ch = ch;
				ppa.g.lun = lun;
				ppa.g.pl = 0;
				lunp = get_lun(conv_ftl->ssd, &ppa);
				clean_one_flashpg(conv_ftl, &ppa);

				if (flashpg == (spp->flashpgs_per_blk - 1)) {
					struct convparams *cpp = &conv_ftl->cp;

					mark_block_free(conv_ftl, &ppa);

					if (cpp->enable_gc_delay) {
						struct nand_cmd gce = {
							.type = GC_IO,
							.cmd = NAND_ERASE,
							.stime = 0,
							.interleave_pci_dma = false,
							.ppa = &ppa,
						};
						ssd_advance_nand(conv_ftl->ssd, &gce);
					}

					lunp->gc_endtime = lunp->next_lun_avail_time;
				}
			}
		}
	}

	/* update line status */
	mark_line_free(conv_ftl, &ppa);

	return 0;
}

static void foreground_gc(struct conv_ftl *conv_ftl)
{
	if (should_gc_high(conv_ftl)) {
		NVMEV_DEBUG_VERBOSE("should_gc_high passed");
		/* perform GC here until !should_gc(conv_ftl) */
		do_gc(conv_ftl, true);
		//NVMEV_INFO("[NoFreeLine] %s <- do_gc() : %d\n", __func__, ret);
	}
}

#ifdef FDP_SIMULATOR
static int fdp_do_gc(struct fdp_ftl *fdp_ftl, bool force)
{
	struct reclaim_unit *victim_ru = NULL;

	struct line *victim_line = NULL;
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct ppa ppa;
	int flashpg;


	victim_ru = select_victim_ru (fdp_ftl, force);

	victim_line = select_victim_line((struct conv_ftl *) fdp_ftl, force);
	if (!victim_line) {
		//NVMEV_INFO("[NoFreeLine] %s victiom_line is NULL \n", __func__);
		return -1;
	}

	ppa.g.blk = victim_line->id;
	NVMEV_INFO("GC-ing line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d\n", ppa.g.blk,
		    victim_line->ipc, victim_line->vpc, fdp_ftl->lm.victim_line_cnt,
		    fdp_ftl->lm.full_line_cnt, fdp_ftl->lm.free_line_cnt);

	fdp_ftl->wfc.credits_to_refill = victim_line->ipc;

	/* copy back valid data */
	for (flashpg = 0; flashpg < spp->flashpgs_per_blk; flashpg++) {
		int ch, lun;

		ppa.g.pg = flashpg * spp->pgs_per_flashpg;
		for (ch = 0; ch < spp->nchs; ch++) {
			for (lun = 0; lun < spp->luns_per_ch; lun++) {
				struct nand_lun *lunp;

				ppa.g.ch = ch;
				ppa.g.lun = lun;
				ppa.g.pl = 0;
				lunp = get_lun(fdp_ftl->ssd, &ppa);
				clean_one_flashpg(fdp_ftl, &ppa);

				if (flashpg == (spp->flashpgs_per_blk - 1)) {
					struct convparams *cpp = &conv_ftl->cp;

					mark_block_free(conv_ftl, &ppa);

					if (cpp->enable_gc_delay) {
						struct nand_cmd gce = {
							.type = GC_IO,
							.cmd = NAND_ERASE,
							.stime = 0,
							.interleave_pci_dma = false,
							.ppa = &ppa,
						};
						ssd_advance_nand(conv_ftl->ssd, &gce);
					}

					lunp->gc_endtime = lunp->next_lun_avail_time;
				}
			}
		}
	}

	/* update line status */
	mark_line_free(conv_ftl, &ppa);

	return 0;
}

static void fdp_foreground_gc(struct fdp_ftl *fdp_ftl)
{
	if (should_gc_high(conv_ftl)) {
		NVMEV_DEBUG_VERBOSE("should_gc_high passed");
		/* perform GC here until !should_gc(conv_ftl) */
		fdp_do_gc(fdp_ftl, true);
		//NVMEV_INFO("[NoFreeLine] %s <- do_gc() : %d\n", __func__, ret);
	}
}
#endif //FDP_SIMULATOR

static bool is_same_flash_page(struct conv_ftl *conv_ftl, struct ppa ppa1, struct ppa ppa2)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint32_t ppa1_page = ppa1.g.pg / spp->pgs_per_flashpg;
	uint32_t ppa2_page = ppa2.g.pg / spp->pgs_per_flashpg;

	return (ppa1.h.blk_in_ssd == ppa2.h.blk_in_ssd) && (ppa1_page == ppa2_page);
}

static bool conv_read(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];
	/* spp are shared by all instances*/
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
	uint64_t lpn;
	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_completed, nsecs_latest = nsecs_start;
	uint32_t xfer_size, i;
	uint32_t nr_parts = ns->nr_parts;

	struct ppa prev_ppa;
	struct nand_cmd srd = {
		.type = USER_IO,
		.cmd = NAND_READ,
		.stime = nsecs_start,
		.interleave_pci_dma = true,
	};

	NVMEV_ASSERT(conv_ftls);
	NVMEV_DEBUG_VERBOSE("%s: start_lpn=%lld, len=%lld, end_lpn=%lld", __func__, start_lpn, nr_lba, end_lpn);
	if ((end_lpn / nr_parts) >= spp->tt_pgs) {
		NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n", __func__,
			    start_lpn, spp->tt_pgs);
		return false;
	}

	if (LBA_TO_BYTE(nr_lba) <= (KB(4) * nr_parts)) {
		srd.stime += spp->fw_4kb_rd_lat;
	} else {
		srd.stime += spp->fw_rd_lat;
	}

	for (i = 0; (i < nr_parts) && (start_lpn <= end_lpn); i++, start_lpn++) {
		conv_ftl = &conv_ftls[start_lpn % nr_parts];
		xfer_size = 0;
		prev_ppa = get_maptbl_ent(conv_ftl, start_lpn / nr_parts);

		/* normal IO read path */
		for (lpn = start_lpn; lpn <= end_lpn; lpn += nr_parts) {
			uint64_t local_lpn;
			struct ppa cur_ppa;

			local_lpn = lpn / nr_parts;
			cur_ppa = get_maptbl_ent(conv_ftl, local_lpn);
			if (!mapped_ppa(&cur_ppa) || !valid_ppa(conv_ftl, &cur_ppa)) {
				NVMEV_DEBUG_VERBOSE("lpn 0x%llx not mapped to valid ppa\n", local_lpn);
				NVMEV_DEBUG_VERBOSE("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d\n",
					    cur_ppa.g.ch, cur_ppa.g.lun, cur_ppa.g.blk,
					    cur_ppa.g.pl, cur_ppa.g.pg);
				continue;
			}

			// aggregate read io in same flash page
			if (mapped_ppa(&prev_ppa) &&
			    is_same_flash_page(conv_ftl, cur_ppa, prev_ppa)) {
				xfer_size += spp->pgsz;
				continue;
			}

			if (xfer_size > 0) {
				srd.xfer_size = xfer_size;
				srd.ppa = &prev_ppa;
				nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd);
				nsecs_latest = max(nsecs_completed, nsecs_latest);
			}

			xfer_size = spp->pgsz;
			prev_ppa = cur_ppa;
		}

		// issue remaining io
		if (xfer_size > 0) {
			srd.xfer_size = xfer_size;
			srd.ppa = &prev_ppa;
			nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd);
			nsecs_latest = max(nsecs_completed, nsecs_latest);
		}
	}

	ret->nsecs_target = nsecs_latest;
	ret->status = NVME_SC_SUCCESS;
	return true;
}

#ifdef FDP_SIMULATOR
static bool is_ru_available(struct nvmev_reclaim_unit *ru) {
	if (ru->ref_cnt != 0) {
		return false;
	}

	if (ru->ruh != NULL) {
		// if ref_cnt is 0, this ru doesn't referenced by a ruh
		return false;
	}
	
	if (ru->ruamw <= 0 ) {
		return false;
	}

	return true;
}

static void ru_write (struct nvmev_reclaim_unit *ru, uint64_t nr_lba) {
	NVMEV_ASSERT(ru != NULL);

check_size:
	if (ru->ruamw > nr_lba)
		goto ru_write;

	struct nvmev_reclaim_unit_handle *ruh = ru->ruh;
	struct nvmev_reclaim_group *rg = ru->rg;
	NVMEV_ASSERT(ruh != NULL);
	NVMEV_ASSERT(rg != NULL);


	/*
	NVMEV_INFO("%s() rg %d ru %d nr_lba %d ru->ruamw %d\n", 
			__func__, rg->id, ru->id, nr_lba, ru->ruamw);
			*/
	nr_lba -= ru->ruamw;
	ru->ruamw = 0;

	// Dereferrence RU
	ru->ref_cnt--;
	ru->ruh = NULL;

	// Find New Reclaim Unit
	int i;
	for (i = 1; i < 64; i++) {
		int ru_idx = (ru->id + i) % 64;
		if (is_ru_available(&rg->ru[ru_idx])) {
			ru =  &rg->ru[ru_idx];
			ru->ruh = ruh;
			ruh->ru[rg->id] = ru;
			ru->ref_cnt ++;
			/*
			NVMEV_INFO("%s() find new RU rg %d ru %d nr_lba %d ru->ruamw %d\n", 
					__func__, rg->id, ru->id, nr_lba, ru->ruamw);
					*/
			goto check_size;
		}
	}
	NVMEV_INFO("%s() There is no RU\n", 
			__func__);

ru_write:
	ru->ruamw -= nr_lba;
}

static void write_on_placement (struct nvmev_placement_handle_list *phndls, uint32_t phid, uint64_t nr_lba) {
	NVMEV_ASSERT(phndls != NULL);
	
	if (phid < phndls->nphndls) {
		struct nvmev_reclaim_unit_handle *ruh = phndls->phnd[phid].ruh;
		NVMEV_ASSERT(ruh != NULL);
		int i;
		for (i = 0; i < 16; i++) {
			ru_write (ruh->ru[i], nr_lba/16);
		}
	} else {
		NVMEV_INFO("[DSPEC] %s() Out of Bound (phid: %d) \n", __func__,phid);
	}
}

static bool fdp_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct fdp_ftl *fdp_ftls = (struct fdp_ftl *)ns->ftls;
	struct fdp_ftl *fdp_ftl = &fdp_ftls[0];

	/* wbuf and spp are shared by all instances */
	struct ssdparams *spp = &fdp_ftl->ssd->sp;
	struct buffer *wbuf = fdp_ftl->ssd->write_buffer;

	struct nvme_command *cmd = req->cmd;

	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;

	uint64_t lpn;
	uint32_t nr_parts = ns->nr_parts;

	uint64_t nsecs_latest;
	uint64_t nsecs_xfer_completed;
	uint32_t allocated_buf_size;

	struct nand_cmd swr = {
		.type = USER_IO,
		.cmd = NAND_WRITE,
		.interleave_pci_dma = false,
		.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg,
	};

	uint32_t dspec = 0;
	write_on_placement(ns->eg->phndls, dspec, nr_lba);
	dspec = NVME_GET(cmd->rw.dsmgmt, READ_WRITE_CDW13_DSPEC);

	/*
	if (dspec != 0) {
		NVMEV_INFO("[DSPEC] %s() dspec: %d\n", __func__,dspec,);
	}

	NVMEV_INFO("%s: start_lpn=%lld, len=%lld, end_lpn=%lld nr_parts %d", 
			__func__, start_lpn, nr_lba, end_lpn, nr_parts);
	*/
	if ((end_lpn / nr_parts) >= spp->tt_pgs) {
		NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n",
				__func__, start_lpn, spp->tt_pgs);
		return false;
	}

	allocated_buf_size = buffer_allocate(wbuf, LBA_TO_BYTE(nr_lba));
	if (allocated_buf_size < LBA_TO_BYTE(nr_lba))
		return false;

	nsecs_latest =
		ssd_advance_write_buffer(fdp_ftl->ssd, req->nsecs_start, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;

	swr.stime = nsecs_latest;

	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		uint64_t local_lpn;
		uint64_t nsecs_completed = 0;
		struct ppa ppa;

		fdp_ftl = &fdp_ftls[lpn % nr_parts];
		local_lpn = lpn / nr_parts;
		ppa = get_maptbl_ent(
			(struct conv_ftl *) fdp_ftl, local_lpn); // Check whether the given LPN has been written before
		if (mapped_ppa(&ppa)) {
			/* update old page information first */
			mark_page_invalid( (struct conv_ftl *) fdp_ftl, &ppa);
			set_rmap_ent( (struct conv_ftl *) fdp_ftl, INVALID_LPN, &ppa);
			NVMEV_INFO("%s: %lld is invalid, ", __func__, ppa2pgidx((struct conv_ftl *) fdp_ftl, &ppa));
		}

		/* new write */
		ppa = get_fdp_new_page(fdp_ftl, dspec, USER_IO);

		/* update maptbl */
		set_maptbl_ent((struct conv_ftl *) fdp_ftl, local_lpn, &ppa);
		NVMEV_INFO("%s: got new ppa %lld, ", __func__, ppa2pgidx((struct conv_ftl *) fdp_ftl, &ppa));
		/* update rmap */
		set_rmap_ent((struct conv_ftl *) fdp_ftl, local_lpn, &ppa);

		mark_page_valid((struct conv_ftl *) fdp_ftl, &ppa);

		/* need to advance the write pointer here */
		advance_fdp_write_pointer(fdp_ftl, dspec, USER_IO);

		/* Aggregate write io in flash page */
		if (last_pg_in_wordline((struct conv_ftl *) fdp_ftl, &ppa)) {
			swr.ppa = &ppa;

			nsecs_completed = ssd_advance_nand(fdp_ftl->ssd, &swr);
			nsecs_latest = max(nsecs_completed, nsecs_latest);

			schedule_internal_operation(req->sq_id, nsecs_completed, wbuf,
						    spp->pgs_per_oneshotpg * spp->pgsz);
		}

		consume_write_credit((struct conv_ftl *) fdp_ftl);
		check_and_refill_write_credit((struct conv_ftl *) fdp_ftl);
	}

	if ((cmd->rw.control & NVME_RW_FUA) || (spp->write_early_completion == 0)) {
		/* Wait all flash operations */
		ret->nsecs_target = nsecs_latest;
	} else {
		/* Early completion */
		ret->nsecs_target = nsecs_xfer_completed;
	}
	ret->status = NVME_SC_SUCCESS;

	return true;
}
#endif //FDP_SIMULATOR

static bool conv_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];

	/* wbuf and spp are shared by all instances */
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct buffer *wbuf = conv_ftl->ssd->write_buffer;

	struct nvme_command *cmd = req->cmd;

	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;

	uint64_t lpn;
	uint32_t nr_parts = ns->nr_parts;

	uint64_t nsecs_latest;
	uint64_t nsecs_xfer_completed;
	uint32_t allocated_buf_size;

	struct nand_cmd swr = {
		.type = USER_IO,
		.cmd = NAND_WRITE,
		.interleave_pci_dma = false,
		.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg,
	};

	NVMEV_DEBUG_VERBOSE("%s: start_lpn=%lld, len=%lld, end_lpn=%lld", __func__, start_lpn, nr_lba, end_lpn);
	if ((end_lpn / nr_parts) >= spp->tt_pgs) {
		NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n",
				__func__, start_lpn, spp->tt_pgs);
		return false;
	}

	allocated_buf_size = buffer_allocate(wbuf, LBA_TO_BYTE(nr_lba));
	if (allocated_buf_size < LBA_TO_BYTE(nr_lba))
		return false;

	nsecs_latest =
		ssd_advance_write_buffer(conv_ftl->ssd, req->nsecs_start, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;

	swr.stime = nsecs_latest;

	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		uint64_t local_lpn;
		uint64_t nsecs_completed = 0;
		struct ppa ppa;

		conv_ftl = &conv_ftls[lpn % nr_parts];
		local_lpn = lpn / nr_parts;
		ppa = get_maptbl_ent(
			conv_ftl, local_lpn); // Check whether the given LPN has been written before
		if (mapped_ppa(&ppa)) {
			/* update old page information first */
			mark_page_invalid(conv_ftl, &ppa);
			set_rmap_ent(conv_ftl, INVALID_LPN, &ppa);
			NVMEV_DEBUG("%s: %lld is invalid, ", __func__, ppa2pgidx(conv_ftl, &ppa));
		}

		/* new write */
		ppa = get_new_page(conv_ftl, USER_IO);
		/* update maptbl */
		set_maptbl_ent(conv_ftl, local_lpn, &ppa);
		NVMEV_DEBUG("%s: got new ppa %lld, ", __func__, ppa2pgidx(conv_ftl, &ppa));
		/* update rmap */
		set_rmap_ent(conv_ftl, local_lpn, &ppa);

		mark_page_valid(conv_ftl, &ppa);

		/* need to advance the write pointer here */
		advance_write_pointer(conv_ftl, USER_IO);

		/* Aggregate write io in flash page */
		if (last_pg_in_wordline(conv_ftl, &ppa)) {
			swr.ppa = &ppa;

			nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &swr);
			nsecs_latest = max(nsecs_completed, nsecs_latest);

			schedule_internal_operation(req->sq_id, nsecs_completed, wbuf,
						    spp->pgs_per_oneshotpg * spp->pgsz);
		}

		consume_write_credit(conv_ftl);
		check_and_refill_write_credit(conv_ftl);
	}

	if ((cmd->rw.control & NVME_RW_FUA) || (spp->write_early_completion == 0)) {
		/* Wait all flash operations */
		ret->nsecs_target = nsecs_latest;
	} else {
		/* Early completion */
		ret->nsecs_target = nsecs_xfer_completed;
	}
	ret->status = NVME_SC_SUCCESS;

	return true;
}

static void conv_flush(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	uint64_t start, latest;
	uint32_t i;
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;

	start = local_clock();
	latest = start;
	for (i = 0; i < ns->nr_parts; i++) {
		latest = max(latest, ssd_next_idle_time(conv_ftls[i].ssd));
	}

	NVMEV_DEBUG_VERBOSE("%s: latency=%llu\n", __func__, latest - start);

	ret->status = NVME_SC_SUCCESS;
	ret->nsecs_target = latest;
	return;
}

#ifdef FDP_SIMULATOR
static void conv_io_mgmt_recv(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct nvme_command *cmd = req->cmd;
	struct nvme_ns_mgmt* mgmt = &cmd->ns_mgmt;
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;

	enum io_mgmt_recv_mo {
		no_action					= 0x0,
		reclaim_unit_handle_status	= 0x1,
		vendor_specific				= 0xff,
	};

	
	int mo = NVME_GET(mgmt->cdw10, NAMESPACE_MGMT_CDW10_MO);
	switch (mo) {
	case no_action:
		NVMEV_INFO("[COMMAND] %s() NO ACTION!!\n", 
				__func__);
		break;
	case reclaim_unit_handle_status:
		int nsid = mgmt->nsid - 1;
		NVMEV_INFO("[COMMAND] %s() Reclaim Unit Handle Status for NSID: %d\n", 
				__func__, nsid);
		NVMEV_INFO( "[COMMAND] ns_mgmt: opcode %x flags %x command_id %x nsid %x cdw2 %u cdw3 %u metadata 0x%p addr 0x%p metadata_len %u data_len %u cdw10 %u cdw11 %u cdw12 %u cdw13 %u cdw14 %u cdw15 %u\n",
					mgmt->opcode,
					mgmt->flags,
					mgmt->command_id,
					mgmt->nsid,
					mgmt->cdw2,
					mgmt->cdw3,
					mgmt->metadata,
					mgmt->addr,
					mgmt->metadata_len,
					mgmt->data_len,
					mgmt->cdw10,
					mgmt->cdw11,
					mgmt->cdw12,
					mgmt->cdw13,
					mgmt->cdw14,
					mgmt->cdw15);
		NVMEV_ASSERT(nsid < MAX_NAMESPACES && nsid >= 0);

		struct nvmev_ns *nsp = &nvmev_vdev->ns[nsid];
		NVMEV_ASSERT(nsp != NULL);

		struct nvmev_endg *eg = nsp->eg;
		NVMEV_ASSERT(eg != NULL);

		if (!eg->fdp_enable) {
			ret->status = NVME_SC_FDP_DISABLED;
			return;
		}

		struct nvmev_placement_handle_list *phndls = eg->phndls;
		NVMEV_ASSERT(phndls != NULL);

		struct nvme_fdp_ruh_status* ruhs = prp_address(mgmt->addr);

		int remain_data_len = (mgmt->cdw11+1) << 2;
		
		if (remain_data_len >= sizeof(*ruhs)) {
			ruhs->nruhsd = phndls->nphndls;
			remain_data_len -= sizeof(*ruhs);
		} else {
			ret->status = NVME_SC_SUCCESS;
			return;
		}

		int max_nruhsd = remain_data_len / sizeof(struct nvme_fdp_ruh_status_desc);
		int res_nruhsd = min(phndls->nphndls, max_nruhsd);

		//NVMEV_INFO("[COMMAND] %s() ruhs 0x%p mgmt.data_len %d max_nruhsd %d  data_len %d phndls->nphndls %d\n", 
		//			__func__, ruhs ,mgmt->data_len, max_nruhsd, (mgmt->cdw11+1)<<2, phndls->nphndls);
		// NVMEV_ASSERT(phndls->nphndls < max_nruhsd);

		int i;
		for (i = 0; i < res_nruhsd; i++) {
			ruhs->ruhss[i].pid = phndls->phnd[i].id;

			struct nvmev_reclaim_unit_handle *ruh = phndls->phnd[i].ruh;
			ruhs->ruhss[i].ruhid = ruh->id;
			ruhs->ruhss[i].earutr = 0;
			ruhs->ruhss[i].ruamw = 0;
			int j;
			for (j = 0; j < 16; j++) {
				struct nvmev_reclaim_unit *ru = ruh->ru[j];
				NVMEV_ASSERT(ru != NULL);
				ruhs->ruhss[i].ruamw += ru->ruamw;
			}
		}

		NVMEV_INFO("[COMMAND] %s() Reclaim Unit Handle Status: Send %d descriptor\n", 
				__func__, res_nruhsd);
		for (i = 0; i < res_nruhsd ; i++) {
			NVMEV_INFO( "Descriptor %d pid: %d ruhid: %d ruamw: %d\n",
						i,
						ruhs->ruhss[i].pid,
						ruhs->ruhss[i].ruhid,
						ruhs->ruhss[i].ruamw);
			struct nvmev_reclaim_unit_handle *ruh = phndls->phnd[i].ruh;
			int j;
			for (j = 0; j < 16; j++) {
				struct nvmev_reclaim_unit *ru = ruh->ru[j];
				NVMEV_INFO( " RU %d ruid: %d ref_cnt: %d ruamw: %d\n",
							j,
							ru->id,
							ru->ref_cnt,
							ru->ruamw);
			}
		}
		ret->status = NVME_SC_SUCCESS;

		break;
	case vendor_specific:
		ret->status = NVME_SC_INVALID_FIELD;
		NVMEV_INFO("[COMMAND] %s() Vendor Specific\n", 
				__func__);
		break;
	default:
		ret->status = NVME_SC_INVALID_FIELD;
		NVMEV_ERROR("[COMMAND] %s() command not implemented: 0x%x\n", 
				__func__, mo);
	}
}
#endif //FDP_SIMULATOR

bool conv_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct nvme_command *cmd = req->cmd;

	NVMEV_ASSERT(ns->csi == NVME_CSI_NVM);

	switch (cmd->common.opcode) {
	case nvme_cmd_write:
//#ifdef FDP_SIMULATOR
	//	struct nvmev_endg *eg = ns->eg;
		//NVMEV_INFO("%s: %s (0x%x) fdp enable: %d\n", __func__,
	//		nvme_opcode_string(cmd->common.opcode), cmd->common.opcode, eg->fdp_enable);
//#endif //FDP_SIMULATOR
		
#ifdef FDP_SIMULATOR
		NVMEV_ASSERT(ns->eg != NULL);
		if (ns->eg->fdp_enable)
			return fdp_write(ns, req, ret);
		else
			return conv_write(ns, req, ret);
#endif //FDP_SIMULATOR
		break;
	case nvme_cmd_read:
		if (!conv_read(ns, req, ret))
			return false;
		break;
	case nvme_cmd_flush:
		conv_flush(ns, req, ret);
		break;
#ifdef FDP_SIMULATOR
	// TODO: [KOO] IO Management Receive Command
	case nvme_cmd_io_mgmt_recv:
		conv_io_mgmt_recv(ns, req, ret);
		break;
#endif //FDP_SIMULATOR
	default:
		NVMEV_ERROR("%s: command not implemented: %s (0x%x)\n", __func__,
				nvme_opcode_string(cmd->common.opcode), cmd->common.opcode);
		break;
	}

	return true;
}
