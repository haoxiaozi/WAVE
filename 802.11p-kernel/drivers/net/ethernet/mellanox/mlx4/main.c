/*
 * Copyright (c) 2004, 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Sun Microsystems, Inc. All rights reserved.
 * Copyright (c) 2005, 2006, 2007, 2008 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2006, 2007 Cisco Systems, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/io-mapping.h>
#include <linux/delay.h>
#include <linux/kmod.h>

#include <linux/mlx4/device.h>
#include <linux/mlx4/doorbell.h>

#include "mlx4.h"
#include "fw.h"
#include "icm.h"

MODULE_AUTHOR("Roland Dreier");
MODULE_DESCRIPTION("Mellanox ConnectX HCA low-level driver");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION(DRV_VERSION);

struct workqueue_struct *mlx4_wq;

#ifdef CONFIG_MLX4_DEBUG

int mlx4_debug_level = 0;
module_param_named(debug_level, mlx4_debug_level, int, 0644);
MODULE_PARM_DESC(debug_level, "Enable debug tracing if > 0");

#endif /* CONFIG_MLX4_DEBUG */

#ifdef CONFIG_PCI_MSI

static int msi_x = 1;
module_param(msi_x, int, 0444);
MODULE_PARM_DESC(msi_x, "attempt to use MSI-X if nonzero");

#else /* CONFIG_PCI_MSI */

#define msi_x (0)

#endif /* CONFIG_PCI_MSI */

static uint8_t num_vfs[3] = {0, 0, 0};
static int num_vfs_argc;
module_param_array(num_vfs, byte , &num_vfs_argc, 0444);
MODULE_PARM_DESC(num_vfs, "enable #num_vfs functions if num_vfs > 0\n"
			  "num_vfs=port1,port2,port1+2");

static uint8_t probe_vf[3] = {0, 0, 0};
static int probe_vfs_argc;
module_param_array(probe_vf, byte, &probe_vfs_argc, 0444);
MODULE_PARM_DESC(probe_vf, "number of vfs to probe by pf driver (num_vfs > 0)\n"
			   "probe_vf=port1,port2,port1+2");

int mlx4_log_num_mgm_entry_size = MLX4_DEFAULT_MGM_LOG_ENTRY_SIZE;
module_param_named(log_num_mgm_entry_size,
			mlx4_log_num_mgm_entry_size, int, 0444);
MODULE_PARM_DESC(log_num_mgm_entry_size, "log mgm size, that defines the num"
					 " of qp per mcg, for example:"
					 " 10 gives 248.range: 7 <="
					 " log_num_mgm_entry_size <= 12."
					 " To activate device managed"
					 " flow steering when available, set to -1");

static bool enable_64b_cqe_eqe = true;
module_param(enable_64b_cqe_eqe, bool, 0444);
MODULE_PARM_DESC(enable_64b_cqe_eqe,
		 "Enable 64 byte CQEs/EQEs when the FW supports this (default: True)");

#define PF_CONTEXT_BEHAVIOUR_MASK	(MLX4_FUNC_CAP_64B_EQE_CQE | \
					 MLX4_FUNC_CAP_EQE_CQE_STRIDE | \
					 MLX4_FUNC_CAP_DMFS_A0_STATIC)

static char mlx4_version[] =
	DRV_NAME ": Mellanox ConnectX core driver v"
	DRV_VERSION " (" DRV_RELDATE ")\n";

static struct mlx4_profile default_profile = {
	.num_qp		= 1 << 18,
	.num_srq	= 1 << 16,
	.rdmarc_per_qp	= 1 << 4,
	.num_cq		= 1 << 16,
	.num_mcg	= 1 << 13,
	.num_mpt	= 1 << 19,
	.num_mtt	= 1 << 20, /* It is really num mtt segements */
};

static struct mlx4_profile low_mem_profile = {
	.num_qp		= 1 << 17,
	.num_srq	= 1 << 6,
	.rdmarc_per_qp	= 1 << 4,
	.num_cq		= 1 << 8,
	.num_mcg	= 1 << 8,
	.num_mpt	= 1 << 9,
	.num_mtt	= 1 << 7,
};

static int log_num_mac = 7;
module_param_named(log_num_mac, log_num_mac, int, 0444);
MODULE_PARM_DESC(log_num_mac, "Log2 max number of MACs per ETH port (1-7)");

static int log_num_vlan;
module_param_named(log_num_vlan, log_num_vlan, int, 0444);
MODULE_PARM_DESC(log_num_vlan, "Log2 max number of VLANs per ETH port (0-7)");
/* Log2 max number of VLANs per ETH port (0-7) */
#define MLX4_LOG_NUM_VLANS 7
#define MLX4_MIN_LOG_NUM_VLANS 0
#define MLX4_MIN_LOG_NUM_MAC 1

static bool use_prio;
module_param_named(use_prio, use_prio, bool, 0444);
MODULE_PARM_DESC(use_prio, "Enable steering by VLAN priority on ETH ports (deprecated)");

int log_mtts_per_seg = ilog2(MLX4_MTT_ENTRY_PER_SEG);
module_param_named(log_mtts_per_seg, log_mtts_per_seg, int, 0444);
MODULE_PARM_DESC(log_mtts_per_seg, "Log2 number of MTT entries per segment (1-7)");

static int port_type_array[2] = {MLX4_PORT_TYPE_NONE, MLX4_PORT_TYPE_NONE};
static int arr_argc = 2;
module_param_array(port_type_array, int, &arr_argc, 0444);
MODULE_PARM_DESC(port_type_array, "Array of port types: HW_DEFAULT (0) is default "
				"1 for IB, 2 for Ethernet");

struct mlx4_port_config {
	struct list_head list;
	enum mlx4_port_type port_type[MLX4_MAX_PORTS + 1];
	struct pci_dev *pdev;
};

static atomic_t pf_loading = ATOMIC_INIT(0);

int mlx4_check_port_params(struct mlx4_dev *dev,
			   enum mlx4_port_type *port_type)
{
	int i;

	for (i = 0; i < dev->caps.num_ports - 1; i++) {
		if (port_type[i] != port_type[i + 1]) {
			if (!(dev->caps.flags & MLX4_DEV_CAP_FLAG_DPDP)) {
				mlx4_err(dev, "Only same port types supported on this HCA, aborting\n");
				return -EINVAL;
			}
		}
	}

	for (i = 0; i < dev->caps.num_ports; i++) {
		if (!(port_type[i] & dev->caps.supported_type[i+1])) {
			mlx4_err(dev, "Requested port type for port %d is not supported on this HCA\n",
				 i + 1);
			return -EINVAL;
		}
	}
	return 0;
}

static void mlx4_set_port_mask(struct mlx4_dev *dev)
{
	int i;

	for (i = 1; i <= dev->caps.num_ports; ++i)
		dev->caps.port_mask[i] = dev->caps.port_type[i];
}

enum {
	MLX4_QUERY_FUNC_NUM_SYS_EQS = 1 << 0,
};

static int mlx4_query_func(struct mlx4_dev *dev, struct mlx4_dev_cap *dev_cap)
{
	int err = 0;
	struct mlx4_func func;

	if (dev->caps.flags2 & MLX4_DEV_CAP_FLAG2_SYS_EQS) {
		err = mlx4_QUERY_FUNC(dev, &func, 0);
		if (err) {
			mlx4_err(dev, "QUERY_DEV_CAP command failed, aborting.\n");
			return err;
		}
		dev_cap->max_eqs = func.max_eq;
		dev_cap->reserved_eqs = func.rsvd_eqs;
		dev_cap->reserved_uars = func.rsvd_uars;
		err |= MLX4_QUERY_FUNC_NUM_SYS_EQS;
	}
	return err;
}

static void mlx4_enable_cqe_eqe_stride(struct mlx4_dev *dev)
{
	struct mlx4_caps *dev_cap = &dev->caps;

	/* FW not supporting or cancelled by user */
	if (!(dev_cap->flags2 & MLX4_DEV_CAP_FLAG2_EQE_STRIDE) ||
	    !(dev_cap->flags2 & MLX4_DEV_CAP_FLAG2_CQE_STRIDE))
		return;

	/* Must have 64B CQE_EQE enabled by FW to use bigger stride
	 * When FW has NCSI it may decide not to report 64B CQE/EQEs
	 */
	if (!(dev_cap->flags & MLX4_DEV_CAP_FLAG_64B_EQE) ||
	    !(dev_cap->flags & MLX4_DEV_CAP_FLAG_64B_CQE)) {
		dev_cap->flags2 &= ~MLX4_DEV_CAP_FLAG2_CQE_STRIDE;
		dev_cap->flags2 &= ~MLX4_DEV_CAP_FLAG2_EQE_STRIDE;
		return;
	}

	if (cache_line_size() == 128 || cache_line_size() == 256) {
		mlx4_dbg(dev, "Enabling CQE stride cacheLine supported\n");
		/* Changing the real data inside CQE size to 32B */
		dev_cap->flags &= ~MLX4_DEV_CAP_FLAG_64B_CQE;
		dev_cap->flags &= ~MLX4_DEV_CAP_FLAG_64B_EQE;

		if (mlx4_is_master(dev))
			dev_cap->function_caps |= MLX4_FUNC_CAP_EQE_CQE_STRIDE;
	} else {
		mlx4_dbg(dev, "Disabling CQE stride cacheLine unsupported\n");
		dev_cap->flags2 &= ~MLX4_DEV_CAP_FLAG2_CQE_STRIDE;
		dev_cap->flags2 &= ~MLX4_DEV_CAP_FLAG2_EQE_STRIDE;
	}
}

static int _mlx4_dev_port(struct mlx4_dev *dev, int port,
			  struct mlx4_port_cap *port_cap)
{
	dev->caps.vl_cap[port]	    = port_cap->max_vl;
	dev->caps.ib_mtu_cap[port]	    = port_cap->ib_mtu;
	dev->phys_caps.gid_phys_table_len[port]  = port_cap->max_gids;
	dev->phys_caps.pkey_phys_table_len[port] = port_cap->max_pkeys;
	/* set gid and pkey table operating lengths by default
	 * to non-sriov values
	 */
	dev->caps.gid_table_len[port]  = port_cap->max_gids;
	dev->caps.pkey_table_len[port] = port_cap->max_pkeys;
	dev->caps.port_width_cap[port] = port_cap->max_port_width;
	dev->caps.eth_mtu_cap[port]    = port_cap->eth_mtu;
	dev->caps.def_mac[port]        = port_cap->def_mac;
	dev->caps.supported_type[port] = port_cap->supported_port_types;
	dev->caps.suggested_type[port] = port_cap->suggested_type;
	dev->caps.default_sense[port] = port_cap->default_sense;
	dev->caps.trans_type[port]	    = port_cap->trans_type;
	dev->caps.vendor_oui[port]     = port_cap->vendor_oui;
	dev->caps.wavelength[port]     = port_cap->wavelength;
	dev->caps.trans_code[port]     = port_cap->trans_code;

	return 0;
}

static int mlx4_dev_port(struct mlx4_dev *dev, int port,
			 struct mlx4_port_cap *port_cap)
{
	int err = 0;

	err = mlx4_QUERY_PORT(dev, port, port_cap);

	if (err)
		mlx4_err(dev, "QUERY_PORT command failed.\n");

	return err;
}

