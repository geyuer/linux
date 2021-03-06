/*
 * Xilinx SDFEC
 *
 * Copyright (C) 2016 - 2017 Xilinx, Inc.
 *
 * Description:
 * This driver is developed for SDFEC16 (Soft Decision FEC 16nm)
 * IP. It exposes a char device interface in sysfs and supports file
 * operations like  open(), close() and ioctl().
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <uapi/misc/xilinx_sdfec.h>

#define DRIVER_NAME	"xilinx_sdfec"
#define DRIVER_VERSION	"0.3"
#define DRIVER_MAX_DEV	BIT(MINORBITS)

static  struct class *xsdfec_class;
static atomic_t xsdfec_ndevs = ATOMIC_INIT(0);
static dev_t xsdfec_devt;

/* Xilinx SDFEC Register Map */
#define XSDFEC_AXI_WR_PROTECT_ADDR		(0x00000)
#define XSDFEC_CODE_WR_PROTECT_ADDR		(0x00004)
#define XSDFEC_ACTIVE_ADDR			(0x00008)
#define XSDFEC_AXIS_WIDTH_ADDR			(0x0000c)
#define XSDFEC_AXIS_ENABLE_ADDR			(0x00010)
#define XSDFEC_AXIS_ENABLE_MASK			(0x0003F)
#define XSDFEC_FEC_CODE_ADDR			(0x00014)
#define XSDFEC_ORDER_ADDR			(0x00018)

/* Interrupt Status Register Bit Mask*/
#define XSDFEC_ISR_MASK				(0x0003F)
/* Interrupt Status Register */
#define XSDFEC_ISR_ADDR				(0x0001c)
/* Write Only - Interrupt Enable Register */
#define XSDFEC_IER_ADDR				(0x00020)
/* Write Only - Interrupt Disable Register */
#define XSDFEC_IDR_ADDR				(0x00024)
/* Read Only - Interrupt Mask Register */
#define XSDFEC_IMR_ADDR				(0x00028)

/* Single Bit Errors */
#define XSDFEC_ECC_ISR_SBE			(0x7FF)
/* Multi Bit Errors */
#define XSDFEC_ECC_ISR_MBE			(0x3FF800)
/* ECC Interrupt Status Bit Mask */
#define XSDFEC_ECC_ISR_MASK	(XSDFEC_ECC_ISR_SBE | XSDFEC_ECC_ISR_MBE)

/* Multi Bit Error Postion */
#define XSDFEC_ECC_MULTI_BIT_POS		(11)
#define XSDFEC_ERROR_MAX_THRESHOLD		(100)

/* ECC Interrupt Status Register */
#define XSDFEC_ECC_ISR_ADDR			(0x0002c)
/* Write Only - ECC Interrupt Enable Register */
#define XSDFEC_ECC_IER_ADDR			(0x00030)
/* Write Only - ECC Interrupt Disable Register */
#define XSDFEC_ECC_IDR_ADDR			(0x00034)
/* Read Only - ECC Interrupt Mask Register */
#define XSDFEC_ECC_IMR_ADDR			(0x00038)

#define XSDFEC_BYPASS_ADDR			(0x0003c)
#define XSDFEC_TEST_EMA_ADDR_BASE		(0x00080)
#define XSDFEC_TEST_EMA_ADDR_HIGH		(0x00089)
#define XSDFEC_TURBO_ADDR			(0x00100)
#define XSDFEC_LDPC_CODE_REG0_ADDR_BASE		(0x02000)
#define XSDFEC_LDPC_CODE_REG0_ADDR_HIGH		(0x021fc)
#define XSDFEC_LDPC_CODE_REG1_ADDR_BASE		(0x02004)
#define XSDFEC_LDPC_CODE_REG1_ADDR_HIGH		(0x02200)
#define XSDFEC_LDPC_CODE_REG2_ADDR_BASE		(0x02008)
#define XSDFEC_LDPC_CODE_REG2_ADDR_HIGH		(0x02204)
#define XSDFEC_LDPC_CODE_REG3_ADDR_BASE		(0x0200c)
#define XSDFEC_LDPC_CODE_REG3_ADDR_HIGH		(0x02208)

/**
 * struct xsdfec_dev - Driver data for SDFEC
 * @regs: device physical base address
 * @dev: pointer to device struct
 * @state: State of the SDFEC device
 * @config: Configuration of the SDFEC device
 * @intr_enabled: indicates IRQ enabled
 * @wr_protect: indicates Write Protect enabled
 * @isr_err_count: Count of ISR errors
 * @cecc_count: Count of Correctable ECC errors (SBE)
 * @uecc_count: Count of Uncorrectable ECC errors (MBE)
 * @open_count: Count of char device being opened
 * @irq: IRQ number
 * @xsdfec_cdev: Character device handle
 * @waitq: Driver wait queue
 *
 * This structure contains necessary state for SDFEC driver to operate
 */
struct xsdfec_dev {
	void __iomem *regs;
	struct device *dev;
	enum xsdfec_state state;
	struct xsdfec_config config;
	bool intr_enabled;
	bool wr_protect;
	atomic_t isr_err_count;
	atomic_t cecc_count;
	atomic_t uecc_count;
	atomic_t open_count;
	int  irq;
	struct cdev xsdfec_cdev;
	wait_queue_head_t waitq;
};

static inline void
xsdfec_regwrite(struct xsdfec_dev *xsdfec, u32 addr, u32 value)
{
	if (xsdfec->wr_protect) {
		dev_err(xsdfec->dev, "SDFEC in write protect");
		return;
	}

	dev_dbg(xsdfec->dev,
		"Writing 0x%x to offset 0x%x", value, addr);
	iowrite32(value, xsdfec->regs + addr);
}

static inline u32
xsdfec_regread(struct xsdfec_dev *xsdfec, u32 addr)
{
	u32 rval;

	rval = ioread32(xsdfec->regs + addr);
	dev_dbg(xsdfec->dev,
		"Read value = 0x%x from offset 0x%x",
		rval, addr);
	return rval;
}

#define XSDFEC_WRITE_PROTECT_ENABLE	(1)
#define XSDFEC_WRITE_PROTECT_DISABLE	(0)
static void
xsdfec_wr_protect(struct xsdfec_dev *xsdfec, bool wr_pr)
{
	if (wr_pr) {
		xsdfec_regwrite(xsdfec,
				XSDFEC_CODE_WR_PROTECT_ADDR,
				XSDFEC_WRITE_PROTECT_ENABLE);
		xsdfec_regwrite(xsdfec,
				XSDFEC_AXI_WR_PROTECT_ADDR,
				XSDFEC_WRITE_PROTECT_ENABLE);
		/* prevents register and tables writes */
		xsdfec->wr_protect = wr_pr;
	} else {
		/* allows register and table writes including protection regs */
		xsdfec->wr_protect = wr_pr;
		xsdfec_regwrite(xsdfec,
				XSDFEC_AXI_WR_PROTECT_ADDR,
				XSDFEC_WRITE_PROTECT_DISABLE);
		xsdfec_regwrite(xsdfec,
				XSDFEC_CODE_WR_PROTECT_ADDR,
				XSDFEC_WRITE_PROTECT_DISABLE);
	}
}

static int
xsdfec_dev_open(struct inode *iptr, struct file *fptr)
{
	struct xsdfec_dev *xsdfec;

	xsdfec = container_of(iptr->i_cdev, struct xsdfec_dev, xsdfec_cdev);
	if (!xsdfec)
		return  -EAGAIN;

	/* Only one open per device at a time */
	if (!atomic_dec_and_test(&xsdfec->open_count)) {
		atomic_inc(&xsdfec->open_count);
		return -EBUSY;
	}

	fptr->private_data = xsdfec;
	return 0;
}

static int
xsdfec_dev_release(struct inode *iptr, struct file *fptr)
{
	struct xsdfec_dev *xsdfec;

	xsdfec = container_of(iptr->i_cdev, struct xsdfec_dev, xsdfec_cdev);
	if (!xsdfec)
		return -EAGAIN;

	atomic_inc(&xsdfec->open_count);
	return 0;
}

