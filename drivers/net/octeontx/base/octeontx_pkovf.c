/*
 *   BSD LICENSE
 *
 *   Copyright (C) Cavium Inc. 2017. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Cavium networks nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include <rte_eal.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_pci.h>
#include <rte_spinlock.h>

#include "../octeontx_logs.h"
#include "octeontx_io.h"
#include "octeontx_pkovf.h"

struct octeontx_pko_iomem {
	uint8_t		*va;
	phys_addr_t	iova;
	size_t		size;
};

#define PKO_IOMEM_NULL (struct octeontx_pko_iomem){0, 0, 0}

struct octeontx_pko_fc_ctl_s {
	int64_t buf_cnt;
	int64_t padding[(PKO_DQ_FC_STRIDE / 8) - 1];
};

struct octeontx_pkovf {
	uint8_t		*bar0;
	uint8_t		*bar2;
	uint16_t	domain;
	uint16_t	vfid;
};

struct octeontx_pko_vf_ctl_s {
	rte_spinlock_t lock;

	struct octeontx_pko_iomem fc_iomem;
	struct octeontx_pko_fc_ctl_s *fc_ctl;
	struct octeontx_pkovf pko[PKO_VF_MAX];
	struct {
		uint64_t chanid;
	} dq_map[PKO_VF_MAX * PKO_VF_NUM_DQ];
};

static struct octeontx_pko_vf_ctl_s pko_vf_ctl;

static void *
octeontx_pko_dq_vf_bar0(uint16_t txq)
{
	int vf_ix;

	vf_ix = txq / PKO_VF_NUM_DQ;
	return pko_vf_ctl.pko[vf_ix].bar0;
}

static int
octeontx_pko_dq_gdq(uint16_t txq)
{
	return txq % PKO_VF_NUM_DQ;
}

/**
 * Open a PKO DQ.
 */
static inline
int octeontx_pko_dq_open(uint16_t txq)
{
	unsigned int reg_off;
	uint8_t *vf_bar0;
	uint64_t rtn;
	int gdq;

	vf_bar0 = octeontx_pko_dq_vf_bar0(txq);
	gdq = octeontx_pko_dq_gdq(txq);

	if (unlikely(gdq < 0 || vf_bar0 == NULL))
		return -EINVAL;
	*(volatile int64_t*)(pko_vf_ctl.fc_ctl + txq) =
		PKO_DQ_FC_DEPTH_PAGES - PKO_DQ_FC_SKID;

	rte_wmb();

	octeontx_write64(PKO_DQ_FC_DEPTH_PAGES,
			 vf_bar0 + PKO_VF_DQ_FC_STATUS(gdq));

	/* Set the register to return descriptor (packet) count as DEPTH */
	/* KIND=1, NCB_QUERY_RSP=0 */
	octeontx_write64(1ull << PKO_DQ_KIND_BIT,
				vf_bar0 + PKO_VF_DQ_WM_CTL(gdq));
	reg_off = PKO_VF_DQ_OP_OPEN(gdq);

	rtn = octeontx_reg_ldadd_u64(vf_bar0 + reg_off, 0);

	/* PKO_DQOP_E::OPEN */
	if (((rtn >> PKO_DQ_OP_BIT) & 0x3) != 0x1)
		return -EIO;

	switch (rtn >> PKO_DQ_STATUS_BIT) {
	case 0xC:	/* DQALREADYCREATED */
	case 0x0:	/* PASS */
		break;
	default:
		return -EIO;
	}

	/* DRAIN=0, DRAIN_NULL_LINK=0, SW_XOFF=0 */
	octeontx_write64(0, vf_bar0 + PKO_VF_DQ_SW_XOFF(gdq));

	return rtn & ((1ull << PKO_DQ_OP_BIT) - 1);
}

/**
 * Close a PKO DQ
 * Flush all packets pending.
 */
static inline
int octeontx_pko_dq_close(uint16_t txq)
{
	unsigned int reg_off;
	uint8_t *vf_bar0;
	uint64_t rtn;
	int res;

	vf_bar0 = octeontx_pko_dq_vf_bar0(txq);
	res = octeontx_pko_dq_gdq(txq);

	if (unlikely(res < 0 || vf_bar0 == NULL))
		return -EINVAL;

	reg_off = PKO_VF_DQ_OP_CLOSE(res);

	rtn = octeontx_reg_ldadd_u64(vf_bar0 + reg_off, 0);

	/* PKO_DQOP_E::CLOSE */
	if (((rtn >> PKO_DQ_OP_BIT) & 0x3) != 0x2)
		return -EIO;

	switch (rtn >> PKO_DQ_STATUS_BIT) {
	case 0xD:	/* DQNOTCREATED */
	case 0x0:	/* PASS */
		break;
	default:
		return -EIO;
	}

	res = rtn & ((1ull << PKO_DQ_OP_BIT) - 1); /* DEPTH */
	return res;
}