#define MLX4_A0_STEERING_TABLE_SIZE	256
static int mlx4_dev_cap(struct mlx4_dev *dev, struct mlx4_dev_cap *dev_cap)
{
	int err;
	int i;

	err = mlx4_QUERY_DEV_CAP(dev, dev_cap);
	if (err) {
		mlx4_err(dev, "QUERY_DEV_CAP command failed, aborting\n");
		return err;
	}

	if (dev_cap->min_page_sz > PAGE_SIZE) {
		mlx4_err(dev, "HCA minimum page size of %d bigger than kernel PAGE_SIZE of %ld, aborting\n",
			 dev_cap->min_page_sz, PAGE_SIZE);
		return -ENODEV;
	}
	if (dev_cap->num_ports > MLX4_MAX_PORTS) {
		mlx4_err(dev, "HCA has %d ports, but we only support %d, aborting\n",
			 dev_cap->num_ports, MLX4_MAX_PORTS);
		return -ENODEV;
	}

	if (dev_cap->uar_size > pci_resource_len(dev->pdev, 2)) {
		mlx4_err(dev, "HCA reported UAR size of 0x%x bigger than PCI resource 2 size of 0x%llx, aborting\n",
			 dev_cap->uar_size,
			 (unsigned long long) pci_resource_len(dev->pdev, 2));
		return -ENODEV;
	}

	dev->caps.num_ports	     = dev_cap->num_ports;
	dev->caps.num_sys_eqs = dev_cap->num_sys_eqs;
	dev->phys_caps.num_phys_eqs = dev_cap->flags2 & MLX4_DEV_CAP_FLAG2_SYS_EQS ?
				      dev->caps.num_sys_eqs :
				      MLX4_MAX_EQ_NUM;
	for (i = 1; i <= dev->caps.num_ports; ++i) {
		err = _mlx4_dev_port(dev, i, dev_cap->port_cap + i);
		if (err) {
			mlx4_err(dev, "QUERY_PORT command failed, aborting\n");
			return err;
		}
	}

	dev->caps.uar_page_size	     = PAGE_SIZE;
	dev->caps.num_uars	     = dev_cap->uar_size / PAGE_SIZE;
	dev->caps.local_ca_ack_delay = dev_cap->local_ca_ack_delay;
	dev->caps.bf_reg_size	     = dev_cap->bf_reg_size;
	dev->caps.bf_regs_per_page   = dev_cap->bf_regs_per_page;
	dev->caps.max_sq_sg	     = dev_cap->max_sq_sg;
	dev->caps.max_rq_sg	     = dev_cap->max_rq_sg;
	dev->caps.max_wqes	     = dev_cap->max_qp_sz;
	dev->caps.max_qp_init_rdma   = dev_cap->max_requester_per_qp;
	dev->caps.max_srq_wqes	     = dev_cap->max_srq_sz;
	dev->caps.max_srq_sge	     = dev_cap->max_rq_sg - 1;
	dev->caps.reserved_srqs	     = dev_cap->reserved_srqs;
	dev->caps.max_sq_desc_sz     = dev_cap->max_sq_desc_sz;
	dev->caps.max_rq_desc_sz     = dev_cap->max_rq_desc_sz;
	/*
	 * Subtract 1 from the limit because we need to allocate a
	 * spare CQE so the HCA HW can tell the difference between an
	 * empty CQ and a full CQ.
	 */
	dev->caps.max_cqes	     = dev_cap->max_cq_sz - 1;
	dev->caps.reserved_cqs	     = dev_cap->reserved_cqs;
	dev->caps.reserved_eqs	     = dev_cap->reserved_eqs;
	dev->caps.reserved_mtts      = dev_cap->reserved_mtts;
	dev->caps.reserved_mrws	     = dev_cap->reserved_mrws;

	/* The first 128 UARs are used for EQ doorbells */
	dev->caps.reserved_uars	     = max_t(int, 128, dev_cap->reserved_uars);
	dev->caps.reserved_pds	     = dev_cap->reserved_pds;
	dev->caps.reserved_xrcds     = (dev->caps.flags & MLX4_DEV_CAP_FLAG_XRC) ?
					dev_cap->reserved_xrcds : 0;
	dev->caps.max_xrcds          = (dev->caps.flags & MLX4_DEV_CAP_FLAG_XRC) ?
					dev_cap->max_xrcds : 0;
	dev->caps.mtt_entry_sz       = dev_cap->mtt_entry_sz;

	dev->caps.max_msg_sz         = dev_cap->max_msg_sz;
	dev->caps.page_size_cap	     = ~(u32) (dev_cap->min_page_sz - 1);
	dev->caps.flags		     = dev_cap->flags;
	dev->caps.flags2	     = dev_cap->flags2;
	dev->caps.bmme_flags	     = dev_cap->bmme_flags;
	dev->caps.reserved_lkey	     = dev_cap->reserved_lkey;
	dev->caps.stat_rate_support  = dev_cap->stat_rate_support;
	dev->caps.max_gso_sz	     = dev_cap->max_gso_sz;
	dev->caps.max_rss_tbl_sz     = dev_cap->max_rss_tbl_sz;

	/* Sense port always allowed on supported devices for ConnectX-1 and -2 */
	if (mlx4_priv(dev)->pci_dev_data & MLX4_PCI_DEV_FORCE_SENSE_PORT)
		dev->caps.flags |= MLX4_DEV_CAP_FLAG_SENSE_SUPPORT;
	/* Don't do sense port on multifunction devices (for now at least) */
	if (mlx4_is_mfunc(dev))
		dev->caps.flags &= ~MLX4_DEV_CAP_FLAG_SENSE_SUPPORT;

	if (mlx4_low_memory_profile()) {
		dev->caps.log_num_macs  = MLX4_MIN_LOG_NUM_MAC;
		dev->caps.log_num_vlans = MLX4_MIN_LOG_NUM_VLANS;
	} else {
		dev->caps.log_num_macs  = log_num_mac;
		dev->caps.log_num_vlans = MLX4_LOG_NUM_VLANS;
	}

	for (i = 1; i <= dev->caps.num_ports; ++i) {
		dev->caps.port_type[i] = MLX4_PORT_TYPE_NONE;
		if (dev->caps.supported_type[i]) {
			/* if only ETH is supported - assign ETH */
			if (dev->caps.supported_type[i] == MLX4_PORT_TYPE_ETH)
				dev->caps.port_type[i] = MLX4_PORT_TYPE_ETH;
			/* if only IB is supported, assign IB */
			else if (dev->caps.supported_type[i] ==
				 MLX4_PORT_TYPE_IB)
				dev->caps.port_type[i] = MLX4_PORT_TYPE_IB;
			else {
				/* if IB and ETH are supported, we set the port
				 * type according to user selection of port type;
				 * if user selected none, take the FW hint */
				if (port_type_array[i - 1] == MLX4_PORT_TYPE_NONE)
					dev->caps.port_type[i] = dev->caps.suggested_type[i] ?
						MLX4_PORT_TYPE_ETH : MLX4_PORT_TYPE_IB;
				else
					dev->caps.port_type[i] = port_type_array[i - 1];
			}
		}
		/*
		 * Link sensing is allowed on the port if 3 conditions are true:
		 * 1. Both protocols are supported on the port.
		 * 2. Different types are supported on the port
		 * 3. FW declared that it supports link sensing
		 */
		mlx4_priv(dev)->sense.sense_allowed[i] =
			((dev->caps.supported_type[i] == MLX4_PORT_TYPE_AUTO) &&
			 (dev->caps.flags & MLX4_DEV_CAP_FLAG_DPDP) &&
			 (dev->caps.flags & MLX4_DEV_CAP_FLAG_SENSE_SUPPORT));

		/*
		 * If "default_sense" bit is set, we move the port to "AUTO" mode
		 * and perform sense_port FW command to try and set the correct
		 * port type from beginning
		 */
		if (mlx4_priv(dev)->sense.sense_allowed[i] && dev->caps.default_sense[i]) {
			enum mlx4_port_type sensed_port = MLX4_PORT_TYPE_NONE;
			dev->caps.possible_type[i] = MLX4_PORT_TYPE_AUTO;
			mlx4_SENSE_PORT(dev, i, &sensed_port);
			if (sensed_port != MLX4_PORT_TYPE_NONE)
				dev->caps.port_type[i] = sensed_port;
		} else {
			dev->caps.possible_type[i] = dev->caps.port_type[i];
		}

		if (dev->caps.log_num_macs > dev_cap->port_cap[i].log_max_macs) {
			dev->caps.log_num_macs = dev_cap->port_cap[i].log_max_macs;
			mlx4_warn(dev, "Requested number of MACs is too much for port %d, reducing to %d\n",
				  i, 1 << dev->caps.log_num_macs);
		}
		if (dev->caps.log_num_vlans > dev_cap->port_cap[i].log_max_vlans) {
			dev->caps.log_num_vlans = dev_cap->port_cap[i].log_max_vlans;
			mlx4_warn(dev, "Requested number of VLANs is too much for port %d, reducing to %d\n",
				  i, 1 << dev->caps.log_num_vlans);
		}
	}

	dev->caps.max_counters = 1 << ilog2(dev_cap->max_counters);

	dev->caps.reserved_qps_cnt[MLX4_QP_REGION_FW] = dev_cap->reserved_qps;
	dev->caps.reserved_qps_cnt[MLX4_QP_REGION_ETH_ADDR] =
		dev->caps.reserved_qps_cnt[MLX4_QP_REGION_FC_ADDR] =
		(1 << dev->caps.log_num_macs) *
		(1 << dev->caps.log_num_vlans) *
		dev->caps.num_ports;
	dev->caps.reserved_qps_cnt[MLX4_QP_REGION_FC_EXCH] = MLX4_NUM_FEXCH;

	if (dev_cap->dmfs_high_rate_qpn_base > 0 &&
	    dev->caps.flags2 & MLX4_DEV_CAP_FLAG2_FS_EN)
		dev->caps.dmfs_high_rate_qpn_base = dev_cap->dmfs_high_rate_qpn_base;
	else
		dev->caps.dmfs_high_rate_qpn_base =
			dev->caps.reserved_qps_cnt[MLX4_QP_REGION_FW];

	if (dev_cap->dmfs_high_rate_qpn_range > 0 &&
	    dev->caps.flags2 & MLX4_DEV_CAP_FLAG2_FS_EN) {
		dev->caps.dmfs_high_rate_qpn_range = dev_cap->dmfs_high_rate_qpn_range;
		dev->caps.dmfs_high_steer_mode = MLX4_STEERING_DMFS_A0_DEFAULT;
		dev->caps.flags2 |= MLX4_DEV_CAP_FLAG2_FS_A0;
	} else {
		dev->caps.dmfs_high_steer_mode = MLX4_STEERING_DMFS_A0_NOT_SUPPORTED;
		dev->caps.dmfs_high_rate_qpn_base =
			dev->caps.reserved_qps_cnt[MLX4_QP_REGION_FW];
		dev->caps.dmfs_high_rate_qpn_range = MLX4_A0_STEERING_TABLE_SIZE;
	}

	dev->caps.reserved_qps_cnt[MLX4_QP_REGION_RSS_RAW_ETH] =
		dev->caps.dmfs_high_rate_qpn_range;

	dev->caps.reserved_qps = dev->caps.reserved_qps_cnt[MLX4_QP_REGION_FW] +
		dev->caps.reserved_qps_cnt[MLX4_QP_REGION_ETH_ADDR] +
		dev->caps.reserved_qps_cnt[MLX4_QP_REGION_FC_ADDR] +
		dev->caps.reserved_qps_cnt[MLX4_QP_REGION_FC_EXCH];

	dev->caps.sqp_demux = (mlx4_is_master(dev)) ? MLX4_MAX_NUM_SLAVES : 0;

	if (!enable_64b_cqe_eqe && !mlx4_is_slave(dev)) {
		if (dev_cap->flags &
		    (MLX4_DEV_CAP_FLAG_64B_CQE | MLX4_DEV_CAP_FLAG_64B_EQE)) {
			mlx4_warn(dev, "64B EQEs/CQEs supported by the device but not enabled\n");
			dev->caps.flags &= ~MLX4_DEV_CAP_FLAG_64B_CQE;
			dev->caps.flags &= ~MLX4_DEV_CAP_FLAG_64B_EQE;
		}

		if (dev_cap->flags2 &
		    (MLX4_DEV_CAP_FLAG2_CQE_STRIDE |
		     MLX4_DEV_CAP_FLAG2_EQE_STRIDE)) {
			mlx4_warn(dev, "Disabling EQE/CQE stride per user request\n");
			dev_cap->flags2 &= ~MLX4_DEV_CAP_FLAG2_CQE_STRIDE;
			dev_cap->flags2 &= ~MLX4_DEV_CAP_FLAG2_EQE_STRIDE;
		}
	}

	if ((dev->caps.flags &
	    (MLX4_DEV_CAP_FLAG_64B_CQE | MLX4_DEV_CAP_FLAG_64B_EQE)) &&
	    mlx4_is_master(dev))
		dev->caps.function_caps |= MLX4_FUNC_CAP_64B_EQE_CQE;

	if (!mlx4_is_slave(dev)) {
		mlx4_enable_cqe_eqe_stride(dev);
		dev->caps.alloc_res_qp_mask =
			(dev->caps.bf_reg_size ? MLX4_RESERVE_ETH_BF_QP : 0) |
			MLX4_RESERVE_A0_QP;
	} else {
		dev->caps.alloc_res_qp_mask = 0;
	}

	return 0;
}

static int mlx4_get_pcie_dev_link_caps(struct mlx4_dev *dev,
				       enum pci_bus_speed *speed,
				       enum pcie_link_width *width)
{
	u32 lnkcap1, lnkcap2;
	int err1, err2;

#define  PCIE_MLW_CAP_SHIFT 4	/* start of MLW mask in link capabilities */

	*speed = PCI_SPEED_UNKNOWN;
	*width = PCIE_LNK_WIDTH_UNKNOWN;

	err1 = pcie_capability_read_dword(dev->pdev, PCI_EXP_LNKCAP, &lnkcap1);
	err2 = pcie_capability_read_dword(dev->pdev, PCI_EXP_LNKCAP2, &lnkcap2);
	if (!err2 && lnkcap2) { /* PCIe r3.0-compliant */
		if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_8_0GB)
			*speed = PCIE_SPEED_8_0GT;
		else if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_5_0GB)
			*speed = PCIE_SPEED_5_0GT;
		else if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_2_5GB)
			*speed = PCIE_SPEED_2_5GT;
	}
	if (!err1) {
		*width = (lnkcap1 & PCI_EXP_LNKCAP_MLW) >> PCIE_MLW_CAP_SHIFT;
		if (!lnkcap2) { /* pre-r3.0 */
			if (lnkcap1 & PCI_EXP_LNKCAP_SLS_5_0GB)
				*speed = PCIE_SPEED_5_0GT;
			else if (lnkcap1 & PCI_EXP_LNKCAP_SLS_2_5GB)
				*speed = PCIE_SPEED_2_5GT;
		}
	}

	if (*speed == PCI_SPEED_UNKNOWN || *width == PCIE_LNK_WIDTH_UNKNOWN) {
		return err1 ? err1 :
			err2 ? err2 : -EINVAL;
	}
	return 0;
}

static void mlx4_check_pcie_caps(struct mlx4_dev *dev)
{
	enum pcie_link_width width, width_cap;
	enum pci_bus_speed speed, speed_cap;
	int err;

#define PCIE_SPEED_STR(speed) \
	(speed == PCIE_SPEED_8_0GT ? "8.0GT/s" : \
	 speed == PCIE_SPEED_5_0GT ? "5.0GT/s" : \
	 speed == PCIE_SPEED_2_5GT ? "2.5GT/s" : \
	 "Unknown")

	err = mlx4_get_pcie_dev_link_caps(dev, &speed_cap, &width_cap);
	if (err) {
		mlx4_warn(dev,
			  "Unable to determine PCIe device BW capabilities\n");
		return;
	}

	err = pcie_get_minimum_link(dev->pdev, &speed, &width);
	if (err || speed == PCI_SPEED_UNKNOWN ||
	    width == PCIE_LNK_WIDTH_UNKNOWN) {
		mlx4_warn(dev,
			  "Unable to determine PCI device chain minimum BW\n");
		return;
	}

	if (width != width_cap || speed != speed_cap)
		mlx4_warn(dev,
			  "PCIe BW is different than device's capability\n");

	mlx4_info(dev, "PCIe link speed is %s, device supports %s\n",
		  PCIE_SPEED_STR(speed), PCIE_SPEED_STR(speed_cap));
	mlx4_info(dev, "PCIe link width is x%d, device supports x%d\n",
		  width, width_cap);
	return;
}

/*The function checks if there are live vf, return the num of them*/
static int mlx4_how_many_lives_vf(struct mlx4_dev *dev)
{
	struct mlx4_priv *priv = mlx4_priv(dev);
	struct mlx4_slave_state *s_state;
	int i;
	int ret = 0;

	for (i = 1/*the ppf is 0*/; i < dev->num_slaves; ++i) {
		s_state = &priv->mfunc.master.slave_state[i];
		if (s_state->active && s_state->last_cmd !=
		    MLX4_COMM_CMD_RESET) {
			mlx4_warn(dev, "%s: slave: %d is still active\n",
				  __func__, i);
			ret++;
		}
	}
	return ret;
}

int mlx4_get_parav_qkey(struct mlx4_dev *dev, u32 qpn, u32 *qkey)
{
	u32 qk = MLX4_RESERVED_QKEY_BASE;

	if (qpn >= dev->phys_caps.base_tunnel_sqpn + 8 * MLX4_MFUNC_MAX ||
	    qpn < dev->phys_caps.base_proxy_sqpn)
		return -EINVAL;

	if (qpn >= dev->phys_caps.base_tunnel_sqpn)
		/* tunnel qp */
		qk += qpn - dev->phys_caps.base_tunnel_sqpn;
	else
		qk += qpn - dev->phys_caps.base_proxy_sqpn;
	*qkey = qk;
	return 0;
}
EXPORT_SYMBOL(mlx4_get_parav_qkey);

void mlx4_sync_pkey_table(struct mlx4_dev *dev, int slave, int port, int i, int val)
{
	struct mlx4_priv *priv = container_of(dev, struct mlx4_priv, dev);

	if (!mlx4_is_master(dev))
		return;

	priv->virt2phys_pkey[slave][port - 1][i] = val;
}
EXPORT_SYMBOL(mlx4_sync_pkey_table);

void mlx4_put_slave_node_guid(struct mlx4_dev *dev, int slave, __be64 guid)
{
	struct mlx4_priv *priv = container_of(dev, struct mlx4_priv, dev);

	if (!mlx4_is_master(dev))
		return;

	priv->slave_node_guids[slave] = guid;
}
EXPORT_SYMBOL(mlx4_put_slave_node_guid);

__be64 mlx4_get_slave_node_guid(struct mlx4_dev *dev, int slave)
{
	struct mlx4_priv *priv = container_of(dev, struct mlx4_priv, dev);

	if (!mlx4_is_master(dev))
		return 0;

	return priv->slave_node_guids[slave];
}
EXPORT_SYMBOL(mlx4_get_slave_node_guid);

int mlx4_is_slave_active(struct mlx4_dev *dev, int slave)
{
	struct mlx4_priv *priv = mlx4_priv(dev);
	struct mlx4_slave_state *s_slave;

	if (!mlx4_is_master(dev))
		return 0;

	s_slave = &priv->mfunc.master.slave_state[slave];
	return !!s_slave->active;
}
EXPORT_SYMBOL(mlx4_is_slave_active);

static void slave_adjust_steering_mode(struct mlx4_dev *dev,
				       struct mlx4_dev_cap *dev_cap,
				       struct mlx4_init_hca_param *hca_param)
{
	dev->caps.steering_mode = hca_param->steering_mode;
	if (dev->caps.steering_mode == MLX4_STEERING_MODE_DEVICE_MANAGED) {
		dev->caps.num_qp_per_mgm = dev_cap->fs_max_num_qp_per_entry;
		dev->caps.fs_log_max_ucast_qp_range_size =
			dev_cap->fs_log_max_ucast_qp_range_size;
	} else
		dev->caps.num_qp_per_mgm =
			4 * ((1 << hca_param->log_mc_entry_sz)/16 - 2);

	mlx4_dbg(dev, "Steering mode is: %s\n",
		 mlx4_steering_mode_str(dev->caps.steering_mode));
}