#define XSDFEC_IS_ACTIVITY_SET	(0x1)
static int
xsdfec_get_status(struct xsdfec_dev *xsdfec, void __user *arg)
{
	struct xsdfec_status status;
	int err = 0;

	status.fec_id = xsdfec->config.fec_id;
	status.state = xsdfec->state;
	status.activity  =
		(xsdfec_regread(xsdfec,
				XSDFEC_ACTIVE_ADDR) &
				XSDFEC_IS_ACTIVITY_SET);

	err = copy_to_user(arg, &status, sizeof(status));
	if (err) {
		dev_err(xsdfec->dev, "%s failed for SDFEC%d",
			__func__, xsdfec->config.fec_id);
		err = -EFAULT;
	}
	return err;
}

static int
xsdfec_get_config(struct xsdfec_dev *xsdfec, void __user *arg)
{
	int err = 0;

	err = copy_to_user(arg, &xsdfec->config, sizeof(xsdfec->config));
	if (err) {
		dev_err(xsdfec->dev, "%s failed for SDFEC%d",
			__func__, xsdfec->config.fec_id);
		err = -EFAULT;
	}
	return err;
}

static int
xsdfec_isr_enable(struct xsdfec_dev *xsdfec, bool enable)
{
	u32 mask_read;

	if (enable) {
		/* Enable */
		xsdfec_regwrite(xsdfec, XSDFEC_IER_ADDR,
				XSDFEC_ISR_MASK);
		mask_read = xsdfec_regread(xsdfec, XSDFEC_IMR_ADDR);
		if (mask_read & XSDFEC_ISR_MASK) {
			dev_err(xsdfec->dev,
				"SDFEC enabling irq with IER failed");
			return -EIO;
		}
	} else {
		/* Disable */
		xsdfec_regwrite(xsdfec, XSDFEC_IDR_ADDR,
				XSDFEC_ISR_MASK);
		mask_read = xsdfec_regread(xsdfec, XSDFEC_IMR_ADDR);
		if ((mask_read & XSDFEC_ISR_MASK) != XSDFEC_ISR_MASK) {
			dev_err(xsdfec->dev,
				"SDFEC disabling irq with IDR failed");
			return -EIO;
		}
	}
	return 0;
}

static int
xsdfec_ecc_isr_enable(struct xsdfec_dev *xsdfec, bool enable)
{
	u32 mask_read;

	if (enable) {
		/* Enable */
		xsdfec_regwrite(xsdfec, XSDFEC_ECC_IER_ADDR,
				XSDFEC_ECC_ISR_MASK);
		mask_read = xsdfec_regread(xsdfec, XSDFEC_ECC_IMR_ADDR);
		if (mask_read & XSDFEC_ECC_ISR_MASK) {
			dev_err(xsdfec->dev,
				"SDFEC enabling ECC irq with ECC IER failed");
			return -EIO;
		}
	} else {
		/* Disable */
		xsdfec_regwrite(xsdfec, XSDFEC_ECC_IDR_ADDR,
				XSDFEC_ECC_ISR_MASK);
		mask_read = xsdfec_regread(xsdfec, XSDFEC_ECC_IMR_ADDR);
		if ((mask_read & XSDFEC_ECC_ISR_MASK) != XSDFEC_ECC_ISR_MASK) {
			dev_err(xsdfec->dev,
				"SDFEC disable ECC irq with ECC IDR failed");
			return -EIO;
		}
	}
	return 0;
}

static int
xsdfec_set_irq(struct xsdfec_dev *xsdfec, void __user *arg)
{
	struct xsdfec_irq  irq;
	int err = 0;

	err = copy_from_user(&irq, arg, sizeof(irq));
	if (err) {
		dev_err(xsdfec->dev, "%s failed for SDFEC%d",
			__func__, xsdfec->config.fec_id);
		return -EFAULT;
	}

	/* Setup tlast related IRQ */
	if (irq.enable_isr) {
		err = xsdfec_isr_enable(xsdfec, true);
		if (err < 0)
			return err;
	}

	/* Setup ECC related IRQ */
	if (irq.enable_ecc_isr) {
		err = xsdfec_ecc_isr_enable(xsdfec, true);
		if (err < 0)
			return err;
	}

	return 0;
}

#define XSDFEC_TURBO_SCALE_MASK		(0xF)
#define XSDFEC_TURBO_SCALE_BIT_POS	(8)
static int
xsdfec_set_turbo(struct xsdfec_dev *xsdfec, void __user *arg)
{
	struct xsdfec_turbo turbo;
	int err = 0;
	u32 turbo_write = 0;

	err = copy_from_user(&turbo, arg, sizeof(turbo));
	if (err) {
		dev_err(xsdfec->dev, "%s failed for SDFEC%d",
			__func__, xsdfec->config.fec_id);
		return -EFAULT;
	}

	/* Check to see what device tree says about the FEC codes */
	if (xsdfec->config.code == XSDFEC_LDPC_CODE) {
		dev_err(xsdfec->dev,
			"%s: Unable to write Turbo to SDFEC%d check DT",
				__func__, xsdfec->config.fec_id);
		return -EIO;
	} else if (xsdfec->config.code == XSDFEC_CODE_INVALID) {
		xsdfec->config.code = XSDFEC_TURBO_CODE;
	}

	if (xsdfec->wr_protect)
		xsdfec_wr_protect(xsdfec, false);

	turbo_write = ((turbo.scale & XSDFEC_TURBO_SCALE_MASK) <<
			XSDFEC_TURBO_SCALE_BIT_POS) | turbo.alg;
	xsdfec_regwrite(xsdfec, XSDFEC_TURBO_ADDR, turbo_write);
	return err;
}

static int
xsdfec_get_turbo(struct xsdfec_dev *xsdfec, void __user *arg)
{
	u32 reg_value;
	struct xsdfec_turbo turbo_params;
	int err;

	if (xsdfec->config.code == XSDFEC_LDPC_CODE) {
		dev_err(xsdfec->dev,
			"%s: SDFEC%d is configured for LDPC, check DT",
			__func__, xsdfec->config.fec_id);
		return -EIO;
	}

	reg_value = xsdfec_regread(xsdfec, XSDFEC_TURBO_ADDR);

	turbo_params.scale = (reg_value & XSDFEC_TURBO_SCALE_MASK) >>
			      XSDFEC_TURBO_SCALE_BIT_POS;
	turbo_params.alg = reg_value & 0x1;

	err = copy_to_user(arg, &turbo_params, sizeof(turbo_params));
	if (err) {
		dev_err(xsdfec->dev, "%s failed for SDFEC%d",
			__func__, xsdfec->config.fec_id);
		err = -EFAULT;
	}

	return err;
}

#define XSDFEC_LDPC_REG_JUMP	(0x10)
#define XSDFEC_REG0_N_MASK	(0x0000FFFF)
#define XSDFEC_REG0_N_LSB	(0)
#define XSDFEC_REG0_K_MASK	(0x7fff0000)
#define XSDFEC_REG0_K_LSB	(16)
static int
xsdfec_reg0_write(struct xsdfec_dev *xsdfec,
		  u32 n, u32 k, u32 offset)
{
	u32 wdata;

	/* Use only lower 16 bits */
	if (n & ~XSDFEC_REG0_N_MASK)
		dev_err(xsdfec->dev, "N value is beyond 16 bits");
	n &= XSDFEC_REG0_N_MASK;
	n <<= XSDFEC_REG0_N_LSB;

	if (k & XSDFEC_REG0_K_MASK)
		dev_err(xsdfec->dev, "K value is beyond 16 bits");

	k = ((k << XSDFEC_REG0_K_LSB) & XSDFEC_REG0_K_MASK);
	wdata = k | n;

	if (XSDFEC_LDPC_CODE_REG0_ADDR_BASE + (offset * XSDFEC_LDPC_REG_JUMP)
				> XSDFEC_LDPC_CODE_REG0_ADDR_HIGH) {
		dev_err(xsdfec->dev,
			"Writing outside of LDPC reg0 space 0x%x",
			XSDFEC_LDPC_CODE_REG0_ADDR_BASE +
			(offset * XSDFEC_LDPC_REG_JUMP));
		return -EINVAL;
	}
	xsdfec_regwrite(xsdfec,
			XSDFEC_LDPC_CODE_REG0_ADDR_BASE +
			(offset * XSDFEC_LDPC_REG_JUMP), wdata);
	return 0;
}

