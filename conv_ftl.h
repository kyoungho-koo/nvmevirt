// SPDX-License-Identifier: GPL-2.0-only

#ifndef _NVMEVIRT_CONV_FTL_H
#define _NVMEVIRT_CONV_FTL_H

#include <linux/types.h>
#include "pqueue/pqueue.h"
#include "ssd_config.h"
#include "ssd.h"

#ifdef FDP_SIMULATOR
struct fdpparams {
	uint32_t gc_thres_lines;
	uint32_t gc_thres_lines_high;
	bool enable_gc_delay;

	double op_area_pcent;
	int pba_pcent; /* (physical space / logical space) * 100*/
};
#endif //FDP_SIMULATOR
struct convparams {
	uint32_t gc_thres_lines;
	uint32_t gc_thres_lines_high;
	bool enable_gc_delay;

	double op_area_pcent;
	int pba_pcent; /* (physical space / logical space) * 100*/
};

struct line {
	int id; /* line id, the same as corresponding block id */
	int ipc; /* invalid page count in this line */
	int vpc; /* valid page count in this line */
	struct list_head entry;
	/* position in the priority queue for victim lines */
	size_t pos;
};

/* wp: record next write addr */
struct write_pointer {
	struct line *curline;
	uint32_t ch;
	uint32_t lun;
	uint32_t pg;
	uint32_t blk;
	uint32_t pl;
};

struct line_mgmt {
	struct line *lines;

	/* free line list, we only need to maintain a list of blk numbers */
	struct list_head free_line_list;
	pqueue_t *victim_line_pq;
	struct list_head full_line_list;

	uint32_t tt_lines;
	uint32_t free_line_cnt;
	uint32_t victim_line_cnt;
	uint32_t full_line_cnt;
};

struct write_flow_control {
	uint32_t write_credits;
	uint32_t credits_to_refill;
};


struct conv_ftl {
	struct ssd *ssd;

	struct convparams cp;
	struct ppa *maptbl; /* page level mapping table */
	uint64_t *rmap; /* reverse mapptbl, assume it's stored in OOB */
	struct write_pointer wp;
	struct write_pointer gc_wp;
	struct line_mgmt lm;
	struct write_flow_control wfc;
};

#ifdef FDP_SIMULATOR
struct ru_params {
	int start_ch; /* start channel */
	int nchs_per_ru; /* # of channels in the Reclaim Unit */
	int luns_per_ru; /* # of luns in the Reclaim Unit */
	int blks_per_ru; /* # of blks in the Reclaim Unit */

};

struct fdp_reclaim_unit {
	struct ru_params rp;

	int id;
	uint32_t ref_cnt;
	uint32_t ruamw;
	uint32_t blks;
	struct fdp_reclaim_group *rg;
	struct fdp_reclaim_unit_handle *ruh;
	struct write_pointer wp;
	struct write_pointer gc_wp;
};

struct fdp_reclaim_group {
	uint16_t id;
	int used_ru;
	struct fdp_reclaim_unit ru[RU_PER_RG];
};

struct fdp_reclaim_unit_handle {
	int id;
	int ru_idx;
	struct fdp_reclaim_unit *ru[RG_PER_FTL];
};

struct fdp_placement_handle {
	int id;
	struct fdp_reclaim_unit_handle *ruh;
};

struct fdp_placement_handle_list {
	uint16_t nphndls;
	struct fdp_placement_handle phnd[];
};

struct fdp_ftl {
	struct ssd *ssd;

	struct convparams cp;
	struct ppa *maptbl; /* page level mapping table */
	uint64_t *rmap; /* reverse mapptbl, assume it's stored in OOB */
	struct write_pointer wp;
	struct write_pointer gc_wp;
	struct line_mgmt lm;
	struct write_flow_control wfc;

	struct fdp_reclaim_group rg[RG_PER_FTL];
	struct fdp_placement_handle_list *phndls;
};

void fdp_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			 uint32_t cpu_nr_dispatcher, uint32_t nphndls);
#endif //FDP_SIMULATOR
void conv_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			 uint32_t cpu_nr_dispatcher);

void conv_remove_namespace(struct nvmev_ns *ns);

bool conv_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req,
			   struct nvmev_result *ret);

#endif