static int mlx4_slave_cap(struct mlx4_dev *dev)
{
	int			   err;
	u32			   page_size;
	struct mlx4_dev_cap	   dev_cap;
	struct mlx4_func_cap	   func_cap;
	struct mlx4_init_hca_param hca_param;
	u8			   i;

	memset(&hca_param, 0, sizeof(hca_param));
	err = mlx4_QUERY_HCA(dev, &hca_param);
	if (err) {
		mlx4_err(dev, "QUERY_HCA command failed, aborting\n");
		return err;
	}

	/* fail if the hca has an unknown global capability
	 * at this time global_caps should be always zeroed
	 */
	if (hca_param.global_caps) {
		mlx4_err(dev, "Unknown hca global capabilities\n");
		return -ENOSYS;
	}

	mlx4_log_num_mgm_entry_size = hca_param.log_mc_entry_sz;

	dev->caps.hca_core_clock = hca_param.hca_core_clock;

	memset(&dev_cap, 0, sizeof(dev_cap));
	dev->caps.max_qp_dest_rdma = 1 << hca_param.log_rd_per_qp;
	err = mlx4_dev_cap(dev, &dev_cap);
	if (err) {
		mlx4_err(dev, "QUERY_DEV_CAP command failed, aborting\n");
		return err;
	}

	err = mlx4_QUERY_FW(dev);
	if (err)
		mlx4_err(dev, "QUERY_FW command failed: could not get FW version\n");

	page_size = ~dev->caps.page_size_cap + 1;
	mlx4_warn(dev, "HCA minimum page size:%d\n", page_size);
	if (page_size > PAGE_SIZE) {
		mlx4_err(dev, "HCA minimum page size of %d bigger than kernel PAGE_SIZE of %ld, aborting\n",
			 page_size, PAGE_SIZE);
		return -ENODEV;
	}

	/* slave gets uar page size from QUERY_HCA fw command */
	dev->caps.uar_page_size = 1 << (hca_param.uar_page_sz + 12);

	/* TODO: relax this assumption */
	if (dev->caps.uar_page_size != PAGE_SIZE) {
		mlx4_err(dev, "UAR size:%d != kernel PAGE_SIZE of %ld\n",
			 dev->caps.uar_page_size, PAGE_SIZE);
		return -ENODEV;
	}

	memset(&func_cap, 0, sizeof(func_cap));
	err = mlx4_QUERY_FUNC_CAP(dev, 0, &func_cap);
	if (err) {
		mlx4_err(dev, "QUERY_FUNC_CAP general command failed, aborting (%d)\n",
			 err);
		return err;
	}

	if ((func_cap.pf_context_behaviour | PF_CONTEXT_BEHAVIOUR_MASK) !=
	    PF_CONTEXT_BEHAVIOUR_MASK) {
		mlx4_err(dev, "Unknown pf context behaviour %x known flags %x\n",
			 func_cap.pf_context_behaviour, PF_CONTEXT_BEHAVIOUR_MASK);
		return -ENOSYS;
	}

	dev->caps.num_ports		= func_cap.num_ports;
	dev->quotas.qp			= func_cap.qp_quota;
	dev->quotas.srq			= func_cap.srq_quota;
	dev->quotas.cq			= func_cap.cq_quota;
	dev->quotas.mpt			= func_cap.mpt_quota;
	dev->quotas.mtt			= func_cap.mtt_quota;
	dev->caps.num_qps		= 1 << hca_param.log_num_qps;
	dev->caps.num_srqs		= 1 << hca_param.log_num_srqs;
	dev->caps.num_cqs		= 1 << hca_param.log_num_cqs;
	dev->caps.num_mpts		= 1 << hca_param.log_mpt_sz;
	dev->caps.num_eqs		= func_cap.max_eq;
	dev->caps.reserved_eqs		= func_cap.reserved_eq;
	dev->caps.num_pds               = MLX4_NUM_PDS;
	dev->caps.num_mgms              = 0;
	dev->caps.num_amgms             = 0;

	if (dev->caps.num_ports > MLX4_MAX_PORTS) {
		mlx4_err(dev, "HCA has %d ports, but we only support %d, aborting\n",
			 dev->caps.num_ports, MLX4_MAX_PORTS);
		return -ENODEV;
	}

	dev->caps.qp0_qkey = kcalloc(dev->caps.num_ports, sizeof(u32), GFP_KERNEL);
	dev->caps.qp0_tunnel = kcalloc(dev->caps.num_ports, sizeof (u32), GFP_KERNEL);
	dev->caps.qp0_proxy = kcalloc(dev->caps.num_ports, sizeof (u32), GFP_KERNEL);
	dev->caps.qp1_tunnel = kcalloc(dev->caps.num_ports, sizeof (u32), GFP_KERNEL);
	dev->caps.qp1_proxy = kcalloc(dev->caps.num_ports, sizeof (u32), GFP_KERNEL);

	if (!dev->caps.qp0_tunnel || !dev->caps.qp0_proxy ||
	    !dev->caps.qp1_tunnel || !dev->caps.qp1_proxy ||
	    !dev->caps.qp0_qkey) {
		err = -ENOMEM;
		goto err_mem;
	}

	for (i = 1; i <= dev->caps.num_ports; ++i) {
		err = mlx4_QUERY_FUNC_CAP(dev, i, &func_cap);
		if (err) {
			mlx4_err(dev, "QUERY_FUNC_CAP port command failed for port %d, aborting (%d)\n",
				 i, err);
			goto err_mem;
		}
		dev->caps.qp0_qkey[i - 1] = func_cap.qp0_qkey;
		dev->caps.qp0_tunnel[i - 1] = func_cap.qp0_tunnel_qpn;
		dev->caps.qp0_proxy[i - 1] = func_cap.qp0_proxy_qpn;
		dev->caps.qp1_tunnel[i - 1] = func_cap.qp1_tunnel_qpn;
		dev->caps.qp1_proxy[i - 1] = func_cap.qp1_proxy_qpn;
		dev->caps.port_mask[i] = dev->caps.port_type[i];
		dev->caps.phys_port_id[i] = func_cap.phys_port_id;
		if (mlx4_get_slave_pkey_gid_tbl_len(dev, i,
						    &dev->caps.gid_table_len[i],
						    &dev->caps.pkey_table_len[i]))
			goto err_mem;
	}

	if (dev->caps.uar_page_size * (dev->caps.num_uars -
				       dev->caps.reserved_uars) >
				       pci_resource_len(dev->pdev, 2)) {
		mlx4_err(dev, "HCA reported UAR region size of 0x%x bigger than PCI resource 2 size of 0x%llx, aborting\n",
			 dev->caps.uar_page_size * dev->caps.num_uars,
			 (unsigned long long) pci_resource_len(dev->pdev, 2));
		goto err_mem;
	}

	if (hca_param.dev_cap_enabled & MLX4_DEV_CAP_64B_EQE_ENABLED) {
		dev->caps.eqe_size   = 64;
		dev->caps.eqe_factor = 1;
	} else {
		dev->caps.eqe_size   = 32;
		dev->caps.eqe_factor = 0;
	}

	if (hca_param.dev_cap_enabled & MLX4_DEV_CAP_64B_CQE_ENABLED) {
		dev->caps.cqe_size   = 64;
		dev->caps.userspace_caps |= MLX4_USER_DEV_CAP_LARGE_CQE;
	} else {
		dev->caps.cqe_size   = 32;
	}

	if (hca_param.dev_cap_enabled & MLX4_DEV_CAP_EQE_STRIDE_ENABLED) {
		dev->caps.eqe_size = hca_param.eqe_size;
		dev->caps.eqe_factor = 0;
	}

	if (hca_param.dev_cap_enabled & MLX4_DEV_CAP_CQE_STRIDE_ENABLED) {
		dev->caps.cqe_size = hca_param.cqe_size;
		/* User still need to know when CQE > 32B */
		dev->caps.userspace_caps |= MLX4_USER_DEV_CAP_LARGE_CQE;
	}

	dev->caps.flags2 &= ~MLX4_DEV_CAP_FLAG2_TS;
	mlx4_warn(dev, "Timestamping is not supported in slave mode\n");

	slave_adjust_steering_mode(dev, &dev_cap, &hca_param);

	if (func_cap.extra_flags & MLX4_QUERY_FUNC_FLAGS_BF_RES_QP &&
	    dev->caps.bf_reg_size)
		dev->caps.alloc_res_qp_mask |= MLX4_RESERVE_ETH_BF_QP;

	if (func_cap.extra_flags & MLX4_QUERY_FUNC_FLAGS_A0_RES_QP)
		dev->caps.alloc_res_qp_mask |= MLX4_RESERVE_A0_QP;

	return 0;

err_mem:
	kfree(dev->caps.qp0_qkey);
	kfree(dev->caps.qp0_tunnel);
	kfree(dev->caps.qp0_proxy);
	kfree(dev->caps.qp1_tunnel);
	kfree(dev->caps.qp1_proxy);
	dev->caps.qp0_qkey = NULL;
	dev->caps.qp0_tunnel = NULL;
	dev->caps.qp0_proxy = NULL;
	dev->caps.qp1_tunnel = NULL;
	dev->caps.qp1_proxy = NULL;

	return err;
}

static void mlx4_request_modules(struct mlx4_dev *dev)
{
	int port;
	int has_ib_port = false;
	int has_eth_port = false;
#define EN_DRV_NAME	"mlx4_en"
#define IB_DRV_NAME	"mlx4_ib"

	for (port = 1; port <= dev->caps.num_ports; port++) {
		if (dev->caps.port_type[port] == MLX4_PORT_TYPE_IB)
			has_ib_port = true;
		else if (dev->caps.port_type[port] == MLX4_PORT_TYPE_ETH)
			has_eth_port = true;
	}

	if (has_eth_port)
		request_module_nowait(EN_DRV_NAME);
	if (has_ib_port || (dev->caps.flags & MLX4_DEV_CAP_FLAG_IBOE))
		request_module_nowait(IB_DRV_NAME);
}

/*
 * Change the port configuration of the device.
 * Every user of this function must hold the port mutex.
 */
int mlx4_change_port_types(struct mlx4_dev *dev,
			   enum mlx4_port_type *port_types)
{
	int err = 0;
	int change = 0;
	int port;

	for (port = 0; port <  dev->caps.num_ports; port++) {
		/* Change the port type only if the new type is different
		 * from the current, and not set to Auto */
		if (port_types[port] != dev->caps.port_type[port + 1])
			change = 1;
	}
	if (change) {
		mlx4_unregister_device(dev);
		for (port = 1; port <= dev->caps.num_ports; port++) {
			mlx4_CLOSE_PORT(dev, port);
			dev->caps.port_type[port] = port_types[port - 1];
			err = mlx4_SET_PORT(dev, port, -1);
			if (err) {
				mlx4_err(dev, "Failed to set port %d, aborting\n",
					 port);
				goto out;
			}
		}
		mlx4_set_port_mask(dev);
		err = mlx4_register_device(dev);
		if (err) {
			mlx4_err(dev, "Failed to register device\n");
			goto out;
		}
		mlx4_request_modules(dev);
	}

out:
	return err;
}

static ssize_t show_port_type(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct mlx4_port_info *info = container_of(attr, struct mlx4_port_info,
						   port_attr);
	struct mlx4_dev *mdev = info->dev;
	char type[8];

	sprintf(type, "%s",
		(mdev->caps.port_type[info->port] == MLX4_PORT_TYPE_IB) ?
		"ib" : "eth");
	if (mdev->caps.possible_type[info->port] == MLX4_PORT_TYPE_AUTO)
		sprintf(buf, "auto (%s)\n", type);
	else
		sprintf(buf, "%s\n", type);

	return strlen(buf);
}

static ssize_t set_port_type(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct mlx4_port_info *info = container_of(attr, struct mlx4_port_info,
						   port_attr);
	struct mlx4_dev *mdev = info->dev;
	struct mlx4_priv *priv = mlx4_priv(mdev);
	enum mlx4_port_type types[MLX4_MAX_PORTS];
	enum mlx4_port_type new_types[MLX4_MAX_PORTS];
	static DEFINE_MUTEX(set_port_type_mutex);
	int i;
	int err = 0;

	mutex_lock(&set_port_type_mutex);

	if (!strcmp(buf, "ib\n"))
		info->tmp_type = MLX4_PORT_TYPE_IB;
	else if (!strcmp(buf, "eth\n"))
		info->tmp_type = MLX4_PORT_TYPE_ETH;
	else if (!strcmp(buf, "auto\n"))
		info->tmp_type = MLX4_PORT_TYPE_AUTO;
	else {
		mlx4_err(mdev, "%s is not supported port type\n", buf);
		err = -EINVAL;
		goto err_out;
	}

	mlx4_stop_sense(mdev);
	mutex_lock(&priv->port_mutex);
	/* Possible type is always the one that was delivered */
	mdev->caps.possible_type[info->port] = info->tmp_type;

	for (i = 0; i < mdev->caps.num_ports; i++) {
		types[i] = priv->port[i+1].tmp_type ? priv->port[i+1].tmp_type :
					mdev->caps.possible_type[i+1];
		if (types[i] == MLX4_PORT_TYPE_AUTO)
			types[i] = mdev->caps.port_type[i+1];
	}

	if (!(mdev->caps.flags & MLX4_DEV_CAP_FLAG_DPDP) &&
	    !(mdev->caps.flags & MLX4_DEV_CAP_FLAG_SENSE_SUPPORT)) {
		for (i = 1; i <= mdev->caps.num_ports; i++) {
			if (mdev->caps.possible_type[i] == MLX4_PORT_TYPE_AUTO) {
				mdev->caps.possible_type[i] = mdev->caps.port_type[i];
				err = -EINVAL;
			}
		}
	}
	if (err) {
		mlx4_err(mdev, "Auto sensing is not supported on this HCA. Set only 'eth' or 'ib' for both ports (should be the same)\n");
		goto out;
	}

	mlx4_do_sense_ports(mdev, new_types, types);

	err = mlx4_check_port_params(mdev, new_types);
	if (err)
		goto out;

	/* We are about to apply the changes after the configuration
	 * was verified, no need to remember the temporary types
	 * any more */
	for (i = 0; i < mdev->caps.num_ports; i++)
		priv->port[i + 1].tmp_type = 0;

	err = mlx4_change_port_types(mdev, new_types);

out:
	mlx4_start_sense(mdev);
	mutex_unlock(&priv->port_mutex);
err_out:
	mutex_unlock(&set_port_type_mutex);

	return err ? err : count;
}

enum ibta_mtu {
	IB_MTU_256  = 1,
	IB_MTU_512  = 2,
	IB_MTU_1024 = 3,
	IB_MTU_2048 = 4,
	IB_MTU_4096 = 5
};

static inline int int_to_ibta_mtu(int mtu)
{
	switch (mtu) {
	case 256:  return IB_MTU_256;
	case 512:  return IB_MTU_512;
	case 1024: return IB_MTU_1024;
	case 2048: return IB_MTU_2048;
	case 4096: return IB_MTU_4096;
	default: return -1;
	}
}

static inline int ibta_mtu_to_int(enum ibta_mtu mtu)
{
	switch (mtu) {
	case IB_MTU_256:  return  256;
	case IB_MTU_512:  return  512;
	case IB_MTU_1024: return 1024;
	case IB_MTU_2048: return 2048;
	case IB_MTU_4096: return 4096;
	default: return -1;
	}
}

static ssize_t show_port_ib_mtu(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	struct mlx4_port_info *info = container_of(attr, struct mlx4_port_info,
						   port_mtu_attr);
	struct mlx4_dev *mdev = info->dev;

	if (mdev->caps.port_type[info->port] == MLX4_PORT_TYPE_ETH)
		mlx4_warn(mdev, "port level mtu is only used for IB ports\n");

	sprintf(buf, "%d\n",
			ibta_mtu_to_int(mdev->caps.port_ib_mtu[info->port]));
	return strlen(buf);
}

static ssize_t set_port_ib_mtu(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct mlx4_port_info *info = container_of(attr, struct mlx4_port_info,
						   port_mtu_attr);
	struct mlx4_dev *mdev = info->dev;
	struct mlx4_priv *priv = mlx4_priv(mdev);
	int err, port, mtu, ibta_mtu = -1;

	if (mdev->caps.port_type[info->port] == MLX4_PORT_TYPE_ETH) {
		mlx4_warn(mdev, "port level mtu is only used for IB ports\n");
		return -EINVAL;
	}

	err = kstrtoint(buf, 0, &mtu);
	if (!err)
		ibta_mtu = int_to_ibta_mtu(mtu);

	if (err || ibta_mtu < 0) {
		mlx4_err(mdev, "%s is invalid IBTA mtu\n", buf);
		return -EINVAL;
	}

	mdev->caps.port_ib_mtu[info->port] = ibta_mtu;

	mlx4_stop_sense(mdev);
	mutex_lock(&priv->port_mutex);
	mlx4_unregister_device(mdev);
	for (port = 1; port <= mdev->caps.num_ports; port++) {
		mlx4_CLOSE_PORT(mdev, port);
		err = mlx4_SET_PORT(mdev, port, -1);
		if (err) {
			mlx4_err(mdev, "Failed to set port %d, aborting\n",
				 port);
			goto err_set_port;
		}
	}
	err = mlx4_register_device(mdev);
err_set_port:
	mutex_unlock(&priv->port_mutex);
	mlx4_start_sense(mdev);
	return err ? err : count;
}

static int mlx4_load_fw(struct mlx4_dev *dev)
{
	struct mlx4_priv *priv = mlx4_priv(dev);
	int err;

	priv->fw.fw_icm = mlx4_alloc_icm(dev, priv->fw.fw_pages,
					 GFP_HIGHUSER | __GFP_NOWARN, 0);
	if (!priv->fw.fw_icm) {
		mlx4_err(dev, "Couldn't allocate FW area, aborting\n");
		return -ENOMEM;
	}

	err = mlx4_MAP_FA(dev, priv->fw.fw_icm);
	if (err) {
		mlx4_err(dev, "MAP_FA command failed, aborting\n");
		goto err_free;
	}

	err = mlx4_RUN_FW(dev);
	if (err) {
		mlx4_err(dev, "RUN_FW command failed, aborting\n");
		goto err_unmap_fa;
	}

	return 0;

err_unmap_fa:
	mlx4_UNMAP_FA(dev);

err_free:
	mlx4_free_icm(dev, priv->fw.fw_icm, 0);
	return err;
}