static int
xsdfec_collect_ldpc_reg0(struct xsdfec_dev *xsdfec,
			 u32 code_id,
			 struct xsdfec_ldpc_params *ldpc_params)
{
	u32 reg_value;
	u32 reg_addr = XSDFEC_LDPC_CODE_REG0_ADDR_BASE +
		(code_id * XSDFEC_LDPC_REG_JUMP);

	if (reg_addr > XSDFEC_LDPC_CODE_REG0_ADDR_HIGH) {
		dev_err(xsdfec->dev,
			"Accessing outside of LDPC reg0 for code_id %d",
			code_id);
		return -EINVAL;
	}

	reg_value = xsdfec_regread(xsdfec, reg_addr);

	ldpc_params->n = (reg_value >> XSDFEC_REG0_N_LSB) & XSDFEC_REG0_N_MASK;

	ldpc_params->k = (reg_value >> XSDFEC_REG0_K_LSB) & XSDFEC_REG0_K_MASK;

	return 0;
}

#define XSDFEC_REG1_PSIZE_MASK		(0x000001ff)
#define XSDFEC_REG1_NO_PACKING_MASK	(0x00000400)
#define XSDFEC_REG1_NO_PACKING_LSB	(10)
#define XSDFEC_REG1_NM_MASK		(0x000ff800)
#define XSDFEC_REG1_NM_LSB		(11)
#define XSDFEC_REG1_BYPASS_MASK	(0x00100000)
static int
xsdfec_reg1_write(struct xsdfec_dev *xsdfec, u32 psize,
		  u32 no_packing, u32 nm, u32 offset)
{
	u32 wdata;

	if (psize & ~XSDFEC_REG1_PSIZE_MASK)
		dev_err(xsdfec->dev, "Psize is beyond 10 bits");
	psize &= XSDFEC_REG1_PSIZE_MASK;

	if (no_packing != 0 && no_packing != 1)
		dev_err(xsdfec->dev, "No-packing bit register invalid");
	no_packing = ((no_packing << XSDFEC_REG1_NO_PACKING_LSB) &
					XSDFEC_REG1_NO_PACKING_MASK);

	if (nm & ~(XSDFEC_REG1_NM_MASK >> XSDFEC_REG1_NM_LSB))
		dev_err(xsdfec->dev, "NM is beyond 10 bits");
	nm = (nm << XSDFEC_REG1_NM_LSB) & XSDFEC_REG1_NM_MASK;

	wdata = nm | no_packing | psize;
	if (XSDFEC_LDPC_CODE_REG1_ADDR_BASE + (offset * XSDFEC_LDPC_REG_JUMP)
		> XSDFEC_LDPC_CODE_REG1_ADDR_HIGH) {
		dev_err(xsdfec->dev,
			"Writing outside of LDPC reg1 space 0x%x",
			XSDFEC_LDPC_CODE_REG1_ADDR_BASE +
			(offset * XSDFEC_LDPC_REG_JUMP));
		return -EINVAL;
	}
	xsdfec_regwrite(xsdfec, XSDFEC_LDPC_CODE_REG1_ADDR_BASE +
		(offset * XSDFEC_LDPC_REG_JUMP), wdata);
	return 0;
}

static int
xsdfec_collect_ldpc_reg1(struct xsdfec_dev *xsdfec,
			 u32 code_id,
			 struct xsdfec_ldpc_params *ldpc_params)
{
	u32 reg_value;
	u32 reg_addr = XSDFEC_LDPC_CODE_REG1_ADDR_BASE +
		(code_id * XSDFEC_LDPC_REG_JUMP);

	if (reg_addr > XSDFEC_LDPC_CODE_REG1_ADDR_HIGH) {
		dev_err(xsdfec->dev,
			"Accessing outside of LDPC reg1 for code_id %d",
			code_id);
		return -EINVAL;
	}

	reg_value = xsdfec_regread(xsdfec, reg_addr);

	ldpc_params->psize = reg_value & XSDFEC_REG1_PSIZE_MASK;

	ldpc_params->no_packing = ((reg_value >> XSDFEC_REG1_NO_PACKING_LSB) &
				    XSDFEC_REG1_NO_PACKING_MASK);

	ldpc_params->nm = (reg_value >> XSDFEC_REG1_NM_LSB) &
			   XSDFEC_REG1_NM_MASK;
	return 0;
}

#define XSDFEC_REG2_NLAYERS_MASK		(0x000001FF)
#define XSDFEC_REG2_NLAYERS_LSB			(0)
#define XSDFEC_REG2_NNMQC_MASK			(0x000FFE00)
#define XSDFEC_REG2_NMQC_LSB			(9)
#define XSDFEC_REG2_NORM_TYPE_MASK		(0x00100000)
#define XSDFEC_REG2_NORM_TYPE_LSB		(20)
#define XSDFEC_REG2_SPECIAL_QC_MASK		(0x00200000)
#define XSDFEC_REG2_SPEICAL_QC_LSB		(21)
#define XSDFEC_REG2_NO_FINAL_PARITY_MASK	(0x00400000)
#define XSDFEC_REG2_NO_FINAL_PARITY_LSB		(22)
#define XSDFEC_REG2_MAX_SCHEDULE_MASK		(0x01800000)
#define XSDFEC_REG2_MAX_SCHEDULE_LSB		(23)

static int
xsdfec_reg2_write(struct xsdfec_dev *xsdfec, u32 nlayers, u32 nmqc,
		  u32 norm_type, u32 special_qc, u32 no_final_parity,
		  u32 max_schedule, u32 offset)
{
	u32 wdata;

	if (nlayers & ~(XSDFEC_REG2_NLAYERS_MASK >>
				XSDFEC_REG2_NLAYERS_LSB))
		dev_err(xsdfec->dev, "Nlayers exceeds 9 bits");
	nlayers &= XSDFEC_REG2_NLAYERS_MASK;

	if (nmqc & ~(XSDFEC_REG2_NNMQC_MASK >> XSDFEC_REG2_NMQC_LSB))
		dev_err(xsdfec->dev, "NMQC exceeds 11 bits");
	nmqc = (nmqc << XSDFEC_REG2_NMQC_LSB) & XSDFEC_REG2_NNMQC_MASK;

	if (norm_type > 1)
		dev_err(xsdfec->dev, "Norm type is invalid");
	norm_type = ((norm_type << XSDFEC_REG2_NORM_TYPE_LSB) &
					XSDFEC_REG2_NORM_TYPE_MASK);
	if (special_qc > 1)
		dev_err(xsdfec->dev, "Special QC in invalid");
	special_qc = ((special_qc << XSDFEC_REG2_SPEICAL_QC_LSB) &
			XSDFEC_REG2_SPECIAL_QC_MASK);

	if (no_final_parity > 1)
		dev_err(xsdfec->dev, "No final parity check invalid");
	no_final_parity =
		((no_final_parity << XSDFEC_REG2_NO_FINAL_PARITY_LSB) &
					XSDFEC_REG2_NO_FINAL_PARITY_MASK);
	if (max_schedule & ~(XSDFEC_REG2_MAX_SCHEDULE_MASK >>
					XSDFEC_REG2_MAX_SCHEDULE_LSB))
		dev_err(xsdfec->dev, "Max Schdule exceeds 2 bits");
	max_schedule = ((max_schedule << XSDFEC_REG2_MAX_SCHEDULE_LSB) &
				XSDFEC_REG2_MAX_SCHEDULE_MASK);

	wdata = (max_schedule | no_final_parity | special_qc | norm_type |
			nmqc | nlayers);

	if (XSDFEC_LDPC_CODE_REG2_ADDR_BASE + (offset * XSDFEC_LDPC_REG_JUMP)
		> XSDFEC_LDPC_CODE_REG2_ADDR_HIGH) {
		dev_err(xsdfec->dev,
			"Writing outside of LDPC reg2 space 0x%x",
			XSDFEC_LDPC_CODE_REG2_ADDR_BASE +
			(offset * XSDFEC_LDPC_REG_JUMP));
		return -EINVAL;
	}
	xsdfec_regwrite(xsdfec, XSDFEC_LDPC_CODE_REG2_ADDR_BASE +
			(offset * XSDFEC_LDPC_REG_JUMP), wdata);
	return 0;
}