/* Flush all packets pending on a DQ */
static inline
int octeontx_pko_dq_drain(uint16_t txq)
{
	unsigned int gdq;
	uint8_t *vf_bar0;
	uint64_t reg;
	int res, timo = PKO_DQ_DRAIN_TO;

	vf_bar0 = octeontx_pko_dq_vf_bar0(txq);
	res = octeontx_pko_dq_gdq(txq);
	gdq = res;

	 /* DRAIN=1, DRAIN_NULL_LINK=0, SW_XOFF=1 */
	 octeontx_write64(0x3, vf_bar0 + PKO_VF_DQ_SW_XOFF(gdq));
	/* Wait until buffers leave DQs */
	reg = octeontx_read64(vf_bar0 + PKO_VF_DQ_WM_CNT(gdq));
	while (reg && timo > 0) {
		rte_delay_us(100);
		timo--;
		reg = octeontx_read64(vf_bar0 + PKO_VF_DQ_WM_CNT(gdq));
	}
	/* DRAIN=0, DRAIN_NULL_LINK=0, SW_XOFF=0 */
	octeontx_write64(0, vf_bar0 + PKO_VF_DQ_SW_XOFF(gdq));

	return reg;
}

static inline int
octeontx_pko_dq_range_lookup(struct octeontx_pko_vf_ctl_s *ctl, uint64_t chanid,
			     unsigned int dq_num, unsigned int dq_from)
{
	unsigned int dq, dq_cnt;
	unsigned int dq_base;

	dq_cnt = 0;
	dq = dq_from;
	while (dq < RTE_DIM(ctl->dq_map)) {
		dq_base = dq;
		dq_cnt = 0;
		while (ctl->dq_map[dq].chanid == ~chanid &&
			dq < RTE_DIM(ctl->dq_map)) {
			dq_cnt++;
			if (dq_cnt == dq_num)
				return dq_base;
			dq++;
		}
		dq++;
	}
	return -1;
}

static inline void
octeontx_pko_dq_range_assign(struct octeontx_pko_vf_ctl_s *ctl, uint64_t chanid,
			     unsigned int dq_base, unsigned int dq_num)
{
	unsigned int dq, dq_cnt;

	dq_cnt = 0;
	while (dq_cnt < dq_num) {
		dq = dq_base + dq_cnt;

		octeontx_log_dbg("DQ# %u assigned to CHAN# %" PRIx64 "", dq,
			chanid);

		ctl->dq_map[dq].chanid = ~chanid;
		dq_cnt++;
	}
}

static inline int
octeontx_pko_dq_claim(struct octeontx_pko_vf_ctl_s *ctl, unsigned int dq_base,
		      unsigned int dq_num, uint64_t chanid)
{
	const uint64_t null_chanid = ~0ull;
	int dq;

	rte_spinlock_lock(&ctl->lock);

	dq = octeontx_pko_dq_range_lookup(ctl, null_chanid, dq_num, dq_base);
	if (dq < 0 || (unsigned int)dq != dq_base) {
		rte_spinlock_unlock(&ctl->lock);
		return -1;
	}
	octeontx_pko_dq_range_assign(ctl, chanid, dq_base, dq_num);

	rte_spinlock_unlock(&ctl->lock);

	return 0;
}

static inline int
octeontx_pko_dq_free(struct octeontx_pko_vf_ctl_s *ctl, uint64_t chanid)
{
	const uint64_t null_chanid = ~0ull;
	unsigned int dq = 0, dq_cnt = 0;

	rte_spinlock_lock(&ctl->lock);
	while (dq < RTE_DIM(ctl->dq_map)) {
		if (ctl->dq_map[dq].chanid == ~chanid) {
			ctl->dq_map[dq].chanid = ~null_chanid;
			dq_cnt++;
		}
		dq++;
	}
	rte_spinlock_unlock(&ctl->lock);

	return dq_cnt > 0 ? 0 : -EINVAL;
}

int
octeontx_pko_channel_open(int dq_base, int dq_num, int chanid)
{
	struct octeontx_pko_vf_ctl_s *ctl = &pko_vf_ctl;
	int res;

	res = octeontx_pko_dq_claim(ctl, dq_base, dq_num, chanid);
	if (res < 0)
		return -1;

	return 0;
}

int
octeontx_pko_channel_close(int chanid)
{
	struct octeontx_pko_vf_ctl_s *ctl = &pko_vf_ctl;
	int res;

	res = octeontx_pko_dq_free(ctl, chanid);
	if (res < 0)
		return -1;

	return 0;
}

static inline int
octeontx_pko_chan_start(struct octeontx_pko_vf_ctl_s *ctl, uint64_t chanid)
{
	unsigned int dq_vf;
	unsigned int dq, dq_cnt;

	dq_cnt = 0;
	dq = 0;
	while (dq < RTE_DIM(ctl->dq_map)) {
		dq_vf = dq / PKO_VF_NUM_DQ;

		if (!ctl->pko[dq_vf].bar0) {
			dq += PKO_VF_NUM_DQ;
			continue;
		}

		if (ctl->dq_map[dq].chanid != ~chanid) {
			dq++;
			continue;
		}

		if (octeontx_pko_dq_open(dq) < 0)
			break;

		dq_cnt++;
		dq++;
	}

	return dq_cnt;
}