static int mlx4_init_cmpt_table(struct mlx4_dev *dev, u64 cmpt_base,
				int cmpt_entry_sz)
{
	struct mlx4_priv *priv = mlx4_priv(dev);
	int err;
	int num_eqs;

	err = mlx4_init_icm_table(dev, &priv->qp_table.cmpt_table,
				  cmpt_base +
				  ((u64) (MLX4_CMPT_TYPE_QP *
					  cmpt_entry_sz) << MLX4_CMPT_SHIFT),
				  cmpt_entry_sz, dev->caps.num_qps,
				  dev->caps.reserved_qps_cnt[MLX4_QP_REGION_FW],
				  0, 0);
	if (err)
		goto err;

	err = mlx4_init_icm_table(dev, &priv->srq_table.cmpt_table,
				  cmpt_base +
				  ((u64) (MLX4_CMPT_TYPE_SRQ *
					  cmpt_entry_sz) << MLX4_CMPT_SHIFT),
				  cmpt_entry_sz, dev->caps.num_srqs,
				  dev->caps.reserved_srqs, 0, 0);
	if (err)
		goto err_qp;

	err = mlx4_init_icm_table(dev, &priv->cq_table.cmpt_table,
				  cmpt_base +
				  ((u64) (MLX4_CMPT_TYPE_CQ *
					  cmpt_entry_sz) << MLX4_CMPT_SHIFT),
				  cmpt_entry_sz, dev->caps.num_cqs,
				  dev->caps.reserved_cqs, 0, 0);
	if (err)
		goto err_srq;

	num_eqs = dev->phys_caps.num_phys_eqs;
	err = mlx4_init_icm_table(dev, &priv->eq_table.cmpt_table,
				  cmpt_base +
				  ((u64) (MLX4_CMPT_TYPE_EQ *
					  cmpt_entry_sz) << MLX4_CMPT_SHIFT),
				  cmpt_entry_sz, num_eqs, num_eqs, 0, 0);
	if (err)
		goto err_cq;

	return 0;

err_cq:
	mlx4_cleanup_icm_table(dev, &priv->cq_table.cmpt_table);

err_srq:
	mlx4_cleanup_icm_table(dev, &priv->srq_table.cmpt_table);

err_qp:
	mlx4_cleanup_icm_table(dev, &priv->qp_table.cmpt_table);

err:
	return err;
}

static int mlx4_init_icm(struct mlx4_dev *dev, struct mlx4_dev_cap *dev_cap,
			 struct mlx4_init_hca_param *init_hca, u64 icm_size)
{
	struct mlx4_priv *priv = mlx4_priv(dev);
	u64 aux_pages;
	int num_eqs;
	int err;

	err = mlx4_SET_ICM_SIZE(dev, icm_size, &aux_pages);
	if (err) {
		mlx4_err(dev, "SET_ICM_SIZE command failed, aborting\n");
		return err;
	}

	mlx4_dbg(dev, "%lld KB of HCA context requires %lld KB aux memory\n",
		 (unsigned long long) icm_size >> 10,
		 (unsigned long long) aux_pages << 2);

	priv->fw.aux_icm = mlx4_alloc_icm(dev, aux_pages,
					  GFP_HIGHUSER | __GFP_NOWARN, 0);
	if (!priv->fw.aux_icm) {
		mlx4_err(dev, "Couldn't allocate aux memory, aborting\n");
		return -ENOMEM;
	}

	err = mlx4_MAP_ICM_AUX(dev, priv->fw.aux_icm);
	if (err) {
		mlx4_err(dev, "MAP_ICM_AUX command failed, aborting\n");
		goto err_free_aux;
	}

	err = mlx4_init_cmpt_table(dev, init_hca->cmpt_base, dev_cap->cmpt_entry_sz);
	if (err) {
		mlx4_err(dev, "Failed to map cMPT context memory, aborting\n");
		goto err_unmap_aux;
	}


	num_eqs = dev->phys_caps.num_phys_eqs;
	err = mlx4_init_icm_table(dev, &priv->eq_table.table,
				  init_hca->eqc_base, dev_cap->eqc_entry_sz,
				  num_eqs, num_eqs, 0, 0);
	if (err) {
		mlx4_err(dev, "Failed to map EQ context memory, aborting\n");
		goto err_unmap_cmpt;
	}

	/*
	 * Reserved MTT entries must be aligned up to a cacheline
	 * boundary, since the FW will write to them, while the driver
	 * writes to all other MTT entries. (The variable
	 * dev->caps.mtt_entry_sz below is really the MTT segment
	 * size, not the raw entry size)
	 */
	dev->caps.reserved_mtts =
		ALIGN(dev->caps.reserved_mtts * dev->caps.mtt_entry_sz,
		      dma_get_cache_alignment()) / dev->caps.mtt_entry_sz;

	err = mlx4_init_icm_table(dev, &priv->mr_table.mtt_table,
				  init_hca->mtt_base,
				  dev->caps.mtt_entry_sz,
				  dev->caps.num_mtts,
				  dev->caps.reserved_mtts, 1, 0);
	if (err) {
		mlx4_err(dev, "Failed to map MTT context memory, aborting\n");
		goto err_unmap_eq;
	}

	err = mlx4_init_icm_table(dev, &priv->mr_table.dmpt_table,
				  init_hca->dmpt_base,
				  dev_cap->dmpt_entry_sz,
				  dev->caps.num_mpts,
				  dev->caps.reserved_mrws, 1, 1);
	if (err) {
		mlx4_err(dev, "Failed to map dMPT context memory, aborting\n");
		goto err_unmap_mtt;
	}

	err = mlx4_init_icm_table(dev, &priv->qp_table.qp_table,
				  init_hca->qpc_base,
				  dev_cap->qpc_entry_sz,
				  dev->caps.num_qps,
				  dev->caps.reserved_qps_cnt[MLX4_QP_REGION_FW],
				  0, 0);
	if (err) {
		mlx4_err(dev, "Failed to map QP context memory, aborting\n");
		goto err_unmap_dmpt;
	}

	err = mlx4_init_icm_table(dev, &priv->qp_table.auxc_table,
				  init_hca->auxc_base,
				  dev_cap->aux_entry_sz,
				  dev->caps.num_qps,
				  dev->caps.reserved_qps_cnt[MLX4_QP_REGION_FW],
				  0, 0);
	if (err) {
		mlx4_err(dev, "Failed to map AUXC context memory, aborting\n");
		goto err_unmap_qp;
	}

	err = mlx4_init_icm_table(dev, &priv->qp_table.altc_table,
				  init_hca->altc_base,
				  dev_cap->altc_entry_sz,
				  dev->caps.num_qps,
				  dev->caps.reserved_qps_cnt[MLX4_QP_REGION_FW],
				  0, 0);
	if (err) {
		mlx4_err(dev, "Failed to map ALTC context memory, aborting\n");
		goto err_unmap_auxc;
	}

	err = mlx4_init_icm_table(dev, &priv->qp_table.rdmarc_table,
				  init_hca->rdmarc_base,
				  dev_cap->rdmarc_entry_sz << priv->qp_table.rdmarc_shift,
				  dev->caps.num_qps,
				  dev->caps.reserved_qps_cnt[MLX4_QP_REGION_FW],
				  0, 0);
	if (err) {
		mlx4_err(dev, "Failed to map RDMARC context memory, aborting\n");
		goto err_unmap_altc;
	}

	err = mlx4_init_icm_table(dev, &priv->cq_table.table,
				  init_hca->cqc_base,
				  dev_cap->cqc_entry_sz,
				  dev->caps.num_cqs,
				  dev->caps.reserved_cqs, 0, 0);
	if (err) {
		mlx4_err(dev, "Failed to map CQ context memory, aborting\n");
		goto err_unmap_rdmarc;
	}

	err = mlx4_init_icm_table(dev, &priv->srq_table.table,
				  init_hca->srqc_base,
				  dev_cap->srq_entry_sz,
				  dev->caps.num_srqs,
				  dev->caps.reserved_srqs, 0, 0);
	if (err) {
		mlx4_err(dev, "Failed to map SRQ context memory, aborting\n");
		goto err_unmap_cq;
	}

	/*
	 * For flow steering device managed mode it is required to use
	 * mlx4_init_icm_table. For B0 steering mode it's not strictly
	 * required, but for simplicity just map the whole multicast
	 * group table now.  The table isn't very big and it's a lot
	 * easier than trying to track ref counts.
	 */
	err = mlx4_init_icm_table(dev, &priv->mcg_table.table,
				  init_hca->mc_base,
				  mlx4_get_mgm_entry_size(dev),
				  dev->caps.num_mgms + dev->caps.num_amgms,
				  dev->caps.num_mgms + dev->caps.num_amgms,
				  0, 0);
	if (err) {
		mlx4_err(dev, "Failed to map MCG context memory, aborting\n");
		goto err_unmap_srq;
	}

	return 0;

err_unmap_srq:
	mlx4_cleanup_icm_table(dev, &priv->srq_table.table);

err_unmap_cq:
	mlx4_cleanup_icm_table(dev, &priv->cq_table.table);

err_unmap_rdmarc:
	mlx4_cleanup_icm_table(dev, &priv->qp_table.rdmarc_table);

err_unmap_altc:
	mlx4_cleanup_icm_table(dev, &priv->qp_table.altc_table);

err_unmap_auxc:
	mlx4_cleanup_icm_table(dev, &priv->qp_table.auxc_table);

err_unmap_qp:
	mlx4_cleanup_icm_table(dev, &priv->qp_table.qp_table);

err_unmap_dmpt:
	mlx4_cleanup_icm_table(dev, &priv->mr_table.dmpt_table);

err_unmap_mtt:
	mlx4_cleanup_icm_table(dev, &priv->mr_table.mtt_table);

err_unmap_eq:
	mlx4_cleanup_icm_table(dev, &priv->eq_table.table);

err_unmap_cmpt:
	mlx4_cleanup_icm_table(dev, &priv->eq_table.cmpt_table);
	mlx4_cleanup_icm_table(dev, &priv->cq_table.cmpt_table);
	mlx4_cleanup_icm_table(dev, &priv->srq_table.cmpt_table);
	mlx4_cleanup_icm_table(dev, &priv->qp_table.cmpt_table);

err_unmap_aux:
	mlx4_UNMAP_ICM_AUX(dev);

err_free_aux:
	mlx4_free_icm(dev, priv->fw.aux_icm, 0);

	return err;
}

static void mlx4_free_icms(struct mlx4_dev *dev)
{
	struct mlx4_priv *priv = mlx4_priv(dev);

	mlx4_cleanup_icm_table(dev, &priv->mcg_table.table);
	mlx4_cleanup_icm_table(dev, &priv->srq_table.table);
	mlx4_cleanup_icm_table(dev, &priv->cq_table.table);
	mlx4_cleanup_icm_table(dev, &priv->qp_table.rdmarc_table);
	mlx4_cleanup_icm_table(dev, &priv->qp_table.altc_table);
	mlx4_cleanup_icm_table(dev, &priv->qp_table.auxc_table);
	mlx4_cleanup_icm_table(dev, &priv->qp_table.qp_table);
	mlx4_cleanup_icm_table(dev, &priv->mr_table.dmpt_table);
	mlx4_cleanup_icm_table(dev, &priv->mr_table.mtt_table);
	mlx4_cleanup_icm_table(dev, &priv->eq_table.table);
	mlx4_cleanup_icm_table(dev, &priv->eq_table.cmpt_table);
	mlx4_cleanup_icm_table(dev, &priv->cq_table.cmpt_table);
	mlx4_cleanup_icm_table(dev, &priv->srq_table.cmpt_table);
	mlx4_cleanup_icm_table(dev, &priv->qp_table.cmpt_table);

	mlx4_UNMAP_ICM_AUX(dev);
	mlx4_free_icm(dev, priv->fw.aux_icm, 0);
}

static void mlx4_slave_exit(struct mlx4_dev *dev)
{
	struct mlx4_priv *priv = mlx4_priv(dev);

	mutex_lock(&priv->cmd.slave_cmd_mutex);
	if (mlx4_comm_cmd(dev, MLX4_COMM_CMD_RESET, 0, MLX4_COMM_TIME))
		mlx4_warn(dev, "Failed to close slave function\n");
	mutex_unlock(&priv->cmd.slave_cmd_mutex);
}

static int map_bf_area(struct mlx4_dev *dev)
{
	struct mlx4_priv *priv = mlx4_priv(dev);
	resource_size_t bf_start;
	resource_size_t bf_len;
	int err = 0;

	if (!dev->caps.bf_reg_size)
		return -ENXIO;

	bf_start = pci_resource_start(dev->pdev, 2) +
			(dev->caps.num_uars << PAGE_SHIFT);
	bf_len = pci_resource_len(dev->pdev, 2) -
			(dev->caps.num_uars << PAGE_SHIFT);
	priv->bf_mapping = io_mapping_create_wc(bf_start, bf_len);
	if (!priv->bf_mapping)
		err = -ENOMEM;

	return err;
}

static void unmap_bf_area(struct mlx4_dev *dev)
{
	if (mlx4_priv(dev)->bf_mapping)
		io_mapping_free(mlx4_priv(dev)->bf_mapping);
}

cycle_t mlx4_read_clock(struct mlx4_dev *dev)
{
	u32 clockhi, clocklo, clockhi1;
	cycle_t cycles;
	int i;
	struct mlx4_priv *priv = mlx4_priv(dev);

	for (i = 0; i < 10; i++) {
		clockhi = swab32(readl(priv->clock_mapping));
		clocklo = swab32(readl(priv->clock_mapping + 4));
		clockhi1 = swab32(readl(priv->clock_mapping));
		if (clockhi == clockhi1)
			break;
	}

	cycles = (u64) clockhi << 32 | (u64) clocklo;

	return cycles;
}
EXPORT_SYMBOL_GPL(mlx4_read_clock);


static int map_internal_clock(struct mlx4_dev *dev)
{
	struct mlx4_priv *priv = mlx4_priv(dev);

	priv->clock_mapping =
		ioremap(pci_resource_start(dev->pdev, priv->fw.clock_bar) +
			priv->fw.clock_offset, MLX4_CLOCK_SIZE);

	if (!priv->clock_mapping)
		return -ENOMEM;

	return 0;
}

static void unmap_internal_clock(struct mlx4_dev *dev)
{
	struct mlx4_priv *priv = mlx4_priv(dev);

	if (priv->clock_mapping)
		iounmap(priv->clock_mapping);
}

static void mlx4_close_hca(struct mlx4_dev *dev)
{
	unmap_internal_clock(dev);
	unmap_bf_area(dev);
	if (mlx4_is_slave(dev))
		mlx4_slave_exit(dev);
	else {
		mlx4_CLOSE_HCA(dev, 0);
		mlx4_free_icms(dev);
	}
}

static void mlx4_close_fw(struct mlx4_dev *dev)
{
	if (!mlx4_is_slave(dev)) {
		mlx4_UNMAP_FA(dev);
		mlx4_free_icm(dev, mlx4_priv(dev)->fw.fw_icm, 0);
	}
}