static int
xsdfec_collect_ldpc_reg2(struct xsdfec_dev *xsdfec,
			 u32 code_id,
			 struct xsdfec_ldpc_params *ldpc_params)
{
	u32 reg_value;
	u32 reg_addr = XSDFEC_LDPC_CODE_REG2_ADDR_BASE +
		(code_id * XSDFEC_LDPC_REG_JUMP);

	if (reg_addr > XSDFEC_LDPC_CODE_REG2_ADDR_HIGH) {
		dev_err(xsdfec->dev,
			"Accessing outside of LDPC reg2 for code_id %d",
			code_id);
		return -EINVAL;
	}

	reg_value = xsdfec_regread(xsdfec, reg_addr);

	ldpc_params->nlayers = ((reg_value >> XSDFEC_REG2_NLAYERS_LSB) &
				XSDFEC_REG2_NLAYERS_MASK);

	ldpc_params->nmqc = (reg_value >> XSDFEC_REG2_NMQC_LSB) &
			     XSDFEC_REG2_NNMQC_MASK;

	ldpc_params->norm_type = ((reg_value >> XSDFEC_REG2_NORM_TYPE_LSB) &
				  XSDFEC_REG2_NORM_TYPE_MASK);

	ldpc_params->special_qc = ((reg_value >> XSDFEC_REG2_SPEICAL_QC_LSB) &
				   XSDFEC_REG2_SPECIAL_QC_MASK);

	ldpc_params->no_final_parity =
		((reg_value >> XSDFEC_REG2_NO_FINAL_PARITY_LSB) &
		 XSDFEC_REG2_NO_FINAL_PARITY_MASK);

	ldpc_params->max_schedule =
		((reg_value >> XSDFEC_REG2_MAX_SCHEDULE_LSB) &
		 XSDFEC_REG2_MAX_SCHEDULE_MASK);

	return 0;
}

#define XSDFEC_REG3_LA_OFF_LSB		(8)
#define XSDFEC_REG3_QC_OFF_LSB		(16)
static int
xsdfec_reg3_write(struct xsdfec_dev *xsdfec, u8 sc_off,
		  u8 la_off, u16 qc_off, u32 offset)
{
	u32 wdata;

	wdata = ((qc_off << XSDFEC_REG3_QC_OFF_LSB) |
		(la_off << XSDFEC_REG3_LA_OFF_LSB) | sc_off);
	if (XSDFEC_LDPC_CODE_REG3_ADDR_BASE +
		(offset *  XSDFEC_LDPC_REG_JUMP) >
			XSDFEC_LDPC_CODE_REG3_ADDR_HIGH) {
		dev_err(xsdfec->dev,
			"Writing outside of LDPC reg3 space 0x%x",
			XSDFEC_LDPC_CODE_REG3_ADDR_BASE +
			(offset * XSDFEC_LDPC_REG_JUMP));
		return -EINVAL;
	}
	xsdfec_regwrite(xsdfec, XSDFEC_LDPC_CODE_REG3_ADDR_BASE +
			(offset * XSDFEC_LDPC_REG_JUMP), wdata);
	return 0;
}

static int
xsdfec_collect_ldpc_reg3(struct xsdfec_dev *xsdfec,
			 u32 code_id,
			 struct xsdfec_ldpc_params *ldpc_params)
{
	u32 reg_value;
	u32 reg_addr = XSDFEC_LDPC_CODE_REG3_ADDR_BASE +
		(code_id * XSDFEC_LDPC_REG_JUMP);

	if (reg_addr > XSDFEC_LDPC_CODE_REG3_ADDR_HIGH) {
		dev_err(xsdfec->dev,
			"Accessing outside of LDPC reg3 for code_id %d",
			code_id);
		return -EINVAL;
	}

	reg_value = xsdfec_regread(xsdfec, reg_addr);

	ldpc_params->qc_off = (reg_addr >> XSDFEC_REG3_QC_OFF_LSB) & 0xFF;
	ldpc_params->la_off = (reg_addr >> XSDFEC_REG3_LA_OFF_LSB) & 0xFF;
	ldpc_params->sc_off = (reg_addr & 0xFF);

	return 0;
}

#define XSDFEC_SC_TABLE_DEPTH		(0x3fc)
#define XSDFEC_REG_WIDTH_JUMP		(4)
static int
xsdfec_sc_table_write(struct xsdfec_dev *xsdfec, u32 offset,
		      u32 *sc_ptr, u32 len)
{
	int reg;

	/*
	 * Writes that go beyond the length of
	 * Shared Scale(SC) table should fail
	 */
	if ((XSDFEC_REG_WIDTH_JUMP * (offset + len)) > XSDFEC_SC_TABLE_DEPTH) {
		dev_err(xsdfec->dev, "Write exceeds SC table length");
		return -EINVAL;
	}

	for (reg = 0; reg < len; reg++) {
		xsdfec_regwrite(xsdfec, XSDFEC_LDPC_SC_TABLE_ADDR_BASE +
		(offset + reg) *  XSDFEC_REG_WIDTH_JUMP, sc_ptr[reg]);
	}
	return reg;
}

static int
xsdfec_collect_sc_table(struct xsdfec_dev *xsdfec, u32 offset,
			u32 *sc_ptr, u32 len)
{
	u32 reg;
	u32 reg_addr;
	u32 deepest_reach = (XSDFEC_REG_WIDTH_JUMP * (offset + len));

	if (deepest_reach > XSDFEC_SC_TABLE_DEPTH) {
		dev_err(xsdfec->dev, "Access will exceed SC table length");
		return -EINVAL;
	}

	for (reg = 0; reg < len; reg++) {
		reg_addr = XSDFEC_LDPC_SC_TABLE_ADDR_BASE +
			((offset + reg) * XSDFEC_REG_WIDTH_JUMP);

		sc_ptr[reg] = xsdfec_regread(xsdfec, reg_addr);
	}

	return 0;
}

#define XSDFEC_LA_TABLE_DEPTH		(0xFFC)
static int
xsdfec_la_table_write(struct xsdfec_dev *xsdfec, u32 offset,
		      u32 *la_ptr, u32 len)
{
	int reg;

	if (XSDFEC_REG_WIDTH_JUMP * (offset + len) > XSDFEC_LA_TABLE_DEPTH) {
		dev_err(xsdfec->dev, "Write exceeds LA table length");
		return -EINVAL;
	}

	for (reg = 0; reg < len; reg++) {
		xsdfec_regwrite(xsdfec, XSDFEC_LDPC_LA_TABLE_ADDR_BASE +
				(offset + reg) * XSDFEC_REG_WIDTH_JUMP,
				la_ptr[reg]);
	}
	return reg;
}

static int
xsdfec_collect_la_table(struct xsdfec_dev *xsdfec, u32 offset,
			u32 *la_ptr, u32 len)
{
	u32 reg;
	u32 reg_addr;
	u32 deepest_reach = (XSDFEC_REG_WIDTH_JUMP * (offset + len));

	if (deepest_reach > XSDFEC_LA_TABLE_DEPTH) {
		dev_err(xsdfec->dev, "Access will exceed LA table length");
		return -EINVAL;
	}

	for (reg = 0; reg < len; reg++) {
		reg_addr = XSDFEC_LDPC_LA_TABLE_ADDR_BASE +
				((offset + reg) * XSDFEC_REG_WIDTH_JUMP);

		la_ptr[reg] = xsdfec_regread(xsdfec, reg_addr);
	}

	return 0;
}