int
octeontx_pko_channel_start(int chanid)
{
	struct octeontx_pko_vf_ctl_s *ctl = &pko_vf_ctl;
	int dq_cnt;

	dq_cnt = octeontx_pko_chan_start(ctl, chanid);
	if (dq_cnt < 0)
		return -1;

	return dq_cnt;
}

static inline int
octeontx_pko_chan_stop(struct octeontx_pko_vf_ctl_s *ctl, uint64_t chanid)
{
	unsigned int dq, dq_cnt, dq_vf;
	int res;

	dq_cnt = 0;
	dq = 0;
	while (dq < RTE_DIM(ctl->dq_map)) {
		dq_vf = dq / PKO_VF_NUM_DQ;

		if (!ctl->pko[dq_vf].bar0) {
			dq += PKO_VF_NUM_DQ;
			continue;
		}

		if (ctl->dq_map[dq].chanid != ~chanid) {
			dq++;
			continue;
		}

		res = octeontx_pko_dq_drain(dq);
		if (res > 0)
			octeontx_log_err("draining DQ%d, buffers left: %x",
					 dq, res);

		res = octeontx_pko_dq_close(dq);
		if (res < 0)
			octeontx_log_err("closing DQ%d failed\n", dq);

		dq_cnt++;
		dq++;
	}
	return dq_cnt;
}

int
octeontx_pko_channel_stop(int chanid)
{
	struct octeontx_pko_vf_ctl_s *ctl = &pko_vf_ctl;

	octeontx_pko_chan_stop(ctl, chanid);
	return 0;
}

static void
octeontx_pkovf_setup(void)
{
	static bool init_once;

	if (!init_once) {
		unsigned int i;

		rte_spinlock_init(&pko_vf_ctl.lock);

		pko_vf_ctl.fc_iomem = PKO_IOMEM_NULL;
		pko_vf_ctl.fc_ctl = NULL;

		for (i = 0; i < PKO_VF_MAX; i++) {
			pko_vf_ctl.pko[i].bar0 = NULL;
			pko_vf_ctl.pko[i].bar2 = NULL;
			pko_vf_ctl.pko[i].domain = ~(uint16_t)0;
			pko_vf_ctl.pko[i].vfid = ~(uint16_t)0;
		}

		for (i = 0; i < (PKO_VF_MAX * PKO_VF_NUM_DQ); i++)
			pko_vf_ctl.dq_map[i].chanid = 0;

		init_once = true;
	}
}

/* PKOVF pcie device*/
static int
pkovf_probe(struct rte_pci_driver *pci_drv, struct rte_pci_device *pci_dev)
{
	uint64_t val;
	uint16_t vfid;
	uint16_t domain;
	uint8_t *bar0;
	uint8_t *bar2;
	struct octeontx_pkovf *res;

	RTE_SET_USED(pci_drv);

	/* For secondary processes, the primary has done all the work */
	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return 0;

	if (pci_dev->mem_resource[0].addr == NULL ||
	    pci_dev->mem_resource[2].addr == NULL) {
		octeontx_log_err("Empty bars %p %p",
			pci_dev->mem_resource[0].addr,
			pci_dev->mem_resource[2].addr);
		return -ENODEV;
	}
	bar0 = pci_dev->mem_resource[0].addr;
	bar2 = pci_dev->mem_resource[2].addr;

	octeontx_pkovf_setup();

	/* get vfid and domain */
	val = octeontx_read64(bar0 + PKO_VF_DQ_FC_CONFIG);
	domain = (val >> 7) & 0xffff;
	vfid = (val >> 23) & 0xffff;

	if (unlikely(vfid >= PKO_VF_MAX)) {
		octeontx_log_err("pko: Invalid vfid %d", vfid);
		return -EINVAL;
	}

	res = &pko_vf_ctl.pko[vfid];
	res->vfid = vfid;
	res->domain = domain;
	res->bar0 = bar0;
	res->bar2 = bar2;

	octeontx_log_dbg("Domain=%d group=%d", res->domain, res->vfid);
	return 0;
}

#define PCI_VENDOR_ID_CAVIUM               0x177D
#define PCI_DEVICE_ID_OCTEONTX_PKO_VF      0xA049

static const struct rte_pci_id pci_pkovf_map[] = {
	{
		RTE_PCI_DEVICE(PCI_VENDOR_ID_CAVIUM,
				PCI_DEVICE_ID_OCTEONTX_PKO_VF)
	},
	{
		.vendor_id = 0,
	},
};

static struct rte_pci_driver pci_pkovf = {
	.id_table = pci_pkovf_map,
	.drv_flags = RTE_PCI_DRV_NEED_MAPPING,
	.probe = pkovf_probe,
};

RTE_PMD_REGISTER_PCI(octeontx_pkovf, pci_pkovf);