static int mlx4_init_slave(struct mlx4_dev *dev)
{
	struct mlx4_priv *priv = mlx4_priv(dev);
	u64 dma = (u64) priv->mfunc.vhcr_dma;
	int ret_from_reset = 0;
	u32 slave_read;
	u32 cmd_channel_ver;

	if (atomic_read(&pf_loading)) {
		mlx4_warn(dev, "PF is not ready - Deferring probe\n");
		return -EPROBE_DEFER;
	}

	mutex_lock(&priv->cmd.slave_cmd_mutex);
	priv->cmd.max_cmds = 1;
	mlx4_warn(dev, "Sending reset\n");
	ret_from_reset = mlx4_comm_cmd(dev, MLX4_COMM_CMD_RESET, 0,
				       MLX4_COMM_TIME);
	/* if we are in the middle of flr the slave will try
	 * NUM_OF_RESET_RETRIES times before leaving.*/
	if (ret_from_reset) {
		if (MLX4_DELAY_RESET_SLAVE == ret_from_reset) {
			mlx4_warn(dev, "slave is currently in the middle of FLR - Deferring probe\n");
			mutex_unlock(&priv->cmd.slave_cmd_mutex);
			return -EPROBE_DEFER;
		} else
			goto err;
	}

	/* check the driver version - the slave I/F revision
	 * must match the master's */
	slave_read = swab32(readl(&priv->mfunc.comm->slave_read));
	cmd_channel_ver = mlx4_comm_get_version();

	if (MLX4_COMM_GET_IF_REV(cmd_channel_ver) !=
		MLX4_COMM_GET_IF_REV(slave_read)) {
		mlx4_err(dev, "slave driver version is not supported by the master\n");
		goto err;
	}

	mlx4_warn(dev, "Sending vhcr0\n");
	if (mlx4_comm_cmd(dev, MLX4_COMM_CMD_VHCR0, dma >> 48,
						    MLX4_COMM_TIME))
		goto err;
	if (mlx4_comm_cmd(dev, MLX4_COMM_CMD_VHCR1, dma >> 32,
						    MLX4_COMM_TIME))
		goto err;
	if (mlx4_comm_cmd(dev, MLX4_COMM_CMD_VHCR2, dma >> 16,
						    MLX4_COMM_TIME))
		goto err;
	if (mlx4_comm_cmd(dev, MLX4_COMM_CMD_VHCR_EN, dma, MLX4_COMM_TIME))
		goto err;

	mutex_unlock(&priv->cmd.slave_cmd_mutex);
	return 0;

err:
	mlx4_comm_cmd(dev, MLX4_COMM_CMD_RESET, 0, 0);
	mutex_unlock(&priv->cmd.slave_cmd_mutex);
	return -EIO;
}

static void mlx4_parav_master_pf_caps(struct mlx4_dev *dev)
{
	int i;

	for (i = 1; i <= dev->caps.num_ports; i++) {
		if (dev->caps.port_type[i] == MLX4_PORT_TYPE_ETH)
			dev->caps.gid_table_len[i] =
				mlx4_get_slave_num_gids(dev, 0, i);
		else
			dev->caps.gid_table_len[i] = 1;
		dev->caps.pkey_table_len[i] =
			dev->phys_caps.pkey_phys_table_len[i] - 1;
	}
}

static int choose_log_fs_mgm_entry_size(int qp_per_entry)
{
	int i = MLX4_MIN_MGM_LOG_ENTRY_SIZE;

	for (i = MLX4_MIN_MGM_LOG_ENTRY_SIZE; i <= MLX4_MAX_MGM_LOG_ENTRY_SIZE;
	      i++) {
		if (qp_per_entry <= 4 * ((1 << i) / 16 - 2))
			break;
	}

	return (i <= MLX4_MAX_MGM_LOG_ENTRY_SIZE) ? i : -1;
}

static const char *dmfs_high_rate_steering_mode_str(int dmfs_high_steer_mode)
{
	switch (dmfs_high_steer_mode) {
	case MLX4_STEERING_DMFS_A0_DEFAULT:
		return "default performance";

	case MLX4_STEERING_DMFS_A0_DYNAMIC:
		return "dynamic hybrid mode";

	case MLX4_STEERING_DMFS_A0_STATIC:
		return "performance optimized for limited rule configuration (static)";

	case MLX4_STEERING_DMFS_A0_DISABLE:
		return "disabled performance optimized steering";

	case MLX4_STEERING_DMFS_A0_NOT_SUPPORTED:
		return "performance optimized steering not supported";

	default:
		return "Unrecognized mode";
	}
}

#define MLX4_DMFS_A0_STEERING			(1UL << 2)

static void choose_steering_mode(struct mlx4_dev *dev,
				 struct mlx4_dev_cap *dev_cap)
{
	if (mlx4_log_num_mgm_entry_size <= 0) {
		if ((-mlx4_log_num_mgm_entry_size) & MLX4_DMFS_A0_STEERING) {
			if (dev->caps.dmfs_high_steer_mode ==
			    MLX4_STEERING_DMFS_A0_NOT_SUPPORTED)
				mlx4_err(dev, "DMFS high rate mode not supported\n");
			else
				dev->caps.dmfs_high_steer_mode =
					MLX4_STEERING_DMFS_A0_STATIC;
		}
	}

	if (mlx4_log_num_mgm_entry_size <= 0 &&
	    dev_cap->flags2 & MLX4_DEV_CAP_FLAG2_FS_EN &&
	    (!mlx4_is_mfunc(dev) ||
	     (dev_cap->fs_max_num_qp_per_entry >= (dev->num_vfs + 1))) &&
	    choose_log_fs_mgm_entry_size(dev_cap->fs_max_num_qp_per_entry) >=
		MLX4_MIN_MGM_LOG_ENTRY_SIZE) {
		dev->oper_log_mgm_entry_size =
			choose_log_fs_mgm_entry_size(dev_cap->fs_max_num_qp_per_entry);
		dev->caps.steering_mode = MLX4_STEERING_MODE_DEVICE_MANAGED;
		dev->caps.num_qp_per_mgm = dev_cap->fs_max_num_qp_per_entry;
		dev->caps.fs_log_max_ucast_qp_range_size =
			dev_cap->fs_log_max_ucast_qp_range_size;
	} else {
		if (dev->caps.dmfs_high_steer_mode !=
		    MLX4_STEERING_DMFS_A0_NOT_SUPPORTED)
			dev->caps.dmfs_high_steer_mode = MLX4_STEERING_DMFS_A0_DISABLE;
		if (dev->caps.flags & MLX4_DEV_CAP_FLAG_VEP_UC_STEER &&
		    dev->caps.flags & MLX4_DEV_CAP_FLAG_VEP_MC_STEER)
			dev->caps.steering_mode = MLX4_STEERING_MODE_B0;
		else {
			dev->caps.steering_mode = MLX4_STEERING_MODE_A0;

			if (dev->caps.flags & MLX4_DEV_CAP_FLAG_VEP_UC_STEER ||
			    dev->caps.flags & MLX4_DEV_CAP_FLAG_VEP_MC_STEER)
				mlx4_warn(dev, "Must have both UC_STEER and MC_STEER flags set to use B0 steering - falling back to A0 steering mode\n");
		}
		dev->oper_log_mgm_entry_size =
			mlx4_log_num_mgm_entry_size > 0 ?
			mlx4_log_num_mgm_entry_size :
			MLX4_DEFAULT_MGM_LOG_ENTRY_SIZE;
		dev->caps.num_qp_per_mgm = mlx4_get_qp_per_mgm(dev);
	}
	mlx4_dbg(dev, "Steering mode is: %s, oper_log_mgm_entry_size = %d, modparam log_num_mgm_entry_size = %d\n",
		 mlx4_steering_mode_str(dev->caps.steering_mode),
		 dev->oper_log_mgm_entry_size,
		 mlx4_log_num_mgm_entry_size);
}

static void choose_tunnel_offload_mode(struct mlx4_dev *dev,
				       struct mlx4_dev_cap *dev_cap)
{
	if (dev->caps.steering_mode == MLX4_STEERING_MODE_DEVICE_MANAGED &&
	    dev_cap->flags2 & MLX4_DEV_CAP_FLAG2_VXLAN_OFFLOADS &&
	    dev->caps.dmfs_high_steer_mode != MLX4_STEERING_DMFS_A0_STATIC)
		dev->caps.tunnel_offload_mode = MLX4_TUNNEL_OFFLOAD_MODE_VXLAN;
	else
		dev->caps.tunnel_offload_mode = MLX4_TUNNEL_OFFLOAD_MODE_NONE;

	mlx4_dbg(dev, "Tunneling offload mode is: %s\n",  (dev->caps.tunnel_offload_mode
		 == MLX4_TUNNEL_OFFLOAD_MODE_VXLAN) ? "vxlan" : "none");
}

static int mlx4_validate_optimized_steering(struct mlx4_dev *dev)
{
	int i;
	struct mlx4_port_cap port_cap;

	if (dev->caps.dmfs_high_steer_mode == MLX4_STEERING_DMFS_A0_NOT_SUPPORTED)
		return -EINVAL;

	for (i = 1; i <= dev->caps.num_ports; i++) {
		if (mlx4_dev_port(dev, i, &port_cap)) {
			mlx4_err(dev,
				 "QUERY_DEV_CAP command failed, can't veify DMFS high rate steering.\n");
		} else if ((dev->caps.dmfs_high_steer_mode !=
			    MLX4_STEERING_DMFS_A0_DEFAULT) &&
			   (port_cap.dmfs_optimized_state ==
			    !!(dev->caps.dmfs_high_steer_mode ==
			    MLX4_STEERING_DMFS_A0_DISABLE))) {
			mlx4_err(dev,
				 "DMFS high rate steer mode differ, driver requested %s but %s in FW.\n",
				 dmfs_high_rate_steering_mode_str(
					dev->caps.dmfs_high_steer_mode),
				 (port_cap.dmfs_optimized_state ?
					"enabled" : "disabled"));
		}
	}

	return 0;
}

static int mlx4_init_fw(struct mlx4_dev *dev)
{
	struct mlx4_mod_stat_cfg   mlx4_cfg;
	int err = 0;

	if (!mlx4_is_slave(dev)) {
		err = mlx4_QUERY_FW(dev);
		if (err) {
			if (err == -EACCES)
				mlx4_info(dev, "non-primary physical function, skipping\n");
			else
				mlx4_err(dev, "QUERY_FW command failed, aborting\n");
			return err;
		}

		err = mlx4_load_fw(dev);
		if (err) {
			mlx4_err(dev, "Failed to start FW, aborting\n");
			return err;
		}

		mlx4_cfg.log_pg_sz_m = 1;
		mlx4_cfg.log_pg_sz = 0;
		err = mlx4_MOD_STAT_CFG(dev, &mlx4_cfg);
		if (err)
			mlx4_warn(dev, "Failed to override log_pg_sz parameter\n");
	}

	return err;
}

static int mlx4_init_hca(struct mlx4_dev *dev)
{
	struct mlx4_priv	  *priv = mlx4_priv(dev);
	struct mlx4_adapter	   adapter;
	struct mlx4_dev_cap	   dev_cap;
	struct mlx4_profile	   profile;
	struct mlx4_init_hca_param init_hca;
	u64 icm_size;
	struct mlx4_config_dev_params params;
	int err;

	if (!mlx4_is_slave(dev)) {
		err = mlx4_dev_cap(dev, &dev_cap);
		if (err) {
			mlx4_err(dev, "QUERY_DEV_CAP command failed, aborting\n");
			goto err_stop_fw;
		}

		choose_steering_mode(dev, &dev_cap);
		choose_tunnel_offload_mode(dev, &dev_cap);

		if (dev->caps.dmfs_high_steer_mode == MLX4_STEERING_DMFS_A0_STATIC &&
		    mlx4_is_master(dev))
			dev->caps.function_caps |= MLX4_FUNC_CAP_DMFS_A0_STATIC;

		err = mlx4_get_phys_port_id(dev);
		if (err)
			mlx4_err(dev, "Fail to get physical port id\n");

		if (mlx4_is_master(dev))
			mlx4_parav_master_pf_caps(dev);

		if (mlx4_low_memory_profile()) {
			mlx4_info(dev, "Running from within kdump kernel. Using low memory profile\n");
			profile = low_mem_profile;
		} else {
			profile = default_profile;
		}
		if (dev->caps.steering_mode ==
		    MLX4_STEERING_MODE_DEVICE_MANAGED)
			profile.num_mcg = MLX4_FS_NUM_MCG;

		icm_size = mlx4_make_profile(dev, &profile, &dev_cap,
					     &init_hca);
		if ((long long) icm_size < 0) {
			err = icm_size;
			goto err_stop_fw;
		}

		dev->caps.max_fmr_maps = (1 << (32 - ilog2(dev->caps.num_mpts))) - 1;

		init_hca.log_uar_sz = ilog2(dev->caps.num_uars);
		init_hca.uar_page_sz = PAGE_SHIFT - 12;
		init_hca.mw_enabled = 0;
		if (dev->caps.flags & MLX4_DEV_CAP_FLAG_MEM_WINDOW ||
		    dev->caps.bmme_flags & MLX4_BMME_FLAG_TYPE_2_WIN)
			init_hca.mw_enabled = INIT_HCA_TPT_MW_ENABLE;

		err = mlx4_init_icm(dev, &dev_cap, &init_hca, icm_size);
		if (err)
			goto err_stop_fw;

		err = mlx4_INIT_HCA(dev, &init_hca);
		if (err) {
			mlx4_err(dev, "INIT_HCA command failed, aborting\n");
			goto err_free_icm;
		}

		if (dev_cap.flags2 & MLX4_DEV_CAP_FLAG2_SYS_EQS) {
			err = mlx4_query_func(dev, &dev_cap);
			if (err < 0) {
				mlx4_err(dev, "QUERY_FUNC command failed, aborting.\n");
				goto err_stop_fw;
			} else if (err & MLX4_QUERY_FUNC_NUM_SYS_EQS) {
				dev->caps.num_eqs = dev_cap.max_eqs;
				dev->caps.reserved_eqs = dev_cap.reserved_eqs;
				dev->caps.reserved_uars = dev_cap.reserved_uars;
			}
		}

		/*
		 * If TS is supported by FW
		 * read HCA frequency by QUERY_HCA command
		 */
		if (dev->caps.flags2 & MLX4_DEV_CAP_FLAG2_TS) {
			memset(&init_hca, 0, sizeof(init_hca));
			err = mlx4_QUERY_HCA(dev, &init_hca);
			if (err) {
				mlx4_err(dev, "QUERY_HCA command failed, disable timestamp\n");
				dev->caps.flags2 &= ~MLX4_DEV_CAP_FLAG2_TS;
			} else {
				dev->caps.hca_core_clock =
					init_hca.hca_core_clock;
			}

			/* In case we got HCA frequency 0 - disable timestamping
			 * to avoid dividing by zero
			 */
			if (!dev->caps.hca_core_clock) {
				dev->caps.flags2 &= ~MLX4_DEV_CAP_FLAG2_TS;
				mlx4_err(dev,
					 "HCA frequency is 0 - timestamping is not supported\n");
			} else if (map_internal_clock(dev)) {
				/*
				 * Map internal clock,
				 * in case of failure disable timestamping
				 */
				dev->caps.flags2 &= ~MLX4_DEV_CAP_FLAG2_TS;
				mlx4_err(dev, "Failed to map internal clock. Timestamping is not supported\n");
			}
		}

		if (dev->caps.dmfs_high_steer_mode !=
		    MLX4_STEERING_DMFS_A0_NOT_SUPPORTED) {
			if (mlx4_validate_optimized_steering(dev))
				mlx4_warn(dev, "Optimized steering validation failed\n");

			if (dev->caps.dmfs_high_steer_mode ==
			    MLX4_STEERING_DMFS_A0_DISABLE) {
				dev->caps.dmfs_high_rate_qpn_base =
					dev->caps.reserved_qps_cnt[MLX4_QP_REGION_FW];
				dev->caps.dmfs_high_rate_qpn_range =
					MLX4_A0_STEERING_TABLE_SIZE;
			}

			mlx4_dbg(dev, "DMFS high rate steer mode is: %s\n",
				 dmfs_high_rate_steering_mode_str(
					dev->caps.dmfs_high_steer_mode));
		}
	} else {
		err = mlx4_init_slave(dev);
		if (err) {
			if (err != -EPROBE_DEFER)
				mlx4_err(dev, "Failed to initialize slave\n");
			return err;
		}

		err = mlx4_slave_cap(dev);
		if (err) {
			mlx4_err(dev, "Failed to obtain slave caps\n");
			goto err_close;
		}
	}

	if (map_bf_area(dev))
		mlx4_dbg(dev, "Failed to map blue flame area\n");

	/*Only the master set the ports, all the rest got it from it.*/
	if (!mlx4_is_slave(dev))
		mlx4_set_port_mask(dev);

	err = mlx4_QUERY_ADAPTER(dev, &adapter);
	if (err) {
		mlx4_err(dev, "QUERY_ADAPTER command failed, aborting\n");
		goto unmap_bf;
	}

	/* Query CONFIG_DEV parameters */
	err = mlx4_config_dev_retrieval(dev, &params);
	if (err && err != -ENOTSUPP) {
		mlx4_err(dev, "Failed to query CONFIG_DEV parameters\n");
	} else if (!err) {
		dev->caps.rx_checksum_flags_port[1] = params.rx_csum_flags_port_1;
		dev->caps.rx_checksum_flags_port[2] = params.rx_csum_flags_port_2;
	}
	priv->eq_table.inta_pin = adapter.inta_pin;
	memcpy(dev->board_id, adapter.board_id, sizeof dev->board_id);

	return 0;

unmap_bf:
	unmap_internal_clock(dev);
	unmap_bf_area(dev);

	if (mlx4_is_slave(dev)) {
		kfree(dev->caps.qp0_qkey);
		kfree(dev->caps.qp0_tunnel);
		kfree(dev->caps.qp0_proxy);
		kfree(dev->caps.qp1_tunnel);
		kfree(dev->caps.qp1_proxy);
	}

err_close:
	if (mlx4_is_slave(dev))
		mlx4_slave_exit(dev);
	else
		mlx4_CLOSE_HCA(dev, 0);

err_free_icm:
	if (!mlx4_is_slave(dev))
		mlx4_free_icms(dev);

err_stop_fw:
	if (!mlx4_is_slave(dev)) {
		mlx4_UNMAP_FA(dev);
		mlx4_free_icm(dev, priv->fw.fw_icm, 0);
	}
	return err;
}