#define XSDFEC_QC_TABLE_DEPTH		(0x7FFC)
static int
xsdfec_qc_table_write(struct xsdfec_dev *xsdfec,
		      u32 offset, u32 *qc_ptr, u32 len)
{
	int reg;

	if (XSDFEC_REG_WIDTH_JUMP * (offset + len) > XSDFEC_QC_TABLE_DEPTH) {
		dev_err(xsdfec->dev, "Write exceeds QC table length");
		return -EINVAL;
	}

	for (reg = 0; reg < len; reg++) {
		xsdfec_regwrite(xsdfec, XSDFEC_LDPC_QC_TABLE_ADDR_BASE +
		 (offset + reg) * XSDFEC_REG_WIDTH_JUMP, qc_ptr[reg]);
	}

	return reg;
}

static int
xsdfec_collect_qc_table(struct xsdfec_dev *xsdfec,
			u32 offset, u32 *qc_ptr, u32 len)
{
	u32 reg;
	u32 reg_addr;
	u32 deepest_reach = (XSDFEC_REG_WIDTH_JUMP * (offset + len));

	if (deepest_reach > XSDFEC_QC_TABLE_DEPTH) {
		dev_err(xsdfec->dev, "Access will exceed QC table length");
		return -EINVAL;
	}

	for (reg = 0; reg < len; reg++) {
		reg_addr = XSDFEC_LDPC_QC_TABLE_ADDR_BASE +
		 (offset + reg) * XSDFEC_REG_WIDTH_JUMP;

		qc_ptr[reg] = xsdfec_regread(xsdfec, reg_addr);
	}

	return 0;
}

static int
xsdfec_add_ldpc(struct xsdfec_dev *xsdfec, void __user *arg)
{
	struct xsdfec_ldpc_params *ldpc;
	int err;

	ldpc = kzalloc(sizeof(*ldpc), GFP_KERNEL);
	if (!ldpc)
		return -ENOMEM;

	err = copy_from_user(ldpc, arg, sizeof(*ldpc));
	if (err) {
		dev_err(xsdfec->dev,
			"%s failed to copy from user for SDFEC%d",
			__func__, xsdfec->config.fec_id);
		goto err_out;
	}
	if (xsdfec->config.code == XSDFEC_TURBO_CODE) {
		dev_err(xsdfec->dev,
			"%s: Unable to write LDPC to SDFEC%d check DT",
			__func__, xsdfec->config.fec_id);
		goto err_out;
	}
	/* Disable Write Protection before proceeding */
	if (xsdfec->wr_protect)
		xsdfec_wr_protect(xsdfec, false);

	/* Write Reg 0 */
	err = xsdfec_reg0_write(xsdfec, ldpc->n, ldpc->k, ldpc->code_id);
	if (err)
		goto err_out;

	/* Write Reg 1 */
	err = xsdfec_reg1_write(xsdfec, ldpc->psize, ldpc->no_packing,
				ldpc->nm, ldpc->code_id);
	if (err)
		goto err_out;

	/* Write Reg 2 */
	err = xsdfec_reg2_write(xsdfec, ldpc->nlayers, ldpc->nmqc,
				ldpc->norm_type, ldpc->special_qc,
				ldpc->no_final_parity, ldpc->max_schedule,
				ldpc->code_id);
	if (err)
		goto err_out;

	/* Write Reg 3 */
	err = xsdfec_reg3_write(xsdfec, ldpc->sc_off,
				ldpc->la_off, ldpc->qc_off, ldpc->code_id);
	if (err)
		goto err_out;

	/* Write Shared Codes */
	err = xsdfec_sc_table_write(xsdfec, ldpc->sc_off,
				    ldpc->sc_table, ldpc->nlayers);
	if (err < 0)
		goto err_out;

	err = xsdfec_la_table_write(xsdfec, 4 * ldpc->la_off,
				    ldpc->la_table, ldpc->nlayers);
	if (err < 0)
		goto err_out;

	err = xsdfec_qc_table_write(xsdfec, 4 * ldpc->qc_off,
				    ldpc->qc_table, ldpc->nqc);
	if (err < 0)
		goto err_out;

	kfree(ldpc);
	return 0;
	/* Error Path */
err_out:
	kfree(ldpc);
	return err;
}

static int
xsdfec_get_ldpc_code_params(struct xsdfec_dev *xsdfec, void __user *arg)
{
	struct xsdfec_ldpc_params *ldpc_params;
	int err = 0;

	if (xsdfec->config.code == XSDFEC_TURBO_CODE) {
		dev_err(xsdfec->dev,
			"%s: SDFEC%d is configured for TURBO, check DT",
				__func__, xsdfec->config.fec_id);
		return -EIO;
	}

	ldpc_params = kzalloc(sizeof(*ldpc_params), GFP_KERNEL);
	if (!ldpc_params)
		return -ENOMEM;

	err = copy_from_user(ldpc_params, arg, sizeof(*ldpc_params));
	if (err) {
		dev_err(xsdfec->dev,
			"%s failed to copy from user for SDFEC%d",
			__func__, xsdfec->config.fec_id);
		goto err_out;
	}

	err = xsdfec_collect_ldpc_reg0(xsdfec, ldpc_params->code_id,
				       ldpc_params);
	if (err)
		goto err_out;

	err = xsdfec_collect_ldpc_reg1(xsdfec, ldpc_params->code_id,
				       ldpc_params);
	if (err)
		goto err_out;

	err = xsdfec_collect_ldpc_reg2(xsdfec, ldpc_params->code_id,
				       ldpc_params);
	if (err)
		goto err_out;

	err = xsdfec_collect_ldpc_reg3(xsdfec, ldpc_params->code_id,
				       ldpc_params);
	if (err)
		goto err_out;

	/*
	 * Collect the shared table values, needs to happen after reading
	 * the registers
	 */
	err = xsdfec_collect_sc_table(xsdfec, ldpc_params->sc_off,
				      ldpc_params->sc_table,
				      ldpc_params->nlayers);
	if (err < 0)
		goto err_out;

	err = xsdfec_collect_la_table(xsdfec, 4 * ldpc_params->la_off,
				      ldpc_params->la_table,
				      ldpc_params->nlayers);
	if (err < 0)
		goto err_out;

	err = xsdfec_collect_qc_table(xsdfec, 4 * ldpc_params->qc_off,
				      ldpc_params->qc_table,
				      ldpc_params->nqc);
	if (err < 0)
		goto err_out;

	err = copy_to_user(arg, ldpc_params, sizeof(*ldpc_params));
	if (err) {
		dev_err(xsdfec->dev, "%s failed for SDFEC%d",
			__func__, xsdfec->config.fec_id);
		err = -EFAULT;
	}

	kfree(ldpc_params);
	return 0;
	/* Error Path */
err_out:
	kfree(ldpc_params);
	return err;
}

static int
xsdfec_set_order(struct xsdfec_dev *xsdfec, void __user *arg)
{
	bool order_out_of_range;
	enum xsdfec_order order = *((enum xsdfec_order *)arg);

	order_out_of_range = (order <= XSDFEC_INVALID_ORDER) ||
			     (order >= XSDFEC_ORDER_MAX);
	if (order_out_of_range) {
		dev_err(xsdfec->dev,
			"%s invalid order value %d for SDFEC%d",
			__func__, order, xsdfec->config.fec_id);
		return -EINVAL;
	}

	/* Verify Device has not started */
	if (xsdfec->state == XSDFEC_STARTED) {
		dev_err(xsdfec->dev,
			"%s attempting to set Order while started for SDFEC%d",
			__func__, xsdfec->config.fec_id);
		return -EIO;
	}

	xsdfec_regwrite(xsdfec, XSDFEC_ORDER_ADDR, (order - 1));

	xsdfec->config.order = order;

	return 0;
}