static int mlx4_init_counters_table(struct mlx4_dev *dev)
{
	struct mlx4_priv *priv = mlx4_priv(dev);
	int nent;

	if (!(dev->caps.flags & MLX4_DEV_CAP_FLAG_COUNTERS))
		return -ENOENT;

	nent = dev->caps.max_counters;
	return mlx4_bitmap_init(&priv->counters_bitmap, nent, nent - 1, 0, 0);
}

static void mlx4_cleanup_counters_table(struct mlx4_dev *dev)
{
	mlx4_bitmap_cleanup(&mlx4_priv(dev)->counters_bitmap);
}

int __mlx4_counter_alloc(struct mlx4_dev *dev, u32 *idx)
{
	struct mlx4_priv *priv = mlx4_priv(dev);

	if (!(dev->caps.flags & MLX4_DEV_CAP_FLAG_COUNTERS))
		return -ENOENT;

	*idx = mlx4_bitmap_alloc(&priv->counters_bitmap);
	if (*idx == -1)
		return -ENOMEM;

	return 0;
}

int mlx4_counter_alloc(struct mlx4_dev *dev, u32 *idx)
{
	u64 out_param;
	int err;

	if (mlx4_is_mfunc(dev)) {
		err = mlx4_cmd_imm(dev, 0, &out_param, RES_COUNTER,
				   RES_OP_RESERVE, MLX4_CMD_ALLOC_RES,
				   MLX4_CMD_TIME_CLASS_A, MLX4_CMD_WRAPPED);
		if (!err)
			*idx = get_param_l(&out_param);

		return err;
	}
	return __mlx4_counter_alloc(dev, idx);
}
EXPORT_SYMBOL_GPL(mlx4_counter_alloc);

void __mlx4_counter_free(struct mlx4_dev *dev, u32 idx)
{
	mlx4_bitmap_free(&mlx4_priv(dev)->counters_bitmap, idx, MLX4_USE_RR);
	return;
}

void mlx4_counter_free(struct mlx4_dev *dev, u32 idx)
{
	u64 in_param = 0;

	if (mlx4_is_mfunc(dev)) {
		set_param_l(&in_param, idx);
		mlx4_cmd(dev, in_param, RES_COUNTER, RES_OP_RESERVE,
			 MLX4_CMD_FREE_RES, MLX4_CMD_TIME_CLASS_A,
			 MLX4_CMD_WRAPPED);
		return;
	}
	__mlx4_counter_free(dev, idx);
}
EXPORT_SYMBOL_GPL(mlx4_counter_free);

static int mlx4_setup_hca(struct mlx4_dev *dev)
{
	struct mlx4_priv *priv = mlx4_priv(dev);
	int err;
	int port;
	__be32 ib_port_default_caps;

	err = mlx4_init_uar_table(dev);
	if (err) {
		mlx4_err(dev, "Failed to initialize user access region table, aborting\n");
		 return err;
	}

	err = mlx4_uar_alloc(dev, &priv->driver_uar);
	if (err) {
		mlx4_err(dev, "Failed to allocate driver access region, aborting\n");
		goto err_uar_table_free;
	}

	priv->kar = ioremap((phys_addr_t) priv->driver_uar.pfn << PAGE_SHIFT, PAGE_SIZE);
	if (!priv->kar) {
		mlx4_err(dev, "Couldn't map kernel access region, aborting\n");
		err = -ENOMEM;
		goto err_uar_free;
	}

	err = mlx4_init_pd_table(dev);
	if (err) {
		mlx4_err(dev, "Failed to initialize protection domain table, aborting\n");
		goto err_kar_unmap;
	}

	err = mlx4_init_xrcd_table(dev);
	if (err) {
		mlx4_err(dev, "Failed to initialize reliable connection domain table, aborting\n");
		goto err_pd_table_free;
	}

	err = mlx4_init_mr_table(dev);
	if (err) {
		mlx4_err(dev, "Failed to initialize memory region table, aborting\n");
		goto err_xrcd_table_free;
	}

	if (!mlx4_is_slave(dev)) {
		err = mlx4_init_mcg_table(dev);
		if (err) {
			mlx4_err(dev, "Failed to initialize multicast group table, aborting\n");
			goto err_mr_table_free;
		}
		err = mlx4_config_mad_demux(dev);
		if (err) {
			mlx4_err(dev, "Failed in config_mad_demux, aborting\n");
			goto err_mcg_table_free;
		}
	}

	err = mlx4_init_eq_table(dev);
	if (err) {
		mlx4_err(dev, "Failed to initialize event queue table, aborting\n");
		goto err_mcg_table_free;
	}

	err = mlx4_cmd_use_events(dev);
	if (err) {
		mlx4_err(dev, "Failed to switch to event-driven firmware commands, aborting\n");
		goto err_eq_table_free;
	}

	err = mlx4_NOP(dev);
	if (err) {
		if (dev->flags & MLX4_FLAG_MSI_X) {
			mlx4_warn(dev, "NOP command failed to generate MSI-X interrupt IRQ %d)\n",
				  priv->eq_table.eq[dev->caps.num_comp_vectors].irq);
			mlx4_warn(dev, "Trying again without MSI-X\n");
		} else {
			mlx4_err(dev, "NOP command failed to generate interrupt (IRQ %d), aborting\n",
				 priv->eq_table.eq[dev->caps.num_comp_vectors].irq);
			mlx4_err(dev, "BIOS or ACPI interrupt routing problem?\n");
		}

		goto err_cmd_poll;
	}

	mlx4_dbg(dev, "NOP command IRQ test passed\n");

	err = mlx4_init_cq_table(dev);
	if (err) {
		mlx4_err(dev, "Failed to initialize completion queue table, aborting\n");
		goto err_cmd_poll;
	}

	err = mlx4_init_srq_table(dev);
	if (err) {
		mlx4_err(dev, "Failed to initialize shared receive queue table, aborting\n");
		goto err_cq_table_free;
	}

	err = mlx4_init_qp_table(dev);
	if (err) {
		mlx4_err(dev, "Failed to initialize queue pair table, aborting\n");
		goto err_srq_table_free;
	}

	err = mlx4_init_counters_table(dev);
	if (err && err != -ENOENT) {
		mlx4_err(dev, "Failed to initialize counters table, aborting\n");
		goto err_qp_table_free;
	}

	if (!mlx4_is_slave(dev)) {
		for (port = 1; port <= dev->caps.num_ports; port++) {
			ib_port_default_caps = 0;
			err = mlx4_get_port_ib_caps(dev, port,
						    &ib_port_default_caps);
			if (err)
				mlx4_warn(dev, "failed to get port %d default ib capabilities (%d). Continuing with caps = 0\n",
					  port, err);
			dev->caps.ib_port_def_cap[port] = ib_port_default_caps;

			/* initialize per-slave default ib port capabilities */
			if (mlx4_is_master(dev)) {
				int i;
				for (i = 0; i < dev->num_slaves; i++) {
					if (i == mlx4_master_func_num(dev))
						continue;
					priv->mfunc.master.slave_state[i].ib_cap_mask[port] =
						ib_port_default_caps;
				}
			}

			if (mlx4_is_mfunc(dev))
				dev->caps.port_ib_mtu[port] = IB_MTU_2048;
			else
				dev->caps.port_ib_mtu[port] = IB_MTU_4096;

			err = mlx4_SET_PORT(dev, port, mlx4_is_master(dev) ?
					    dev->caps.pkey_table_len[port] : -1);
			if (err) {
				mlx4_err(dev, "Failed to set port %d, aborting\n",
					 port);
				goto err_counters_table_free;
			}
		}
	}

	return 0;

err_counters_table_free:
	mlx4_cleanup_counters_table(dev);

err_qp_table_free:
	mlx4_cleanup_qp_table(dev);

err_srq_table_free:
	mlx4_cleanup_srq_table(dev);

err_cq_table_free:
	mlx4_cleanup_cq_table(dev);

err_cmd_poll:
	mlx4_cmd_use_polling(dev);

err_eq_table_free:
	mlx4_cleanup_eq_table(dev);

err_mcg_table_free:
	if (!mlx4_is_slave(dev))
		mlx4_cleanup_mcg_table(dev);

err_mr_table_free:
	mlx4_cleanup_mr_table(dev);

err_xrcd_table_free:
	mlx4_cleanup_xrcd_table(dev);

err_pd_table_free:
	mlx4_cleanup_pd_table(dev);

err_kar_unmap:
	iounmap(priv->kar);

err_uar_free:
	mlx4_uar_free(dev, &priv->driver_uar);

err_uar_table_free:
	mlx4_cleanup_uar_table(dev);
	return err;
}

static void mlx4_enable_msi_x(struct mlx4_dev *dev)
{
	struct mlx4_priv *priv = mlx4_priv(dev);
	struct msix_entry *entries;
	int i;

	if (msi_x) {
		int nreq = dev->caps.num_ports * num_online_cpus() + MSIX_LEGACY_SZ;

		nreq = min_t(int, dev->caps.num_eqs - dev->caps.reserved_eqs,
			     nreq);

		entries = kcalloc(nreq, sizeof *entries, GFP_KERNEL);
		if (!entries)
			goto no_msi;

		for (i = 0; i < nreq; ++i)
			entries[i].entry = i;

		nreq = pci_enable_msix_range(dev->pdev, entries, 2, nreq);

		if (nreq < 0) {
			kfree(entries);
			goto no_msi;
		} else if (nreq < MSIX_LEGACY_SZ +
			   dev->caps.num_ports * MIN_MSIX_P_PORT) {
			/*Working in legacy mode , all EQ's shared*/
			dev->caps.comp_pool           = 0;
			dev->caps.num_comp_vectors = nreq - 1;
		} else {
			dev->caps.comp_pool           = nreq - MSIX_LEGACY_SZ;
			dev->caps.num_comp_vectors = MSIX_LEGACY_SZ - 1;
		}
		for (i = 0; i < nreq; ++i)
			priv->eq_table.eq[i].irq = entries[i].vector;

		dev->flags |= MLX4_FLAG_MSI_X;

		kfree(entries);
		return;
	}

no_msi:
	dev->caps.num_comp_vectors = 1;
	dev->caps.comp_pool	   = 0;

	for (i = 0; i < 2; ++i)
		priv->eq_table.eq[i].irq = dev->pdev->irq;
}

static int mlx4_init_port_info(struct mlx4_dev *dev, int port)
{
	struct mlx4_port_info *info = &mlx4_priv(dev)->port[port];
	int err = 0;

	info->dev = dev;
	info->port = port;
	if (!mlx4_is_slave(dev)) {
		mlx4_init_mac_table(dev, &info->mac_table);
		mlx4_init_vlan_table(dev, &info->vlan_table);
		mlx4_init_roce_gid_table(dev, &info->gid_table);
		info->base_qpn = mlx4_get_base_qpn(dev, port);
	}

	sprintf(info->dev_name, "mlx4_port%d", port);
	info->port_attr.attr.name = info->dev_name;
	if (mlx4_is_mfunc(dev))
		info->port_attr.attr.mode = S_IRUGO;
	else {
		info->port_attr.attr.mode = S_IRUGO | S_IWUSR;
		info->port_attr.store     = set_port_type;
	}
	info->port_attr.show      = show_port_type;
	sysfs_attr_init(&info->port_attr.attr);

	err = device_create_file(&dev->pdev->dev, &info->port_attr);
	if (err) {
		mlx4_err(dev, "Failed to create file for port %d\n", port);
		info->port = -1;
	}

	sprintf(info->dev_mtu_name, "mlx4_port%d_mtu", port);
	info->port_mtu_attr.attr.name = info->dev_mtu_name;
	if (mlx4_is_mfunc(dev))
		info->port_mtu_attr.attr.mode = S_IRUGO;
	else {
		info->port_mtu_attr.attr.mode = S_IRUGO | S_IWUSR;
		info->port_mtu_attr.store     = set_port_ib_mtu;
	}
	info->port_mtu_attr.show      = show_port_ib_mtu;
	sysfs_attr_init(&info->port_mtu_attr.attr);

	err = device_create_file(&dev->pdev->dev, &info->port_mtu_attr);
	if (err) {
		mlx4_err(dev, "Failed to create mtu file for port %d\n", port);
		device_remove_file(&info->dev->pdev->dev, &info->port_attr);
		info->port = -1;
	}

	return err;
}

static void mlx4_cleanup_port_info(struct mlx4_port_info *info)
{
	if (info->port < 0)
		return;

	device_remove_file(&info->dev->pdev->dev, &info->port_attr);
	device_remove_file(&info->dev->pdev->dev, &info->port_mtu_attr);
}

static int mlx4_init_steering(struct mlx4_dev *dev)
{
	struct mlx4_priv *priv = mlx4_priv(dev);
	int num_entries = dev->caps.num_ports;
	int i, j;

	priv->steer = kzalloc(sizeof(struct mlx4_steer) * num_entries, GFP_KERNEL);
	if (!priv->steer)
		return -ENOMEM;

	for (i = 0; i < num_entries; i++)
		for (j = 0; j < MLX4_NUM_STEERS; j++) {
			INIT_LIST_HEAD(&priv->steer[i].promisc_qps[j]);
			INIT_LIST_HEAD(&priv->steer[i].steer_entries[j]);
		}
	return 0;
}

static void mlx4_clear_steering(struct mlx4_dev *dev)
{
	struct mlx4_priv *priv = mlx4_priv(dev);
	struct mlx4_steer_index *entry, *tmp_entry;
	struct mlx4_promisc_qp *pqp, *tmp_pqp;
	int num_entries = dev->caps.num_ports;
	int i, j;

	for (i = 0; i < num_entries; i++) {
		for (j = 0; j < MLX4_NUM_STEERS; j++) {
			list_for_each_entry_safe(pqp, tmp_pqp,
						 &priv->steer[i].promisc_qps[j],
						 list) {
				list_del(&pqp->list);
				kfree(pqp);
			}
			list_for_each_entry_safe(entry, tmp_entry,
						 &priv->steer[i].steer_entries[j],
						 list) {
				list_del(&entry->list);
				list_for_each_entry_safe(pqp, tmp_pqp,
							 &entry->duplicates,
							 list) {
					list_del(&pqp->list);
					kfree(pqp);
				}
				kfree(entry);
			}
		}
	}
	kfree(priv->steer);
}

static int extended_func_num(struct pci_dev *pdev)
{
	return PCI_SLOT(pdev->devfn) * 8 + PCI_FUNC(pdev->devfn);
}

#define MLX4_OWNER_BASE	0x8069c
#define MLX4_OWNER_SIZE	4

static int mlx4_get_ownership(struct mlx4_dev *dev)
{
	void __iomem *owner;
	u32 ret;

	if (pci_channel_offline(dev->pdev))
		return -EIO;

	owner = ioremap(pci_resource_start(dev->pdev, 0) + MLX4_OWNER_BASE,
			MLX4_OWNER_SIZE);
	if (!owner) {
		mlx4_err(dev, "Failed to obtain ownership bit\n");
		return -ENOMEM;
	}

	ret = readl(owner);
	iounmap(owner);
	return (int) !!ret;
}

static void mlx4_free_ownership(struct mlx4_dev *dev)
{
	void __iomem *owner;

	if (pci_channel_offline(dev->pdev))
		return;

	owner = ioremap(pci_resource_start(dev->pdev, 0) + MLX4_OWNER_BASE,
			MLX4_OWNER_SIZE);
	if (!owner) {
		mlx4_err(dev, "Failed to obtain ownership bit\n");
		return;
	}
	writel(0, owner);
	msleep(1000);
	iounmap(owner);
}

#define SRIOV_VALID_STATE(flags) (!!((flags) & MLX4_FLAG_SRIOV)	==\
				  !!((flags) & MLX4_FLAG_MASTER))

static u64 mlx4_enable_sriov(struct mlx4_dev *dev, struct pci_dev *pdev,
			     u8 total_vfs, int existing_vfs)
{
	u64 dev_flags = dev->flags;

	dev->dev_vfs = kzalloc(
			total_vfs * sizeof(*dev->dev_vfs),
			GFP_KERNEL);
	if (NULL == dev->dev_vfs) {
		mlx4_err(dev, "Failed to allocate memory for VFs\n");
		goto disable_sriov;
	} else if (!(dev->flags &  MLX4_FLAG_SRIOV)) {
		int err = 0;

		atomic_inc(&pf_loading);
		if (existing_vfs) {
			if (existing_vfs != total_vfs)
				mlx4_err(dev, "SR-IOV was already enabled, but with num_vfs (%d) different than requested (%d)\n",
					 existing_vfs, total_vfs);
		} else {
			mlx4_warn(dev, "Enabling SR-IOV with %d VFs\n", total_vfs);
			err = pci_enable_sriov(pdev, total_vfs);
		}
		if (err) {
			mlx4_err(dev, "Failed to enable SR-IOV, continuing without SR-IOV (err = %d)\n",
				 err);
			atomic_dec(&pf_loading);
			goto disable_sriov;
		} else {
			mlx4_warn(dev, "Running in master mode\n");
			dev_flags |= MLX4_FLAG_SRIOV |
				MLX4_FLAG_MASTER;
			dev_flags &= ~MLX4_FLAG_SLAVE;
			dev->num_vfs = total_vfs;
		}
	}
	return dev_flags;

disable_sriov:
	dev->num_vfs = 0;
	kfree(dev->dev_vfs);
	return dev_flags & ~MLX4_FLAG_MASTER;
}

enum {
	MLX4_DEV_CAP_CHECK_NUM_VFS_ABOVE_64 = -1,
};

static int mlx4_check_dev_cap(struct mlx4_dev *dev, struct mlx4_dev_cap *dev_cap,
			      int *nvfs)
{
	int requested_vfs = nvfs[0] + nvfs[1] + nvfs[2];
	/* Checking for 64 VFs as a limitation of CX2 */
	if (!(dev_cap->flags2 & MLX4_DEV_CAP_FLAG2_80_VFS) &&
	    requested_vfs >= 64) {
		mlx4_err(dev, "Requested %d VFs, but FW does not support more than 64\n",
			 requested_vfs);
		return MLX4_DEV_CAP_CHECK_NUM_VFS_ABOVE_64;
	}
	return 0;
}

static int mlx4_load_one(struct pci_dev *pdev, int pci_dev_data,
			 int total_vfs, int *nvfs, struct mlx4_priv *priv)
{
	struct mlx4_dev *dev;
	unsigned sum = 0;
	int err;
	int port;
	int i;
	struct mlx4_dev_cap *dev_cap = NULL;
	int existing_vfs = 0;

	dev = &priv->dev;

	INIT_LIST_HEAD(&priv->ctx_list);
	spin_lock_init(&priv->ctx_lock);

	mutex_init(&priv->port_mutex);

	INIT_LIST_HEAD(&priv->pgdir_list);
	mutex_init(&priv->pgdir_mutex);

	INIT_LIST_HEAD(&priv->bf_list);
	mutex_init(&priv->bf_mutex);

	dev->rev_id = pdev->revision;
	dev->numa_node = dev_to_node(&pdev->dev);

	/* Detect if this device is a virtual function */
	if (pci_dev_data & MLX4_PCI_DEV_IS_VF) {
		mlx4_warn(dev, "Detected virtual function - running in slave mode\n");
		dev->flags |= MLX4_FLAG_SLAVE;
	} else {
		/* We reset the device and enable SRIOV only for physical
		 * devices.  Try to claim ownership on the device;
		 * if already taken, skip -- do not allow multiple PFs */
		err = mlx4_get_ownership(dev);
		if (err) {
			if (err < 0)
				return err;
			else {
				mlx4_warn(dev, "Multiple PFs not yet supported - Skipping PF\n");
				return -EINVAL;
			}
		}

		atomic_set(&priv->opreq_count, 0);
		INIT_WORK(&priv->opreq_task, mlx4_opreq_action);

		/*
		 * Now reset the HCA before we touch the PCI capabilities or
		 * attempt a firmware command, since a boot ROM may have left
		 * the HCA in an undefined state.
		 */
		err = mlx4_reset(dev);
		if (err) {
			mlx4_err(dev, "Failed to reset HCA, aborting\n");
			goto err_sriov;
		}

		if (total_vfs) {
			existing_vfs = pci_num_vf(pdev);
			dev->flags = MLX4_FLAG_MASTER;
			dev->num_vfs = total_vfs;
		}
	}

slave_start:
	err = mlx4_cmd_init(dev);
	if (err) {
		mlx4_err(dev, "Failed to init command interface, aborting\n");
		goto err_sriov;
	}

	/* In slave functions, the communication channel must be initialized
	 * before posting commands. Also, init num_slaves before calling
	 * mlx4_init_hca */
	if (mlx4_is_mfunc(dev)) {
		if (mlx4_is_master(dev)) {
			dev->num_slaves = MLX4_MAX_NUM_SLAVES;

		} else {
			dev->num_slaves = 0;
			err = mlx4_multi_func_init(dev);
			if (err) {
				mlx4_err(dev, "Failed to init slave mfunc interface, aborting\n");
				goto err_cmd;
			}
		}
	}

	err = mlx4_init_fw(dev);
	if (err) {
		mlx4_err(dev, "Failed to init fw, aborting.\n");
		goto err_mfunc;
	}

	if (mlx4_is_master(dev)) {
		if (!dev_cap) {
			dev_cap = kzalloc(sizeof(*dev_cap), GFP_KERNEL);

			if (!dev_cap) {
				err = -ENOMEM;
				goto err_fw;
			}

			err = mlx4_QUERY_DEV_CAP(dev, dev_cap);
			if (err) {
				mlx4_err(dev, "QUERY_DEV_CAP command failed, aborting.\n");
				goto err_fw;
			}

			if (mlx4_check_dev_cap(dev, dev_cap, nvfs))
				goto err_fw;

			if (!(dev_cap->flags2 & MLX4_DEV_CAP_FLAG2_SYS_EQS)) {
				u64 dev_flags = mlx4_enable_sriov(dev, pdev, total_vfs,
								  existing_vfs);

				mlx4_cmd_cleanup(dev, MLX4_CMD_CLEANUP_ALL);
				dev->flags = dev_flags;
				if (!SRIOV_VALID_STATE(dev->flags)) {
					mlx4_err(dev, "Invalid SRIOV state\n");
					goto err_sriov;
				}
				err = mlx4_reset(dev);
				if (err) {
					mlx4_err(dev, "Failed to reset HCA, aborting.\n");
					goto err_sriov;
				}
				goto slave_start;
			}
		} else {
			/* Legacy mode FW requires SRIOV to be enabled before
			 * doing QUERY_DEV_CAP, since max_eq's value is different if
			 * SRIOV is enabled.
			 */
			memset(dev_cap, 0, sizeof(*dev_cap));
			err = mlx4_QUERY_DEV_CAP(dev, dev_cap);
			if (err) {
				mlx4_err(dev, "QUERY_DEV_CAP command failed, aborting.\n");
				goto err_fw;
			}

			if (mlx4_check_dev_cap(dev, dev_cap, nvfs))
				goto err_fw;
		}
	}

	err = mlx4_init_hca(dev);
	if (err) {
		if (err == -EACCES) {
			/* Not primary Physical function
			 * Running in slave mode */
			mlx4_cmd_cleanup(dev, MLX4_CMD_CLEANUP_ALL);
			/* We're not a PF */
			if (dev->flags & MLX4_FLAG_SRIOV) {
				if (!existing_vfs)
					pci_disable_sriov(pdev);
				if (mlx4_is_master(dev))
					atomic_dec(&pf_loading);
				dev->flags &= ~MLX4_FLAG_SRIOV;
			}
			if (!mlx4_is_slave(dev))
				mlx4_free_ownership(dev);
			dev->flags |= MLX4_FLAG_SLAVE;
			dev->flags &= ~MLX4_FLAG_MASTER;
			goto slave_start;
		} else
			goto err_fw;
	}

	if (mlx4_is_master(dev) && (dev_cap->flags2 & MLX4_DEV_CAP_FLAG2_SYS_EQS)) {
		u64 dev_flags = mlx4_enable_sriov(dev, pdev, total_vfs, existing_vfs);

		if ((dev->flags ^ dev_flags) & (MLX4_FLAG_MASTER | MLX4_FLAG_SLAVE)) {
			mlx4_cmd_cleanup(dev, MLX4_CMD_CLEANUP_VHCR);
			dev->flags = dev_flags;
			err = mlx4_cmd_init(dev);
			if (err) {
				/* Only VHCR is cleaned up, so could still
				 * send FW commands
				 */
				mlx4_err(dev, "Failed to init VHCR command interface, aborting\n");
				goto err_close;
			}
		} else {
			dev->flags = dev_flags;
		}

		if (!SRIOV_VALID_STATE(dev->flags)) {
			mlx4_err(dev, "Invalid SRIOV state\n");
			goto err_close;
		}
	}

	/* check if the device is functioning at its maximum possible speed.
	 * No return code for this call, just warn the user in case of PCI
	 * express device capabilities are under-satisfied by the bus.
	 */
	if (!mlx4_is_slave(dev))
		mlx4_check_pcie_caps(dev);

	/* In master functions, the communication channel must be initialized
	 * after obtaining its address from fw */
	if (mlx4_is_master(dev)) {
		int ib_ports = 0;

		mlx4_foreach_port(i, dev, MLX4_PORT_TYPE_IB)
			ib_ports++;

		if (ib_ports &&
		    (num_vfs_argc > 1 || probe_vfs_argc > 1)) {
			mlx4_err(dev,
				 "Invalid syntax of num_vfs/probe_vfs with IB port - single port VFs syntax is only supported when all ports are configured as ethernet\n");
			err = -EINVAL;
			goto err_close;
		}
		if (dev->caps.num_ports < 2 &&
		    num_vfs_argc > 1) {
			err = -EINVAL;
			mlx4_err(dev,
				 "Error: Trying to configure VFs on port 2, but HCA has only %d physical ports\n",
				 dev->caps.num_ports);
			goto err_close;
		}
		memcpy(dev->nvfs, nvfs, sizeof(dev->nvfs));

		for (i = 0; i < sizeof(dev->nvfs)/sizeof(dev->nvfs[0]); i++) {
			unsigned j;

			for (j = 0; j < dev->nvfs[i]; ++sum, ++j) {
				dev->dev_vfs[sum].min_port = i < 2 ? i + 1 : 1;
				dev->dev_vfs[sum].n_ports = i < 2 ? 1 :
					dev->caps.num_ports;
			}
		}

		/* In master functions, the communication channel
		 * must be initialized after obtaining its address from fw
		 */
		err = mlx4_multi_func_init(dev);
		if (err) {
			mlx4_err(dev, "Failed to init master mfunc interface, aborting.\n");
			goto err_close;
		}
	}

	err = mlx4_alloc_eq_table(dev);
	if (err)
		goto err_master_mfunc;

	priv->msix_ctl.pool_bm = 0;
	mutex_init(&priv->msix_ctl.pool_lock);

	mlx4_enable_msi_x(dev);
	if ((mlx4_is_mfunc(dev)) &&
	    !(dev->flags & MLX4_FLAG_MSI_X)) {
		err = -ENOSYS;
		mlx4_err(dev, "INTx is not supported in multi-function mode, aborting\n");
		goto err_free_eq;
	}

	if (!mlx4_is_slave(dev)) {
		err = mlx4_init_steering(dev);
		if (err)
			goto err_disable_msix;
	}

	err = mlx4_setup_hca(dev);
	if (err == -EBUSY && (dev->flags & MLX4_FLAG_MSI_X) &&
	    !mlx4_is_mfunc(dev)) {
		dev->flags &= ~MLX4_FLAG_MSI_X;
		dev->caps.num_comp_vectors = 1;
		dev->caps.comp_pool	   = 0;
		pci_disable_msix(pdev);
		err = mlx4_setup_hca(dev);
	}

	if (err)
		goto err_steer;

	mlx4_init_quotas(dev);

	for (port = 1; port <= dev->caps.num_ports; port++) {
		err = mlx4_init_port_info(dev, port);
		if (err)
			goto err_port;
	}

	err = mlx4_register_device(dev);
	if (err)
		goto err_port;

	mlx4_request_modules(dev);

	mlx4_sense_init(dev);
	mlx4_start_sense(dev);

	priv->removed = 0;

	if (mlx4_is_master(dev) && dev->num_vfs)
		atomic_dec(&pf_loading);

	return 0;

err_port:
	for (--port; port >= 1; --port)
		mlx4_cleanup_port_info(&priv->port[port]);

	mlx4_cleanup_counters_table(dev);
	mlx4_cleanup_qp_table(dev);
	mlx4_cleanup_srq_table(dev);
	mlx4_cleanup_cq_table(dev);
	mlx4_cmd_use_polling(dev);
	mlx4_cleanup_eq_table(dev);
	mlx4_cleanup_mcg_table(dev);
	mlx4_cleanup_mr_table(dev);
	mlx4_cleanup_xrcd_table(dev);
	mlx4_cleanup_pd_table(dev);
	mlx4_cleanup_uar_table(dev);

err_steer:
	if (!mlx4_is_slave(dev))
		mlx4_clear_steering(dev);

err_disable_msix:
	if (dev->flags & MLX4_FLAG_MSI_X)
		pci_disable_msix(pdev);

err_free_eq:
	mlx4_free_eq_table(dev);

err_master_mfunc:
	if (mlx4_is_master(dev))
		mlx4_multi_func_cleanup(dev);

	if (mlx4_is_slave(dev)) {
		kfree(dev->caps.qp0_qkey);
		kfree(dev->caps.qp0_tunnel);
		kfree(dev->caps.qp0_proxy);
		kfree(dev->caps.qp1_tunnel);
		kfree(dev->caps.qp1_proxy);
	}

err_close:
	mlx4_close_hca(dev);

err_fw:
	mlx4_close_fw(dev);

err_mfunc:
	if (mlx4_is_slave(dev))
		mlx4_multi_func_cleanup(dev);

err_cmd:
	mlx4_cmd_cleanup(dev, MLX4_CMD_CLEANUP_ALL);

err_sriov:
	if (dev->flags & MLX4_FLAG_SRIOV && !existing_vfs)
		pci_disable_sriov(pdev);

	if (mlx4_is_master(dev) && dev->num_vfs)
		atomic_dec(&pf_loading);

	kfree(priv->dev.dev_vfs);

	if (!mlx4_is_slave(dev))
		mlx4_free_ownership(dev);

	kfree(dev_cap);
	return err;
}

static int __mlx4_init_one(struct pci_dev *pdev, int pci_dev_data,
			   struct mlx4_priv *priv)
{
	int err;
	int nvfs[MLX4_MAX_PORTS + 1] = {0, 0, 0};
	int prb_vf[MLX4_MAX_PORTS + 1] = {0, 0, 0};
	const int param_map[MLX4_MAX_PORTS + 1][MLX4_MAX_PORTS + 1] = {
		{2, 0, 0}, {0, 1, 2}, {0, 1, 2} };
	unsigned total_vfs = 0;
	unsigned int i;

	pr_info(DRV_NAME ": Initializing %s\n", pci_name(pdev));