static int
xsdfec_set_bypass(struct xsdfec_dev *xsdfec, void __user *arg)
{
	unsigned long bypass = *((unsigned long *)arg);

	if (bypass > 1) {
		dev_err(xsdfec->dev,
			"%s invalid bypass value %ld for SDFEC%d",
			__func__, bypass, xsdfec->config.fec_id);
		return -EINVAL;
	}

	/* Verify Device has not started */
	if (xsdfec->state == XSDFEC_STARTED) {
		dev_err(xsdfec->dev,
			"%s attempting to set bypass while started for SDFEC%d",
			__func__, xsdfec->config.fec_id);
		return -EIO;
	}

	xsdfec_regwrite(xsdfec, XSDFEC_BYPASS_ADDR, bypass);

	return 0;
}

static int
xsdfec_is_active(struct xsdfec_dev *xsdfec, bool __user *is_active)
{
	u32 reg_value;

	reg_value = xsdfec_regread(xsdfec, XSDFEC_ACTIVE_ADDR);
	/* using a double ! operator instead of casting */
	*is_active = !!(reg_value & XSDFEC_IS_ACTIVITY_SET);

	return 0;
}

static u32
xsdfec_translate_axis_width_cfg_val(enum xsdfec_axis_width axis_width_cfg)
{
	u32 axis_width_field = 0;

	switch (axis_width_cfg) {
	case XSDFEC_1x128b:
		axis_width_field = 0;
		break;
	case XSDFEC_2x128b:
		axis_width_field = 1;
		break;
	case XSDFEC_4x128b:
		axis_width_field = 2;
		break;
	}

	return axis_width_field;
}

static u32
xsdfec_translate_axis_words_cfg_val(
	enum xsdfec_axis_word_include axis_word_inc_cfg)
{
	u32 axis_words_field = 0;

	if (axis_word_inc_cfg == XSDFEC_FIXED_VALUE ||
	    axis_word_inc_cfg == XSDFEC_IN_BLOCK)
		axis_words_field = 0;
	else if (axis_word_inc_cfg == XSDFEC_PER_AXI_TRANSACTION)
		axis_words_field = 1;

	return axis_words_field;
}

#define XSDFEC_AXIS_DOUT_WORDS_LSB	(5)
#define XSDFEC_AXIS_DOUT_WIDTH_LSB	(3)
#define XSDFEC_AXIS_DIN_WORDS_LSB	(2)
#define XSDFEC_AXIS_DIN_WIDTH_LSB	(0)
static int
xsdfec_cfg_axi_streams(struct xsdfec_dev *xsdfec)
{
	u32 reg_value;
	u32 dout_words_field;
	u32 dout_width_field;
	u32 din_words_field;
	u32 din_width_field;
	struct xsdfec_config *config = &xsdfec->config;

	/* translate config info to register values */
	dout_words_field =
		xsdfec_translate_axis_words_cfg_val(config->dout_word_include);
	dout_width_field =
		xsdfec_translate_axis_width_cfg_val(config->dout_width);
	din_words_field =
		xsdfec_translate_axis_words_cfg_val(config->din_word_include);
	din_width_field =
		xsdfec_translate_axis_width_cfg_val(config->din_width);

	reg_value = dout_words_field << XSDFEC_AXIS_DOUT_WORDS_LSB;
	reg_value |= dout_width_field << XSDFEC_AXIS_DOUT_WIDTH_LSB;
	reg_value |= din_words_field << XSDFEC_AXIS_DIN_WORDS_LSB;
	reg_value |= din_width_field << XSDFEC_AXIS_DIN_WIDTH_LSB;

	xsdfec_regwrite(xsdfec, XSDFEC_AXIS_WIDTH_ADDR, reg_value);

	return 0;
}

static int xsdfec_start(struct xsdfec_dev *xsdfec)
{
	u32 regread;

	/* Verify Code is loaded */
	if (xsdfec->config.code == XSDFEC_CODE_INVALID) {
		dev_err(xsdfec->dev,
			"%s : set code before start for SDFEC%d",
			__func__, xsdfec->config.fec_id);
		return -EINVAL;
	}
	regread = xsdfec_regread(xsdfec, XSDFEC_FEC_CODE_ADDR);
	regread &= 0x1;
	if (regread != (xsdfec->config.code - 1)) {
		dev_err(xsdfec->dev,
			"%s SDFEC HW code does not match driver code, reg %d, code %d",
			__func__, regread, (xsdfec->config.code - 1));
		return -EINVAL;
	}

	/* Verify Order has been set */
	if (xsdfec->config.order == XSDFEC_INVALID_ORDER) {
		dev_err(xsdfec->dev,
			"%s : set order before starting SDFEC%d",
			__func__, xsdfec->config.fec_id);
		return -EINVAL;
	}

	/* Set AXIS enable */
	xsdfec_regwrite(xsdfec,
			XSDFEC_AXIS_ENABLE_ADDR,
			XSDFEC_AXIS_ENABLE_MASK);
	/* Write Protect Code and Registers */
	xsdfec_wr_protect(xsdfec, true);
	/* Done */
	xsdfec->state = XSDFEC_STARTED;
	return 0;
}

static int
xsdfec_stop(struct xsdfec_dev *xsdfec)
{
	u32 regread;

	if (xsdfec->state != XSDFEC_STARTED)
		dev_err(xsdfec->dev, "Device not started correctly");
	/* Disable Write Protect */
	xsdfec_wr_protect(xsdfec, false);
	/* Disable AXIS_ENABLE register */
	regread = xsdfec_regread(xsdfec, XSDFEC_AXIS_ENABLE_ADDR);
	regread &= (~XSDFEC_AXIS_ENABLE_MASK);
	xsdfec_regwrite(xsdfec, XSDFEC_AXIS_ENABLE_ADDR, regread);
	/* Stop */
	xsdfec->state = XSDFEC_STOPPED;
	return 0;
}

static int
xsdfec_clear_stats(struct xsdfec_dev *xsdfec)
{
	atomic_set(&xsdfec->isr_err_count, 0);
	atomic_set(&xsdfec->uecc_count, 0);
	atomic_set(&xsdfec->cecc_count, 0);

	return 0;
}

static int
xsdfec_get_stats(struct xsdfec_dev *xsdfec, void __user *arg)
{
	int err = 0;
	struct xsdfec_stats user_stats;

	user_stats.isr_err_count = atomic_read(&xsdfec->isr_err_count);
	user_stats.cecc_count = atomic_read(&xsdfec->cecc_count);
	user_stats.uecc_count = atomic_read(&xsdfec->uecc_count);

	err = copy_to_user(arg, &user_stats, sizeof(user_stats));
	if (err) {
		dev_err(xsdfec->dev, "%s failed for SDFEC%d",
			__func__, xsdfec->config.fec_id);
		err = -EFAULT;
	}

	return err;
}

static int
xsdfec_set_default_config(struct xsdfec_dev *xsdfec)
{
	xsdfec->state = XSDFEC_INIT;
	xsdfec->config.order = XSDFEC_INVALID_ORDER;
	xsdfec->wr_protect = false;

	xsdfec_wr_protect(xsdfec, false);
	/* Ensure registers are aligned with core configuration */
	xsdfec_regwrite(xsdfec, XSDFEC_FEC_CODE_ADDR, xsdfec->config.code - 1);
	xsdfec_cfg_axi_streams(xsdfec);

	return 0;
}