	err = pci_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "Cannot enable PCI device, aborting\n");
		return err;
	}

	/* Due to requirement that all VFs and the PF are *guaranteed* 2 MACS
	 * per port, we must limit the number of VFs to 63 (since their are
	 * 128 MACs)
	 */
	for (i = 0; i < sizeof(nvfs)/sizeof(nvfs[0]) && i < num_vfs_argc;
	     total_vfs += nvfs[param_map[num_vfs_argc - 1][i]], i++) {
		nvfs[param_map[num_vfs_argc - 1][i]] = num_vfs[i];
		if (nvfs[i] < 0) {
			dev_err(&pdev->dev, "num_vfs module parameter cannot be negative\n");
			err = -EINVAL;
			goto err_disable_pdev;
		}
	}
	for (i = 0; i < sizeof(prb_vf)/sizeof(prb_vf[0]) && i < probe_vfs_argc;
	     i++) {
		prb_vf[param_map[probe_vfs_argc - 1][i]] = probe_vf[i];
		if (prb_vf[i] < 0 || prb_vf[i] > nvfs[i]) {
			dev_err(&pdev->dev, "probe_vf module parameter cannot be negative or greater than num_vfs\n");
			err = -EINVAL;
			goto err_disable_pdev;
		}
	}
	if (total_vfs >= MLX4_MAX_NUM_VF) {
		dev_err(&pdev->dev,
			"Requested more VF's (%d) than allowed (%d)\n",
			total_vfs, MLX4_MAX_NUM_VF - 1);
		err = -EINVAL;
		goto err_disable_pdev;
	}

	for (i = 0; i < MLX4_MAX_PORTS; i++) {
		if (nvfs[i] + nvfs[2] >= MLX4_MAX_NUM_VF_P_PORT) {
			dev_err(&pdev->dev,
				"Requested more VF's (%d) for port (%d) than allowed (%d)\n",
				nvfs[i] + nvfs[2], i + 1,
				MLX4_MAX_NUM_VF_P_PORT - 1);
			err = -EINVAL;
			goto err_disable_pdev;
		}
	}

	/* Check for BARs. */
	if (!(pci_dev_data & MLX4_PCI_DEV_IS_VF) &&
	    !(pci_resource_flags(pdev, 0) & IORESOURCE_MEM)) {
		dev_err(&pdev->dev, "Missing DCS, aborting (driver_data: 0x%x, pci_resource_flags(pdev, 0):0x%lx)\n",
			pci_dev_data, pci_resource_flags(pdev, 0));
		err = -ENODEV;
		goto err_disable_pdev;
	}
	if (!(pci_resource_flags(pdev, 2) & IORESOURCE_MEM)) {
		dev_err(&pdev->dev, "Missing UAR, aborting\n");
		err = -ENODEV;
		goto err_disable_pdev;
	}

	err = pci_request_regions(pdev, DRV_NAME);
	if (err) {
		dev_err(&pdev->dev, "Couldn't get PCI resources, aborting\n");
		goto err_disable_pdev;
	}

	pci_set_master(pdev);

	err = pci_set_dma_mask(pdev, DMA_BIT_MASK(64));
	if (err) {
		dev_warn(&pdev->dev, "Warning: couldn't set 64-bit PCI DMA mask\n");
		err = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));
		if (err) {
			dev_err(&pdev->dev, "Can't set PCI DMA mask, aborting\n");
			goto err_release_regions;
		}
	}
	err = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64));
	if (err) {
		dev_warn(&pdev->dev, "Warning: couldn't set 64-bit consistent PCI DMA mask\n");
		err = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32));
		if (err) {
			dev_err(&pdev->dev, "Can't set consistent PCI DMA mask, aborting\n");
			goto err_release_regions;
		}
	}

	/* Allow large DMA segments, up to the firmware limit of 1 GB */
	dma_set_max_seg_size(&pdev->dev, 1024 * 1024 * 1024);
	/* Detect if this device is a virtual function */
	if (pci_dev_data & MLX4_PCI_DEV_IS_VF) {
		/* When acting as pf, we normally skip vfs unless explicitly
		 * requested to probe them.
		 */
		if (total_vfs) {
			unsigned vfs_offset = 0;

			for (i = 0; i < sizeof(nvfs)/sizeof(nvfs[0]) &&
			     vfs_offset + nvfs[i] < extended_func_num(pdev);
			     vfs_offset += nvfs[i], i++)
				;
			if (i == sizeof(nvfs)/sizeof(nvfs[0])) {
				err = -ENODEV;
				goto err_release_regions;
			}
			if ((extended_func_num(pdev) - vfs_offset)
			    > prb_vf[i]) {
				dev_warn(&pdev->dev, "Skipping virtual function:%d\n",
					 extended_func_num(pdev));
				err = -ENODEV;
				goto err_release_regions;
			}
		}
	}

	err = mlx4_load_one(pdev, pci_dev_data, total_vfs, nvfs, priv);
	if (err)
		goto err_release_regions;
	return 0;

err_release_regions:
	pci_release_regions(pdev);

err_disable_pdev:
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	return err;
}

static int mlx4_init_one(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct mlx4_priv *priv;
	struct mlx4_dev *dev;
	int ret;

	printk_once(KERN_INFO "%s", mlx4_version);

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	dev       = &priv->dev;
	dev->pdev = pdev;
	pci_set_drvdata(pdev, dev);
	priv->pci_dev_data = id->driver_data;

	ret =  __mlx4_init_one(pdev, id->driver_data, priv);
	if (ret)
		kfree(priv);

	return ret;
}

static void mlx4_unload_one(struct pci_dev *pdev)
{
	struct mlx4_dev  *dev  = pci_get_drvdata(pdev);
	struct mlx4_priv *priv = mlx4_priv(dev);
	int               pci_dev_data;
	int p;
	int active_vfs = 0;

	if (priv->removed)
		return;

	pci_dev_data = priv->pci_dev_data;

	/* Disabling SR-IOV is not allowed while there are active vf's */
	if (mlx4_is_master(dev)) {
		active_vfs = mlx4_how_many_lives_vf(dev);
		if (active_vfs) {
			pr_warn("Removing PF when there are active VF's !!\n");
			pr_warn("Will not disable SR-IOV.\n");
		}
	}
	mlx4_stop_sense(dev);
	mlx4_unregister_device(dev);

	for (p = 1; p <= dev->caps.num_ports; p++) {
		mlx4_cleanup_port_info(&priv->port[p]);
		mlx4_CLOSE_PORT(dev, p);
	}

	if (mlx4_is_master(dev))
		mlx4_free_resource_tracker(dev,
					   RES_TR_FREE_SLAVES_ONLY);

	mlx4_cleanup_counters_table(dev);
	mlx4_cleanup_qp_table(dev);
	mlx4_cleanup_srq_table(dev);
	mlx4_cleanup_cq_table(dev);
	mlx4_cmd_use_polling(dev);
	mlx4_cleanup_eq_table(dev);
	mlx4_cleanup_mcg_table(dev);
	mlx4_cleanup_mr_table(dev);
	mlx4_cleanup_xrcd_table(dev);
	mlx4_cleanup_pd_table(dev);

	if (mlx4_is_master(dev))
		mlx4_free_resource_tracker(dev,
					   RES_TR_FREE_STRUCTS_ONLY);

	iounmap(priv->kar);
	mlx4_uar_free(dev, &priv->driver_uar);
	mlx4_cleanup_uar_table(dev);
	if (!mlx4_is_slave(dev))
		mlx4_clear_steering(dev);
	mlx4_free_eq_table(dev);
	if (mlx4_is_master(dev))
		mlx4_multi_func_cleanup(dev);
	mlx4_close_hca(dev);
	mlx4_close_fw(dev);
	if (mlx4_is_slave(dev))
		mlx4_multi_func_cleanup(dev);
	mlx4_cmd_cleanup(dev, MLX4_CMD_CLEANUP_ALL);

	if (dev->flags & MLX4_FLAG_MSI_X)
		pci_disable_msix(pdev);
	if (dev->flags & MLX4_FLAG_SRIOV && !active_vfs) {
		mlx4_warn(dev, "Disabling SR-IOV\n");
		pci_disable_sriov(pdev);
		dev->flags &= ~MLX4_FLAG_SRIOV;
		dev->num_vfs = 0;
	}

	if (!mlx4_is_slave(dev))
		mlx4_free_ownership(dev);

	kfree(dev->caps.qp0_qkey);
	kfree(dev->caps.qp0_tunnel);
	kfree(dev->caps.qp0_proxy);
	kfree(dev->caps.qp1_tunnel);
	kfree(dev->caps.qp1_proxy);
	kfree(dev->dev_vfs);

	memset(priv, 0, sizeof(*priv));
	priv->pci_dev_data = pci_dev_data;
	priv->removed = 1;
}

static void mlx4_remove_one(struct pci_dev *pdev)
{
	struct mlx4_dev  *dev  = pci_get_drvdata(pdev);
	struct mlx4_priv *priv = mlx4_priv(dev);

	mlx4_unload_one(pdev);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	kfree(priv);
	pci_set_drvdata(pdev, NULL);
}

int mlx4_restart_one(struct pci_dev *pdev)
{
	struct mlx4_dev	 *dev  = pci_get_drvdata(pdev);
	struct mlx4_priv *priv = mlx4_priv(dev);
	int nvfs[MLX4_MAX_PORTS + 1] = {0, 0, 0};
	int pci_dev_data, err, total_vfs;

	pci_dev_data = priv->pci_dev_data;
	total_vfs = dev->num_vfs;
	memcpy(nvfs, dev->nvfs, sizeof(dev->nvfs));

	mlx4_unload_one(pdev);
	err = mlx4_load_one(pdev, pci_dev_data, total_vfs, nvfs, priv);
	if (err) {
		mlx4_err(dev, "%s: ERROR: mlx4_load_one failed, pci_name=%s, err=%d\n",
			 __func__, pci_name(pdev), err);
		return err;
	}

	return err;
}

static const struct pci_device_id mlx4_pci_table[] = {
	/* MT25408 "Hermon" SDR */
	{ PCI_VDEVICE(MELLANOX, 0x6340), MLX4_PCI_DEV_FORCE_SENSE_PORT },
	/* MT25408 "Hermon" DDR */
	{ PCI_VDEVICE(MELLANOX, 0x634a), MLX4_PCI_DEV_FORCE_SENSE_PORT },
	/* MT25408 "Hermon" QDR */
	{ PCI_VDEVICE(MELLANOX, 0x6354), MLX4_PCI_DEV_FORCE_SENSE_PORT },
	/* MT25408 "Hermon" DDR PCIe gen2 */
	{ PCI_VDEVICE(MELLANOX, 0x6732), MLX4_PCI_DEV_FORCE_SENSE_PORT },
	/* MT25408 "Hermon" QDR PCIe gen2 */
	{ PCI_VDEVICE(MELLANOX, 0x673c), MLX4_PCI_DEV_FORCE_SENSE_PORT },
	/* MT25408 "Hermon" EN 10GigE */
	{ PCI_VDEVICE(MELLANOX, 0x6368), MLX4_PCI_DEV_FORCE_SENSE_PORT },
	/* MT25408 "Hermon" EN 10GigE PCIe gen2 */
	{ PCI_VDEVICE(MELLANOX, 0x6750), MLX4_PCI_DEV_FORCE_SENSE_PORT },
	/* MT25458 ConnectX EN 10GBASE-T 10GigE */
	{ PCI_VDEVICE(MELLANOX, 0x6372), MLX4_PCI_DEV_FORCE_SENSE_PORT },
	/* MT25458 ConnectX EN 10GBASE-T+Gen2 10GigE */
	{ PCI_VDEVICE(MELLANOX, 0x675a), MLX4_PCI_DEV_FORCE_SENSE_PORT },
	/* MT26468 ConnectX EN 10GigE PCIe gen2*/
	{ PCI_VDEVICE(MELLANOX, 0x6764), MLX4_PCI_DEV_FORCE_SENSE_PORT },
	/* MT26438 ConnectX EN 40GigE PCIe gen2 5GT/s */
	{ PCI_VDEVICE(MELLANOX, 0x6746), MLX4_PCI_DEV_FORCE_SENSE_PORT },
	/* MT26478 ConnectX2 40GigE PCIe gen2 */
	{ PCI_VDEVICE(MELLANOX, 0x676e), MLX4_PCI_DEV_FORCE_SENSE_PORT },
	/* MT25400 Family [ConnectX-2 Virtual Function] */
	{ PCI_VDEVICE(MELLANOX, 0x1002), MLX4_PCI_DEV_IS_VF },
	/* MT27500 Family [ConnectX-3] */
	{ PCI_VDEVICE(MELLANOX, 0x1003), 0 },
	/* MT27500 Family [ConnectX-3 Virtual Function] */
	{ PCI_VDEVICE(MELLANOX, 0x1004), MLX4_PCI_DEV_IS_VF },
	{ PCI_VDEVICE(MELLANOX, 0x1005), 0 }, /* MT27510 Family */
	{ PCI_VDEVICE(MELLANOX, 0x1006), 0 }, /* MT27511 Family */
	{ PCI_VDEVICE(MELLANOX, 0x1007), 0 }, /* MT27520 Family */
	{ PCI_VDEVICE(MELLANOX, 0x1008), 0 }, /* MT27521 Family */
	{ PCI_VDEVICE(MELLANOX, 0x1009), 0 }, /* MT27530 Family */
	{ PCI_VDEVICE(MELLANOX, 0x100a), 0 }, /* MT27531 Family */
	{ PCI_VDEVICE(MELLANOX, 0x100b), 0 }, /* MT27540 Family */
	{ PCI_VDEVICE(MELLANOX, 0x100c), 0 }, /* MT27541 Family */
	{ PCI_VDEVICE(MELLANOX, 0x100d), 0 }, /* MT27550 Family */
	{ PCI_VDEVICE(MELLANOX, 0x100e), 0 }, /* MT27551 Family */
	{ PCI_VDEVICE(MELLANOX, 0x100f), 0 }, /* MT27560 Family */
	{ PCI_VDEVICE(MELLANOX, 0x1010), 0 }, /* MT27561 Family */
	{ 0, }
};

MODULE_DEVICE_TABLE(pci, mlx4_pci_table);

static pci_ers_result_t mlx4_pci_err_detected(struct pci_dev *pdev,
					      pci_channel_state_t state)
{
	mlx4_unload_one(pdev);

	return state == pci_channel_io_perm_failure ?
		PCI_ERS_RESULT_DISCONNECT : PCI_ERS_RESULT_NEED_RESET;
}

static pci_ers_result_t mlx4_pci_slot_reset(struct pci_dev *pdev)
{
	struct mlx4_dev	 *dev  = pci_get_drvdata(pdev);
	struct mlx4_priv *priv = mlx4_priv(dev);
	int               ret;

	ret = __mlx4_init_one(pdev, priv->pci_dev_data, priv);

	return ret ? PCI_ERS_RESULT_DISCONNECT : PCI_ERS_RESULT_RECOVERED;
}

static const struct pci_error_handlers mlx4_err_handler = {
	.error_detected = mlx4_pci_err_detected,
	.slot_reset     = mlx4_pci_slot_reset,
};

static struct pci_driver mlx4_driver = {
	.name		= DRV_NAME,
	.id_table	= mlx4_pci_table,
	.probe		= mlx4_init_one,
	.shutdown	= mlx4_unload_one,
	.remove		= mlx4_remove_one,
	.err_handler    = &mlx4_err_handler,
};

static int __init mlx4_verify_params(void)
{
	if ((log_num_mac < 0) || (log_num_mac > 7)) {
		pr_warn("mlx4_core: bad num_mac: %d\n", log_num_mac);
		return -1;
	}

	if (log_num_vlan != 0)
		pr_warn("mlx4_core: log_num_vlan - obsolete module param, using %d\n",
			MLX4_LOG_NUM_VLANS);

	if (use_prio != 0)
		pr_warn("mlx4_core: use_prio - obsolete module param, ignored\n");

	if ((log_mtts_per_seg < 1) || (log_mtts_per_seg > 7)) {
		pr_warn("mlx4_core: bad log_mtts_per_seg: %d\n",
			log_mtts_per_seg);
		return -1;
	}

	/* Check if module param for ports type has legal combination */
	if (port_type_array[0] == false && port_type_array[1] == true) {
		pr_warn("Module parameter configuration ETH/IB is not supported. Switching to default configuration IB/IB\n");
		port_type_array[0] = true;
	}

	if (mlx4_log_num_mgm_entry_size < -7 ||
	    (mlx4_log_num_mgm_entry_size > 0 &&
	     (mlx4_log_num_mgm_entry_size < MLX4_MIN_MGM_LOG_ENTRY_SIZE ||
	      mlx4_log_num_mgm_entry_size > MLX4_MAX_MGM_LOG_ENTRY_SIZE))) {
		pr_warn("mlx4_core: mlx4_log_num_mgm_entry_size (%d) not in legal range (-7..0 or %d..%d)\n",
			mlx4_log_num_mgm_entry_size,
			MLX4_MIN_MGM_LOG_ENTRY_SIZE,
			MLX4_MAX_MGM_LOG_ENTRY_SIZE);
		return -1;
	}

	return 0;
}

static int __init mlx4_init(void)
{
	int ret;

	if (mlx4_verify_params())
		return -EINVAL;

	mlx4_catas_init();

	mlx4_wq = create_singlethread_workqueue("mlx4");
	if (!mlx4_wq)
		return -ENOMEM;

	ret = pci_register_driver(&mlx4_driver);
	if (ret < 0)
		destroy_workqueue(mlx4_wq);
	return ret < 0 ? ret : 0;
}

static void __exit mlx4_cleanup(void)
{
	pci_unregister_driver(&mlx4_driver);
	destroy_workqueue(mlx4_wq);
}

module_init(mlx4_init);
module_exit(mlx4_cleanup);