static long
xsdfec_dev_ioctl(struct file *fptr, unsigned int cmd, unsigned long data)
{
	struct xsdfec_dev *xsdfec = fptr->private_data;
	void __user *arg = NULL;
	int rval = -EINVAL;
	int err = 0;

	if (!xsdfec)
		return rval;

	/* In failed state allow only reset and get status IOCTLs */
	if (xsdfec->state == XSDFEC_NEEDS_RESET &&
	    (cmd != XSDFEC_SET_DEFAULT_CONFIG && cmd != XSDFEC_GET_STATUS &&
	     cmd != XSDFEC_GET_STATS && cmd != XSDFEC_CLEAR_STATS)) {
		dev_err(xsdfec->dev,
			"SDFEC%d in failed state. Reset Required",
			xsdfec->config.fec_id);
		return -EPERM;
	}

	if (_IOC_TYPE(cmd) != XSDFEC_MAGIC) {
		dev_err(xsdfec->dev, "Not a xilinx sdfec ioctl");
		return -ENOTTY;
	}

	/* check if ioctl argument is present and valid */
	if (_IOC_DIR(cmd) != _IOC_NONE) {
		arg = (void __user *)data;
		if (!arg) {
			dev_err(xsdfec->dev, "xilinx sdfec ioctl argument is NULL Pointer");
			return rval;
		}
	}

	/* Access check of the argument if present */
	if (_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok(VERIFY_WRITE, (void *)arg, _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		err = !access_ok(VERIFY_READ, (void *)arg, _IOC_SIZE(cmd));

	if (err) {
		dev_err(xsdfec->dev, "Invalid xilinx sdfec ioctl argument");
		return -EFAULT;
	}

	switch (cmd) {
	case XSDFEC_START_DEV:
		rval = xsdfec_start(xsdfec);
		break;
	case XSDFEC_STOP_DEV:
		rval = xsdfec_stop(xsdfec);
		break;
	case XSDFEC_CLEAR_STATS:
		rval = xsdfec_clear_stats(xsdfec);
		break;
	case XSDFEC_GET_STATS:
		rval = xsdfec_get_stats(xsdfec, arg);
		break;
	case XSDFEC_GET_STATUS:
		rval = xsdfec_get_status(xsdfec, arg);
		break;
	case XSDFEC_GET_CONFIG:
		rval = xsdfec_get_config(xsdfec, arg);
		break;
	case XSDFEC_SET_DEFAULT_CONFIG:
		rval = xsdfec_set_default_config(xsdfec);
		break;
	case XSDFEC_SET_IRQ:
		rval = xsdfec_set_irq(xsdfec, arg);
		break;
	case XSDFEC_SET_TURBO:
		rval = xsdfec_set_turbo(xsdfec, arg);
		break;
	case XSDFEC_GET_TURBO:
		rval = xsdfec_get_turbo(xsdfec, arg);
		break;
	case XSDFEC_ADD_LDPC_CODE_PARAMS:
		rval  = xsdfec_add_ldpc(xsdfec, arg);
		break;
	case XSDFEC_GET_LDPC_CODE_PARAMS:
		rval = xsdfec_get_ldpc_code_params(xsdfec, arg);
		break;
	case XSDFEC_SET_ORDER:
		rval = xsdfec_set_order(xsdfec, arg);
		break;
	case XSDFEC_SET_BYPASS:
		rval = xsdfec_set_bypass(xsdfec, arg);
		break;
	case XSDFEC_IS_ACTIVE:
		rval = xsdfec_is_active(xsdfec, (bool __user *)arg);
		break;
	default:
		/* Should not get here */
		dev_err(xsdfec->dev, "Undefined SDFEC IOCTL");
		break;
	}
	return rval;
}

static unsigned int
xsdfec_poll(struct file *file, poll_table *wait)
{
	unsigned int mask;
	struct xsdfec_dev *xsdfec = file->private_data;

	if (!xsdfec)
		return POLLNVAL | POLLHUP;

	poll_wait(file, &xsdfec->waitq, wait);

	/* XSDFEC ISR detected an error */
	if (xsdfec->state == XSDFEC_NEEDS_RESET)
		mask = POLLIN | POLLRDNORM;
	else
		mask = POLLPRI | POLLERR;

	return mask;
}

static const struct file_operations xsdfec_fops = {
	.owner = THIS_MODULE,
	.open = xsdfec_dev_open,
	.release = xsdfec_dev_release,
	.unlocked_ioctl = xsdfec_dev_ioctl,
	.poll = xsdfec_poll,
};

static int
xsdfec_parse_of(struct xsdfec_dev *xsdfec)
{
	struct device *dev = xsdfec->dev;
	struct device_node *node = dev->of_node;
	int rval;
	const char *fec_code;
	u32 din_width;
	u32 din_word_include;
	u32 dout_width;
	u32 dout_word_include;

	rval = of_property_read_string(node, "xlnx,sdfec-code", &fec_code);
	if (rval < 0) {
		dev_err(dev, "xlnx,sdfec-code not in DT");
		return rval;
	}

	if (!strcasecmp(fec_code, "ldpc")) {
		xsdfec->config.code = XSDFEC_LDPC_CODE;
	} else if (!strcasecmp(fec_code, "turbo")) {
		xsdfec->config.code = XSDFEC_TURBO_CODE;
	} else {
		dev_err(xsdfec->dev, "Invalid Code in DT");
		return -EINVAL;
	}

	rval = of_property_read_u32(node, "xlnx,sdfec-din-words",
				    &din_word_include);
	if (rval < 0) {
		dev_err(dev, "xlnx,sdfec-din-words not in DT");
		return rval;
	}

	if (din_word_include < XSDFEC_AXIS_WORDS_INCLUDE_MAX) {
		xsdfec->config.din_word_include = din_word_include;
	} else {
		dev_err(xsdfec->dev, "Invalid DIN Words in DT");
		return -EINVAL;
	}

	rval = of_property_read_u32(node, "xlnx,sdfec-din-width", &din_width);
	if (rval < 0) {
		dev_err(dev, "xlnx,sdfec-din-width not in DT");
		return rval;
	}

	switch (din_width) {
	/* Fall through and set for valid values */
	case XSDFEC_1x128b:
	case XSDFEC_2x128b:
	case XSDFEC_4x128b:
		xsdfec->config.din_width = din_width;
		break;
	default:
		dev_err(xsdfec->dev, "Invalid DIN Width in DT");
		return -EINVAL;
	}

	rval = of_property_read_u32(node, "xlnx,sdfec-dout-words",
				    &dout_word_include);
	if (rval < 0) {
		dev_err(dev, "xlnx,sdfec-dout-words not in DT");
		return rval;
	}

	if (dout_word_include < XSDFEC_AXIS_WORDS_INCLUDE_MAX) {
		xsdfec->config.dout_word_include = dout_word_include;
	} else {
		dev_err(xsdfec->dev, "Invalid DOUT Words in DT");
		return -EINVAL;
	}

	rval = of_property_read_u32(node, "xlnx,sdfec-dout-width", &dout_width);
	if (rval < 0) {
		dev_err(dev, "xlnx,sdfec-dout-width not in DT");
		return rval;
	}

	switch (dout_width) {
	/* Fall through and set for valid values */
	case XSDFEC_1x128b:
	case XSDFEC_2x128b:
	case XSDFEC_4x128b:
		xsdfec->config.dout_width = dout_width;
		break;
	default:
		dev_err(xsdfec->dev, "Invalid DOUT Width in DT");
		return -EINVAL;
	}

	/* Write LDPC to CODE Register */
	xsdfec_regwrite(xsdfec, XSDFEC_FEC_CODE_ADDR, xsdfec->config.code - 1);

	xsdfec_cfg_axi_streams(xsdfec);

	return 0;
}

static void
xsdfec_log_ecc_errors(struct xsdfec_dev *xsdfec, u32 ecc_err)
{
	u32 cecc, uecc;
	int uecc_cnt;

	cecc = ecc_err & XSDFEC_ECC_ISR_SBE;
	uecc = ecc_err & XSDFEC_ECC_ISR_MBE;

	uecc_cnt = atomic_add_return(hweight32(uecc), &xsdfec->uecc_count);
	atomic_add(hweight32(cecc), &xsdfec->cecc_count);

	if (uecc_cnt > 0 && uecc_cnt < XSDFEC_ERROR_MAX_THRESHOLD) {
		dev_err(xsdfec->dev,
			"Multi-bit error on xsdfec%d. Needs reset",
			xsdfec->config.fec_id);
	}

	/* Clear ECC errors */
	xsdfec_regwrite(xsdfec, XSDFEC_ECC_ISR_ADDR, 0);
}

static void
xsdfec_log_isr_errors(struct xsdfec_dev *xsdfec, u32 isr_err)
{
	int isr_err_cnt;

	/* Update ISR error counts */
	isr_err_cnt = atomic_add_return(hweight32(isr_err),
					&xsdfec->isr_err_count);
	if (isr_err_cnt > 0 && isr_err_cnt < XSDFEC_ERROR_MAX_THRESHOLD) {
		dev_err(xsdfec->dev,
			"Tlast,or DIN_WORDS or DOUT_WORDS not correct");
	}

	/* Clear ISR error status */
	xsdfec_regwrite(xsdfec, XSDFEC_ECC_ISR_ADDR, 0);
}

static void
xsdfec_reset_required(struct xsdfec_dev *xsdfec)
{
	xsdfec->state = XSDFEC_NEEDS_RESET;
}

static irqreturn_t
xsdfec_irq_thread(int irq, void *dev_id)
{
	struct xsdfec_dev *xsdfec = dev_id;
	irqreturn_t ret = IRQ_HANDLED;
	u32 ecc_err;
	u32 isr_err;
	bool fatal_err = false;

	WARN_ON(xsdfec->irq != irq);

	/* Mask Interrupts */
	xsdfec_isr_enable(xsdfec, false);
	xsdfec_ecc_isr_enable(xsdfec, false);

	/* Read Interrupt Status Registers */
	ecc_err = xsdfec_regread(xsdfec, XSDFEC_ECC_ISR_ADDR);
	isr_err = xsdfec_regread(xsdfec, XSDFEC_ISR_ADDR);

	if (ecc_err & XSDFEC_ECC_ISR_MBE) {
		/* Multi-Bit Errors need Reset */
		xsdfec_log_ecc_errors(xsdfec, ecc_err);
		xsdfec_reset_required(xsdfec);
		fatal_err = true;
	} else if (isr_err & XSDFEC_ISR_MASK) {
		/*
		 * Tlast, DIN_WORDS and DOUT_WORDS related
		 * errors need Reset
		 */
		xsdfec_log_isr_errors(xsdfec, isr_err);
		xsdfec_reset_required(xsdfec);
		fatal_err = true;
	} else if (ecc_err & XSDFEC_ECC_ISR_SBE) {
		/* Correctable ECC Errors */
		xsdfec_log_ecc_errors(xsdfec, ecc_err);
	} else {
		ret = IRQ_NONE;
	}

	if (fatal_err)
		wake_up_interruptible(&xsdfec->waitq);

	/* Unmaks Interrupts */
	xsdfec_isr_enable(xsdfec, true);
	xsdfec_ecc_isr_enable(xsdfec, true);

	return ret;
}

static int
xsdfec_probe(struct platform_device *pdev)
{
	struct xsdfec_dev *xsdfec;
	struct device *dev;
	struct device *dev_create;
	struct resource *res;
	int err;
	bool irq_enabled = true;

	xsdfec = devm_kzalloc(&pdev->dev, sizeof(*xsdfec), GFP_KERNEL);
	if (!xsdfec)
		return -ENOMEM;

	xsdfec->dev = &pdev->dev;
	xsdfec->config.fec_id = atomic_read(&xsdfec_ndevs);

	dev = xsdfec->dev;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	xsdfec->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(xsdfec->regs)) {
		dev_err(dev, "Unable to map resource");
		err = PTR_ERR(xsdfec->regs);
		goto err_xsdfec_dev;
	}

	xsdfec->irq = platform_get_irq(pdev, 0);
	if (xsdfec->irq < 0) {
		dev_dbg(dev, "platform_get_irq failed");
		irq_enabled = false;
	}

	err = xsdfec_parse_of(xsdfec);
	if (err < 0)
		goto err_xsdfec_dev;

	/* Save driver private data */
	platform_set_drvdata(pdev, xsdfec);

	if (irq_enabled) {
		init_waitqueue_head(&xsdfec->waitq);
		/* Register IRQ thread */
		err = devm_request_threaded_irq(dev, xsdfec->irq, NULL,
						xsdfec_irq_thread,
						IRQF_ONESHOT,
						"xilinx-sdfec16",
						xsdfec);
		if (err < 0) {
			dev_err(dev, "unable to request IRQ%d", xsdfec->irq);
			goto err_xsdfec_dev;
		}
	}

	cdev_init(&xsdfec->xsdfec_cdev, &xsdfec_fops);
	xsdfec->xsdfec_cdev.owner = THIS_MODULE;
	err = cdev_add(&xsdfec->xsdfec_cdev,
		       MKDEV(MAJOR(xsdfec_devt), xsdfec->config.fec_id), 1);
	if (err < 0) {
		dev_err(dev, "cdev_add failed");
		err = -EIO;
		goto err_xsdfec_dev;
	}

	if (!xsdfec_class) {
		err = -EIO;
		dev_err(dev, "xsdfec class not created correctly");
		goto err_xsdfec_cdev;
	}

	dev_create = device_create(xsdfec_class, dev,
				   MKDEV(MAJOR(xsdfec_devt),
					 xsdfec->config.fec_id),
				   xsdfec, "xsdfec%d", xsdfec->config.fec_id);
	if (IS_ERR(dev_create)) {
		dev_err(dev, "unable to create device");
		err = PTR_ERR(dev_create);
		goto err_xsdfec_cdev;
	}

	atomic_set(&xsdfec->open_count, 1);
	dev_info(dev, "XSDFEC%d Probe Successful", xsdfec->config.fec_id);
	atomic_inc(&xsdfec_ndevs);
	return 0;

	/* Failure cleanup */
err_xsdfec_cdev:
	cdev_del(&xsdfec->xsdfec_cdev);
err_xsdfec_dev:
	return err;
}

static int
xsdfec_remove(struct platform_device *pdev)
{
	struct xsdfec_dev *xsdfec;
	struct device *dev = &pdev->dev;

	xsdfec = platform_get_drvdata(pdev);
	if (!xsdfec)
		return -ENODEV;
	dev = xsdfec->dev;
	if (!xsdfec_class) {
		dev_err(dev, "xsdfec_class is NULL");
		return -EIO;
	}

	device_destroy(xsdfec_class,
		       MKDEV(MAJOR(xsdfec_devt), xsdfec->config.fec_id));
	cdev_del(&xsdfec->xsdfec_cdev);
	atomic_dec(&xsdfec_ndevs);
	return 0;
}

static const struct of_device_id xsdfec_of_match[] = {
	{ .compatible = "xlnx,sd-fec-1.1", },
	{ /* end of table */ }
};
MODULE_DEVICE_TABLE(of, xsdfec_of_match);

static struct platform_driver xsdfec_driver = {
	.driver = {
		.name = "xilinx-sdfec",
		.of_match_table = xsdfec_of_match,
	},
	.probe = xsdfec_probe,
	.remove =  xsdfec_remove,
};

static int __init xsdfec_init_mod(void)
{
	int err;

	xsdfec_class = class_create(THIS_MODULE, DRIVER_NAME);
	if (IS_ERR(xsdfec_class)) {
		err = PTR_ERR(xsdfec_class);
		pr_err("%s : Unable to register xsdfec class", __func__);
		return err;
	}

	err = alloc_chrdev_region(&xsdfec_devt,
				  0, DRIVER_MAX_DEV, DRIVER_NAME);
	if (err < 0) {
		pr_err("%s : Unable to get major number", __func__);
		goto err_xsdfec_class;
	}

	err = platform_driver_register(&xsdfec_driver);
	if (err < 0) {
		pr_err("%s Unabled to register %s driver",
		       __func__, DRIVER_NAME);
		goto err_xsdfec_drv;
	}
	return 0;

	/* Error Path */
err_xsdfec_drv:
	unregister_chrdev_region(xsdfec_devt, DRIVER_MAX_DEV);
err_xsdfec_class:
	class_destroy(xsdfec_class);
	return err;
}

static void __exit xsdfec_cleanup_mod(void)
{
	platform_driver_unregister(&xsdfec_driver);
	unregister_chrdev_region(xsdfec_devt, DRIVER_MAX_DEV);
	class_destroy(xsdfec_class);
	xsdfec_class = NULL;
}

module_init(xsdfec_init_mod);
module_exit(xsdfec_cleanup_mod);

MODULE_AUTHOR("Xilinx, Inc");
MODULE_DESCRIPTION("Xilinx SD-FEC16 Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);
