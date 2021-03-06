/*
 * Copyright(c) 2019 Xilinx, Inc. All rights reserved.
 *
 * BSD LICENSE
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of the copyright holder nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "qdma_cpm_access.h"
#include "qdma_reg.h"
#include "qdma_cpm_reg.h"
#include "qdma_platform.h"
#include "qdma_access_common.h"
#include "qdma_reg_dump.h"


/** QDMA CPM Context array size */
#define QDMA_CPM_SW_CONTEXT_NUM_WORDS              4
#define QDMA_CPM_CMPT_CONTEXT_NUM_WORDS            4
#define QDMA_CPM_QID2VEC_CONTEXT_NUM_WORDS         1
#define QDMA_CPM_HW_CONTEXT_NUM_WORDS              2
#define QDMA_CPM_CR_CONTEXT_NUM_WORDS              1
#define QDMA_CPM_IND_INTR_CONTEXT_NUM_WORDS        3
#define QDMA_CPM_PFETCH_CONTEXT_NUM_WORDS          2

#define QDMA_CPM_VF_USER_BAR_ID   2

union qdma_cpm_ind_ctxt_cmd {
	uint32_t word;
	struct {
		uint32_t busy:1;
		uint32_t sel:4;
		uint32_t op:2;
		uint32_t qid:11;
		uint32_t rsvd:14;
	} bits;
};

struct qdma_cpm_indirect_ctxt_regs {
	uint32_t qdma_ind_ctxt_data[QDMA_CPM_IND_CTXT_DATA_NUM_REGS];
	uint32_t qdma_ind_ctxt_mask[QDMA_CPM_IND_CTXT_DATA_NUM_REGS];
	union qdma_cpm_ind_ctxt_cmd cmd;
};

static int cpm_hw_monitor_reg(void *dev_hndl, unsigned int reg, uint32_t mask,
		uint32_t val, unsigned int interval_us,
		unsigned int timeout_us);
static int qdma_cpm_indirect_reg_invalidate(void *dev_hndl,
		enum ind_ctxt_cmd_sel sel, uint16_t hw_qid);
static int qdma_cpm_indirect_reg_clear(void *dev_hndl,
		enum ind_ctxt_cmd_sel sel, uint16_t hw_qid);
static int qdma_cpm_indirect_reg_read(void *dev_hndl,
		enum ind_ctxt_cmd_sel sel,
		uint16_t hw_qid, uint32_t cnt, uint32_t *data);
static int qdma_cpm_indirect_reg_write(void *dev_hndl,
		enum ind_ctxt_cmd_sel sel,
		uint16_t hw_qid, uint32_t *data, uint16_t cnt);

/*
 * cpm_hw_monitor_reg() - polling a register repeatly until
 *	(the register value & mask) == val or time is up
 *
 * return -QDMA_BUSY_IIMEOUT_ERR if register value didn't match, 0 other wise
 */
static int cpm_hw_monitor_reg(void *dev_hndl, unsigned int reg, uint32_t mask,
		uint32_t val, unsigned int interval_us, unsigned int timeout_us)
{
	int count;
	uint32_t v;

	if (!interval_us)
		interval_us = QDMA_REG_POLL_DFLT_INTERVAL_US;
	if (!timeout_us)
		timeout_us = QDMA_REG_POLL_DFLT_TIMEOUT_US;

	count = (timeout_us / interval_us) + 1;

	do {
		v = qdma_reg_read(dev_hndl, reg);
		if ((v & mask) == val)
			return QDMA_SUCCESS;
		qdma_udelay(interval_us);
	} while (--count);

	qdma_log_error("%s: Reg read=%u Expected=%u, err:%d\n",
				   __func__, v, val,
				   -QDMA_ERR_HWACC_BUSY_TIMEOUT);
	return -QDMA_ERR_HWACC_BUSY_TIMEOUT;
}

/*
 * qdma_cpm_indirect_reg_invalidate() - helper function to invalidate indirect
 *					context registers.
 *
 * return -QDMA_ERR_HWACC_BUSY_TIMEOUT if register
 *	value didn't match, QDMA_SUCCESS other wise
 */
static int qdma_cpm_indirect_reg_invalidate(void *dev_hndl,
		enum ind_ctxt_cmd_sel sel, uint16_t hw_qid)
{
	union qdma_cpm_ind_ctxt_cmd cmd;

	qdma_reg_access_lock(dev_hndl);

	/* set command register */
	cmd.word = 0;
	cmd.bits.qid = hw_qid;
	cmd.bits.op = QDMA_CTXT_CMD_INV;
	cmd.bits.sel = sel;
	qdma_reg_write(dev_hndl, QDMA_CPM_OFFSET_IND_CTXT_CMD, cmd.word);

	/* check if the operation went through well */
	if (cpm_hw_monitor_reg(dev_hndl, QDMA_CPM_OFFSET_IND_CTXT_CMD,
			QDMA_IND_CTXT_CMD_BUSY_MASK, 0,
			QDMA_REG_POLL_DFLT_INTERVAL_US,
			QDMA_REG_POLL_DFLT_TIMEOUT_US)) {
		qdma_reg_access_release(dev_hndl);
		qdma_log_error("%s: cpm_hw_monitor_reg failed, err:%d\n",
					__func__,
					-QDMA_ERR_HWACC_BUSY_TIMEOUT);
		return -QDMA_ERR_HWACC_BUSY_TIMEOUT;
	}

	qdma_reg_access_release(dev_hndl);

	return QDMA_SUCCESS;
}

/*
 * qdma_cpm_indirect_reg_clear() - helper function to clear indirect
 *				context registers.
 *
 * return -QDMA_ERR_HWACC_BUSY_TIMEOUT if register
 *	value didn't match, QDMA_SUCCESS other wise
 */
static int qdma_cpm_indirect_reg_clear(void *dev_hndl,
		enum ind_ctxt_cmd_sel sel, uint16_t hw_qid)
{
	union qdma_cpm_ind_ctxt_cmd cmd;

	qdma_reg_access_lock(dev_hndl);

	/* set command register */
	cmd.word = 0;
	cmd.bits.qid = hw_qid;
	cmd.bits.op = QDMA_CTXT_CMD_CLR;
	cmd.bits.sel = sel;

	qdma_reg_write(dev_hndl, QDMA_CPM_OFFSET_IND_CTXT_CMD, cmd.word);

	/* check if the operation went through well */
	if (cpm_hw_monitor_reg(dev_hndl, QDMA_CPM_OFFSET_IND_CTXT_CMD,
			QDMA_IND_CTXT_CMD_BUSY_MASK, 0,
			QDMA_REG_POLL_DFLT_INTERVAL_US,
			QDMA_REG_POLL_DFLT_TIMEOUT_US)) {
		qdma_reg_access_release(dev_hndl);
		qdma_log_error("%s: cpm_hw_monitor_reg failed, err:%d\n",
					__func__,
					-QDMA_ERR_HWACC_BUSY_TIMEOUT);
		return -QDMA_ERR_HWACC_BUSY_TIMEOUT;
	}

	qdma_reg_access_release(dev_hndl);

	return QDMA_SUCCESS;
}

/*
 * qdma_cpm_indirect_reg_read() - helper function to read indirect
 *				context registers.
 *
 * return -QDMA_ERR_HWACC_BUSY_TIMEOUT if register
 *	value didn't match, QDMA_SUCCESS other wise
 */
static int qdma_cpm_indirect_reg_read(void *dev_hndl,
		enum ind_ctxt_cmd_sel sel,
		uint16_t hw_qid, uint32_t cnt, uint32_t *data)
{
	uint32_t index = 0, reg_addr = QDMA_OFFSET_IND_CTXT_DATA;
	union qdma_cpm_ind_ctxt_cmd cmd;

	qdma_reg_access_lock(dev_hndl);

	/* set command register */
	cmd.word = 0;
	cmd.bits.qid = hw_qid;
	cmd.bits.op = QDMA_CTXT_CMD_RD;
	cmd.bits.sel = sel;
	qdma_reg_write(dev_hndl, QDMA_CPM_OFFSET_IND_CTXT_CMD, cmd.word);

	/* check if the operation went through well */
	if (cpm_hw_monitor_reg(dev_hndl, QDMA_CPM_OFFSET_IND_CTXT_CMD,
			QDMA_IND_CTXT_CMD_BUSY_MASK, 0,
			QDMA_REG_POLL_DFLT_INTERVAL_US,
			QDMA_REG_POLL_DFLT_TIMEOUT_US)) {
		qdma_reg_access_release(dev_hndl);
		qdma_log_error("%s: cpm_hw_monitor_reg failed, err:%d\n",
					__func__,
					-QDMA_ERR_HWACC_BUSY_TIMEOUT);
		return -QDMA_ERR_HWACC_BUSY_TIMEOUT;
	}

	for (index = 0; index < cnt; index++, reg_addr += sizeof(uint32_t))
		data[index] = qdma_reg_read(dev_hndl, reg_addr);

	qdma_reg_access_release(dev_hndl);

	return QDMA_SUCCESS;
}

/*
 * qdma_cpm_indirect_reg_write() - helper function to write indirect
 *				context registers.
 *
 * return -QDMA_ERR_HWACC_BUSY_TIMEOUT if register
 *	value didn't match, QDMA_SUCCESS other wise
 */
static int qdma_cpm_indirect_reg_write(void *dev_hndl,
		enum ind_ctxt_cmd_sel sel,
		uint16_t hw_qid, uint32_t *data, uint16_t cnt)
{
	uint32_t index, reg_addr;
	struct qdma_cpm_indirect_ctxt_regs regs;
	uint32_t *wr_data = (uint32_t *)&regs;

	qdma_reg_access_lock(dev_hndl);

	/* write the context data */
	for (index = 0; index < QDMA_CPM_IND_CTXT_DATA_NUM_REGS; index++) {
		if (index < cnt)
			regs.qdma_ind_ctxt_data[index] = data[index];
		else
			regs.qdma_ind_ctxt_data[index] = 0;
		regs.qdma_ind_ctxt_mask[index] = 0xFFFFFFFF;
	}

	regs.cmd.word = 0;
	regs.cmd.bits.qid = hw_qid;
	regs.cmd.bits.op = QDMA_CTXT_CMD_WR;
	regs.cmd.bits.sel = sel;
	reg_addr = QDMA_OFFSET_IND_CTXT_DATA;

	for (index = 0; index < ((2 * QDMA_CPM_IND_CTXT_DATA_NUM_REGS) + 1);
			index++, reg_addr += sizeof(uint32_t))
		qdma_reg_write(dev_hndl, reg_addr, wr_data[index]);

	/* check if the operation went through well */
	if (cpm_hw_monitor_reg(dev_hndl, QDMA_CPM_OFFSET_IND_CTXT_CMD,
			QDMA_IND_CTXT_CMD_BUSY_MASK, 0,
			QDMA_REG_POLL_DFLT_INTERVAL_US,
			QDMA_REG_POLL_DFLT_TIMEOUT_US)) {
		qdma_reg_access_release(dev_hndl);
		qdma_log_error("%s: cpm_hw_monitor_reg failed, err:%d\n",
						__func__,
					   -QDMA_ERR_HWACC_BUSY_TIMEOUT);
		return -QDMA_ERR_HWACC_BUSY_TIMEOUT;
	}

	qdma_reg_access_release(dev_hndl);

	return QDMA_SUCCESS;
}

/*****************************************************************************/
/**
 * qdma_cpm_qid2vec_write() - create qid2vec context and program it
 *
 * @dev_hndl:	device handle
 * @c2h:	is c2h queue
 * @hw_qid:	hardware qid of the queue
 * @ctxt:	pointer to the context data
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_qid2vec_write(void *dev_hndl, uint8_t c2h, uint16_t hw_qid,
				struct qdma_qid2vec *ctxt)
{
	uint32_t qid2vec = 0;
	enum ind_ctxt_cmd_sel sel = QDMA_CTXT_SEL_FMAP;

	qdma_cpm_indirect_reg_read(dev_hndl, sel, hw_qid, 1, &qid2vec);
	if (c2h) {
		qid2vec = qid2vec & (QDMA_CPM_QID2VEC_H2C_VECTOR |
						QDMA_CPM_QID2VEC_H2C_COAL_EN);
		qid2vec |= FIELD_SET(QDMA_CPM_QID2VEC_C2H_VECTOR,
				     ctxt->c2h_vector) |
			FIELD_SET(QDMA_CPM_QID2VEC_C2H_COAL_EN,
				  ctxt->c2h_en_coal);
	} else {
		qid2vec = qid2vec & (QDMA_CPM_QID2VEC_C2H_VECTOR |
						QDMA_CPM_QID2VEC_C2H_COAL_EN);
		qid2vec |=
			FIELD_SET(QDMA_CPM_QID2VEC_H2C_VECTOR,
				  ctxt->h2c_vector) |
			FIELD_SET(QDMA_CPM_QID2VEC_H2C_COAL_EN,
				  ctxt->h2c_en_coal);
	}

	qdma_cpm_indirect_reg_write(dev_hndl, sel, hw_qid,
				&qid2vec, QDMA_CPM_QID2VEC_CONTEXT_NUM_WORDS);

	return QDMA_SUCCESS;
}

/*****************************************************************************/
/**
 * qdma_cpm_qid2vec_read() - read qid2vec context
 *
 * @dev_hndl:	device handle
 * @c2h:	is c2h queue
 * @hw_qid:	hardware qid of the queue
 * @ctxt:	pointer to the context data
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_qid2vec_read(void *dev_hndl, uint8_t c2h, uint16_t hw_qid,
				struct qdma_qid2vec *ctxt)
{
	int rv = 0;
	uint32_t qid2vec[QDMA_CPM_QID2VEC_CONTEXT_NUM_WORDS] = {0};
	enum ind_ctxt_cmd_sel sel = QDMA_CTXT_SEL_FMAP;

	if (!dev_hndl || !ctxt) {
		qdma_log_error("%s: dev_hndl=%p qid2vec=%p, err:%d\n",
					   __func__, dev_hndl, ctxt,
					   -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	rv = qdma_cpm_indirect_reg_read(dev_hndl, sel, hw_qid,
				QDMA_CPM_QID2VEC_CONTEXT_NUM_WORDS, qid2vec);
	if (rv < 0)
		return rv;

	if (c2h) {
		ctxt->c2h_vector = FIELD_GET(QDMA_CPM_QID2VEC_C2H_VECTOR,
						qid2vec[0]);
		ctxt->c2h_en_coal =
			(uint8_t)(FIELD_GET(QDMA_CPM_QID2VEC_C2H_COAL_EN,
						qid2vec[0]));
	} else {
		ctxt->h2c_vector =
			(uint8_t)(FIELD_GET(QDMA_CPM_QID2VEC_H2C_VECTOR,
								qid2vec[0]));
		ctxt->h2c_en_coal =
			(uint8_t)(FIELD_GET(QDMA_CPM_QID2VEC_H2C_COAL_EN,
								qid2vec[0]));
	}

	return QDMA_SUCCESS;
}

/*****************************************************************************/
/**
 * qdma_cpm_qid2vec_clear() - clear qid2vec context
 *
 * @dev_hndl:	device handle
 * @hw_qid:	hardware qid of the queue
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_qid2vec_clear(void *dev_hndl, uint16_t hw_qid)
{
	enum ind_ctxt_cmd_sel sel = QDMA_CTXT_SEL_FMAP;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	return qdma_cpm_indirect_reg_clear(dev_hndl, sel, hw_qid);
}

/*****************************************************************************/
/**
 * qdma_cpm_qid2vec_invalidate() - invalidate qid2vec context
 *
 * @dev_hndl:	device handle
 * @hw_qid:	hardware qid of the queue
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_qid2vec_invalidate(void *dev_hndl, uint16_t hw_qid)
{
	enum ind_ctxt_cmd_sel sel = QDMA_CTXT_SEL_FMAP;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	return qdma_cpm_indirect_reg_invalidate(dev_hndl, sel, hw_qid);
}

/*****************************************************************************/
/**
 * qdma_cpm_qid2vec_conf() - configure qid2vector context
 *
 * @dev_hndl:	device handle
 * @c2h:	is c2h queue
 * @hw_qid:	hardware qid of the queue
 * @ctxt:	pointer to the context data
 * @access_type HW access type (qdma_hw_access_type enum) value
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
int qdma_cpm_qid2vec_conf(void *dev_hndl, uint8_t c2h, uint16_t hw_qid,
			 struct qdma_qid2vec *ctxt,
			 enum qdma_hw_access_type access_type)
{
	int ret_val = 0;

	switch (access_type) {
	case QDMA_HW_ACCESS_READ:
		ret_val = qdma_cpm_qid2vec_read(dev_hndl, c2h, hw_qid, ctxt);
		break;
	case QDMA_HW_ACCESS_WRITE:
		ret_val = qdma_cpm_qid2vec_write(dev_hndl, c2h, hw_qid, ctxt);
		break;
	case QDMA_HW_ACCESS_CLEAR:
		ret_val = qdma_cpm_qid2vec_clear(dev_hndl, hw_qid);
		break;
	case QDMA_HW_ACCESS_INVALIDATE:
		ret_val = qdma_cpm_qid2vec_invalidate(dev_hndl, hw_qid);
		break;
	default:
		qdma_log_error("%s: access_type=%d is invalid, err:%d\n",
					   __func__, access_type,
					   -QDMA_ERR_INV_PARAM);
		ret_val = -QDMA_ERR_INV_PARAM;
		break;
	}

	return ret_val;
}

/*****************************************************************************/
/**
 * qdma_cpm_fmap_write() - create fmap context and program it
 *
 * @dev_hndl:	device handle
 * @func_id:	function id of the device
 * @config:	pointer to the fmap data strucutre
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_fmap_write(void *dev_hndl, uint16_t func_id,
		   const struct qdma_fmap_cfg *config)
{
	uint32_t fmap = 0;

	if (!dev_hndl || !config) {
		qdma_log_error("%s: dev_handle or config is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	fmap = FIELD_SET(QDMA_FMAP_CTXT_W0_QID_MASK, config->qbase) |
		FIELD_SET(QDMA_CPM_FMAP_CTXT_W0_QID_MAX_MASK, config->qmax);

	qdma_reg_write(dev_hndl, QDMA_CPM_REG_TRQ_SEL_FMAP_BASE +
			func_id * QDMA_CPM_REG_TRQ_SEL_FMAP_STEP,
			fmap);
	return QDMA_SUCCESS;
}

/*****************************************************************************/
/**
 * qdma_cpm_fmap_read() - read fmap context
 *
 * @dev_hndl:	device handle
 * @func_id:	function id of the device
 * @config:	pointer to the output fmap data
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_fmap_read(void *dev_hndl, uint16_t func_id,
			 struct qdma_fmap_cfg *config)
{
	uint32_t fmap = 0;

	if (!dev_hndl || !config) {
		qdma_log_error("%s: dev_handle=%p fmap=%p NULL, err:%d\n",
						__func__, dev_hndl, config,
					   -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	fmap = qdma_reg_read(dev_hndl, QDMA_CPM_REG_TRQ_SEL_FMAP_BASE +
			     func_id * QDMA_CPM_REG_TRQ_SEL_FMAP_STEP);

	config->qbase = FIELD_GET(QDMA_FMAP_CTXT_W0_QID_MASK, fmap);
	config->qmax =
		(uint16_t)(FIELD_GET(QDMA_CPM_FMAP_CTXT_W0_QID_MAX_MASK, fmap));

	return QDMA_SUCCESS;
}

/*****************************************************************************/
/**
 * qdma_cpm_fmap_clear() - clear fmap context
 *
 * @dev_hndl:	device handle
 * @func_id:	function id of the device
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_fmap_clear(void *dev_hndl, uint16_t func_id)
{
	uint32_t fmap = 0;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	qdma_reg_write(dev_hndl, QDMA_CPM_REG_TRQ_SEL_FMAP_BASE +
			func_id * QDMA_CPM_REG_TRQ_SEL_FMAP_STEP,
			fmap);

	return QDMA_SUCCESS;
}

/*****************************************************************************/
/**
 * qdma_cpm_fmap_conf() - configure fmap context
 *
 * @dev_hndl:	device handle
 * @func_id:	function id of the device
 * @config:	pointer to the fmap data
 * @access_type HW access type (qdma_hw_access_type enum) value
 *		QDMA_HW_ACCESS_INVALIDATE unsupported
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
int qdma_cpm_fmap_conf(void *dev_hndl, uint16_t func_id,
				struct qdma_fmap_cfg *config,
				enum qdma_hw_access_type access_type)
{
	int ret_val = 0;

	switch (access_type) {
	case QDMA_HW_ACCESS_READ:
		ret_val = qdma_cpm_fmap_read(dev_hndl, func_id, config);
		break;
	case QDMA_HW_ACCESS_WRITE:
		ret_val = qdma_cpm_fmap_write(dev_hndl, func_id, config);
		break;
	case QDMA_HW_ACCESS_CLEAR:
		ret_val = qdma_cpm_fmap_clear(dev_hndl, func_id);
		break;
	case QDMA_HW_ACCESS_INVALIDATE:
	default:
		qdma_log_error("%s: access_type=%d is invalid, err:%d\n",
					   __func__, access_type,
					   -QDMA_ERR_INV_PARAM);
		ret_val = -QDMA_ERR_INV_PARAM;
		break;
	}

	return ret_val;
}

/*****************************************************************************/
/**
 * qdma_cpm_sw_context_write() - create sw context and program it
 *
 * @dev_hndl:	device handle
 * @c2h:	is c2h queue
 * @hw_qid:	hardware qid of the queue
 * @ctxt:	pointer to the SW context data strucutre
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_sw_context_write(void *dev_hndl, uint8_t c2h,
			 uint16_t hw_qid,
			 const struct qdma_descq_sw_ctxt *ctxt)
{
	uint32_t sw_ctxt[QDMA_CPM_SW_CONTEXT_NUM_WORDS] = {0};
	uint16_t num_words_count = 0;
	enum ind_ctxt_cmd_sel sel = c2h ?
			QDMA_CTXT_SEL_SW_C2H : QDMA_CTXT_SEL_SW_H2C;

	/* Input args check */
	if (!dev_hndl || !ctxt) {
		qdma_log_error("%s: dev_hndl or ctxt is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	if ((ctxt->desc_sz > QDMA_DESC_SIZE_64B) ||
		(ctxt->rngsz_idx >= QDMA_NUM_RING_SIZES)) {
		qdma_log_error("%s: Invalid desc_sz(%d)/rngidx(%d), err:%d\n",
					__func__,
					ctxt->desc_sz,
					ctxt->rngsz_idx,
					-QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	sw_ctxt[num_words_count++] =
		FIELD_SET(QDMA_SW_CTXT_W0_PIDX, ctxt->pidx) |
		FIELD_SET(QDMA_SW_CTXT_W0_IRQ_ARM_MASK, ctxt->irq_arm);

	sw_ctxt[num_words_count++] =
		FIELD_SET(QDMA_SW_CTXT_W1_QEN_MASK, ctxt->qen) |
		FIELD_SET(QDMA_SW_CTXT_W1_FCRD_EN_MASK, ctxt->frcd_en) |
		FIELD_SET(QDMA_SW_CTXT_W1_WBI_CHK_MASK, ctxt->wbi_chk) |
		FIELD_SET(QDMA_SW_CTXT_W1_WB_INT_EN_MASK, ctxt->wbi_intvl_en) |
		FIELD_SET(QDMA_CPM_SW_CTXT_W1_FUNC_ID_MASK, ctxt->fnc_id) |
		FIELD_SET(QDMA_SW_CTXT_W1_RNG_SZ_MASK, ctxt->rngsz_idx) |
		FIELD_SET(QDMA_SW_CTXT_W1_DSC_SZ_MASK, ctxt->desc_sz) |
		FIELD_SET(QDMA_SW_CTXT_W1_BYP_MASK, ctxt->bypass) |
		FIELD_SET(QDMA_SW_CTXT_W1_MM_CHN_MASK, ctxt->mm_chn) |
		FIELD_SET(QDMA_SW_CTXT_W1_WBK_EN_MASK, ctxt->wbk_en) |
		FIELD_SET(QDMA_SW_CTXT_W1_IRQ_EN_MASK, ctxt->irq_en) |
		FIELD_SET(QDMA_SW_CTXT_W1_PORT_ID_MASK, ctxt->port_id) |
		FIELD_SET(QDMA_SW_CTXT_W1_IRQ_NO_LAST_MASK, ctxt->irq_no_last) |
		FIELD_SET(QDMA_SW_CTXT_W1_ERR_MASK, ctxt->err) |
		FIELD_SET(QDMA_SW_CTXT_W1_ERR_WB_SENT_MASK, ctxt->err_wb_sent) |
		FIELD_SET(QDMA_SW_CTXT_W1_IRQ_REQ_MASK, ctxt->irq_req) |
		FIELD_SET(QDMA_SW_CTXT_W1_MRKR_DIS_MASK, ctxt->mrkr_dis) |
		FIELD_SET(QDMA_SW_CTXT_W1_IS_MM_MASK, ctxt->is_mm);

	sw_ctxt[num_words_count++] = ctxt->ring_bs_addr & 0xffffffff;
	sw_ctxt[num_words_count++] = (ctxt->ring_bs_addr >> 32) & 0xffffffff;

	return qdma_cpm_indirect_reg_write(dev_hndl, sel, hw_qid,
			sw_ctxt, num_words_count);
}

/*****************************************************************************/
/**
 * qdma_cpm_sw_context_read() - read sw context
 *
 * @dev_hndl:	device handle
 * @c2h:	is c2h queue
 * @hw_qid:	hardware qid of the queue
 * @ctxt:	pointer to the output context data
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_sw_context_read(void *dev_hndl, uint8_t c2h,
			 uint16_t hw_qid,
			 struct qdma_descq_sw_ctxt *ctxt)
{
	int rv = 0;
	uint32_t sw_ctxt[QDMA_CPM_SW_CONTEXT_NUM_WORDS] = {0};
	enum ind_ctxt_cmd_sel sel = c2h ?
			QDMA_CTXT_SEL_SW_C2H : QDMA_CTXT_SEL_SW_H2C;
	struct qdma_qid2vec qid2vec_ctxt = {0};

	if (!dev_hndl || !ctxt) {
		qdma_log_error("%s: dev_hndl=%p sw_ctxt=%p, err:%d\n",
					   __func__, dev_hndl, ctxt,
					   -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	rv = qdma_cpm_indirect_reg_read(dev_hndl, sel, hw_qid,
			QDMA_CPM_SW_CONTEXT_NUM_WORDS, sw_ctxt);
	if (rv < 0)
		return rv;

	ctxt->pidx = FIELD_GET(QDMA_SW_CTXT_W0_PIDX, sw_ctxt[0]);
	ctxt->irq_arm =
		(uint8_t)(FIELD_GET(QDMA_SW_CTXT_W0_IRQ_ARM_MASK, sw_ctxt[0]));
	ctxt->fnc_id =
		(uint8_t)(FIELD_GET(QDMA_SW_CTXT_W0_FUNC_ID_MASK, sw_ctxt[0]));

	ctxt->qen = FIELD_GET(QDMA_SW_CTXT_W1_QEN_MASK, sw_ctxt[1]);
	ctxt->frcd_en = FIELD_GET(QDMA_SW_CTXT_W1_FCRD_EN_MASK, sw_ctxt[1]);
	ctxt->wbi_chk = FIELD_GET(QDMA_SW_CTXT_W1_WBI_CHK_MASK, sw_ctxt[1]);
	ctxt->wbi_intvl_en =
			FIELD_GET(QDMA_SW_CTXT_W1_WB_INT_EN_MASK, sw_ctxt[1]);
	ctxt->fnc_id =
		(uint8_t)(FIELD_GET(QDMA_CPM_SW_CTXT_W1_FUNC_ID_MASK,
			sw_ctxt[1]));
	ctxt->rngsz_idx =
		(uint8_t)(FIELD_GET(QDMA_SW_CTXT_W1_RNG_SZ_MASK, sw_ctxt[1]));
	ctxt->desc_sz =
		(uint8_t)(FIELD_GET(QDMA_SW_CTXT_W1_DSC_SZ_MASK, sw_ctxt[1]));
	ctxt->bypass =
		(uint8_t)(FIELD_GET(QDMA_SW_CTXT_W1_BYP_MASK, sw_ctxt[1]));
	ctxt->mm_chn =
		(uint8_t)(FIELD_GET(QDMA_SW_CTXT_W1_MM_CHN_MASK, sw_ctxt[1]));
	ctxt->wbk_en =
		(uint8_t)(FIELD_GET(QDMA_SW_CTXT_W1_WBK_EN_MASK, sw_ctxt[1]));
	ctxt->irq_en =
		(uint8_t)(FIELD_GET(QDMA_SW_CTXT_W1_IRQ_EN_MASK, sw_ctxt[1]));
	ctxt->port_id =
		(uint8_t)(FIELD_GET(QDMA_SW_CTXT_W1_PORT_ID_MASK, sw_ctxt[1]));
	ctxt->irq_no_last =
		(uint8_t)(FIELD_GET(QDMA_SW_CTXT_W1_IRQ_NO_LAST_MASK,
			sw_ctxt[1]));
	ctxt->err =
		(uint8_t)(FIELD_GET(QDMA_SW_CTXT_W1_ERR_MASK, sw_ctxt[1]));
	ctxt->err_wb_sent =
		(uint8_t)(FIELD_GET(QDMA_SW_CTXT_W1_ERR_WB_SENT_MASK,
			sw_ctxt[1]));
	ctxt->irq_req =
		(uint8_t)(FIELD_GET(QDMA_SW_CTXT_W1_IRQ_REQ_MASK, sw_ctxt[1]));
	ctxt->mrkr_dis =
		(uint8_t)(FIELD_GET(QDMA_SW_CTXT_W1_MRKR_DIS_MASK, sw_ctxt[1]));
	ctxt->is_mm =
		(uint8_t)(FIELD_GET(QDMA_SW_CTXT_W1_IS_MM_MASK, sw_ctxt[1]));

	ctxt->ring_bs_addr = ((uint64_t)sw_ctxt[3] << 32) | (sw_ctxt[2]);

	/** Read the QID2VEC Context Data */
	rv = qdma_cpm_qid2vec_read(dev_hndl, c2h, hw_qid, &qid2vec_ctxt);
	if (c2h) {
		ctxt->vec = qid2vec_ctxt.c2h_vector;
		ctxt->intr_aggr = qid2vec_ctxt.c2h_en_coal;
	} else {
		ctxt->vec = qid2vec_ctxt.h2c_vector;
		ctxt->intr_aggr = qid2vec_ctxt.h2c_en_coal;
	}


	return QDMA_SUCCESS;
}

/*****************************************************************************/
/**
 * qdma_cpm_sw_context_clear() - clear sw context
 *
 * @dev_hndl:	device handle
 * @c2h:	is c2h queue
 * @hw_qid:	hardware qid of the queue
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_sw_context_clear(void *dev_hndl, uint8_t c2h,
			  uint16_t hw_qid)
{
	enum ind_ctxt_cmd_sel sel = c2h ?
			QDMA_CTXT_SEL_SW_C2H : QDMA_CTXT_SEL_SW_H2C;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	return qdma_cpm_indirect_reg_clear(dev_hndl, sel, hw_qid);
}

/*****************************************************************************/
/**
 * qdma_cpm_sw_context_invalidate() - invalidate sw context
 *
 * @dev_hndl:	device handle
 * @c2h:	is c2h queue
 * @hw_qid:	hardware qid of the queue
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_sw_context_invalidate(void *dev_hndl, uint8_t c2h,
		uint16_t hw_qid)
{
	enum ind_ctxt_cmd_sel sel = c2h ?
			QDMA_CTXT_SEL_SW_C2H : QDMA_CTXT_SEL_SW_H2C;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	return qdma_cpm_indirect_reg_invalidate(dev_hndl, sel, hw_qid);
}

/*****************************************************************************/
/**
 * qdma_cpm_sw_ctx_conf() - configure SW context
 *
 * @dev_hndl:	device handle
 * @c2h:	is c2h queue
 * @hw_qid:	hardware qid of the queue
 * @ctxt:	pointer to the context data
 * @access_type HW access type (qdma_hw_access_type enum) value
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
int qdma_cpm_sw_ctx_conf(void *dev_hndl, uint8_t c2h, uint16_t hw_qid,
				struct qdma_descq_sw_ctxt *ctxt,
				enum qdma_hw_access_type access_type)
{
	int ret_val = 0;

	switch (access_type) {
	case QDMA_HW_ACCESS_READ:
		ret_val = qdma_cpm_sw_context_read(dev_hndl, c2h, hw_qid,
				ctxt);
		break;
	case QDMA_HW_ACCESS_WRITE:
		ret_val = qdma_cpm_sw_context_write(dev_hndl, c2h, hw_qid,
				ctxt);
		break;
	case QDMA_HW_ACCESS_CLEAR:
		ret_val = qdma_cpm_sw_context_clear(dev_hndl, c2h, hw_qid);
		break;
	case QDMA_HW_ACCESS_INVALIDATE:
		ret_val = qdma_cpm_sw_context_invalidate(dev_hndl, c2h, hw_qid);
		break;
	default:
		qdma_log_error("%s: access_type=%d is invalid, err:%d\n",
					   __func__, access_type,
					   -QDMA_ERR_INV_PARAM);
		ret_val = -QDMA_ERR_INV_PARAM;
		break;
	}

	return ret_val;
}

/*****************************************************************************/
/**
 * qdma_cpm_pfetch_context_write() - create prefetch context and program it
 *
 * @dev_hndl:	device handle
 * @hw_qid:	hardware qid of the queue
 * @ctxt:	pointer to the prefetch context data strucutre
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_pfetch_context_write(void *dev_hndl, uint16_t hw_qid,
		const struct qdma_descq_prefetch_ctxt *ctxt)
{
	uint32_t pfetch_ctxt[QDMA_CPM_PFETCH_CONTEXT_NUM_WORDS] = {0};
	enum ind_ctxt_cmd_sel sel = QDMA_CTXT_SEL_PFTCH;
	uint32_t sw_crdt_l, sw_crdt_h;
	uint16_t num_words_count = 0;

	if (!dev_hndl || !ctxt) {
		qdma_log_error("%s: dev_hndl=%p pfetch_ctxt=%p, err:%d\n",
					   __func__, dev_hndl, ctxt,
					   -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	sw_crdt_l =
		FIELD_GET(QDMA_PFTCH_CTXT_SW_CRDT_GET_L_MASK, ctxt->sw_crdt);
	sw_crdt_h =
		FIELD_GET(QDMA_PFTCH_CTXT_SW_CRDT_GET_H_MASK, ctxt->sw_crdt);

	pfetch_ctxt[num_words_count++] =
		FIELD_SET(QDMA_PFTCH_CTXT_W0_BYPASS_MASK, ctxt->bypass) |
		FIELD_SET(QDMA_PFTCH_CTXT_W0_BUF_SIZE_IDX_MASK,
				ctxt->bufsz_idx) |
		FIELD_SET(QDMA_PFTCH_CTXT_W0_PORT_ID_MASK, ctxt->port_id) |
		FIELD_SET(QDMA_PFTCH_CTXT_W0_ERR_MASK, ctxt->err) |
		FIELD_SET(QDMA_PFTCH_CTXT_W0_PFETCH_EN_MASK, ctxt->pfch_en) |
		FIELD_SET(QDMA_PFTCH_CTXT_W0_Q_IN_PFETCH_MASK, ctxt->pfch) |
		FIELD_SET(QDMA_PFTCH_CTXT_W0_SW_CRDT_L_MASK, sw_crdt_l);

	pfetch_ctxt[num_words_count++] =
		FIELD_SET(QDMA_PFTCH_CTXT_W1_SW_CRDT_H_MASK, sw_crdt_h) |
		FIELD_SET(QDMA_PFTCH_CTXT_W1_VALID_MASK, ctxt->valid);

	return qdma_cpm_indirect_reg_write(dev_hndl, sel, hw_qid,
			pfetch_ctxt, num_words_count);
}

/*****************************************************************************/
/**
 * qdma_cpm_pfetch_context_read() - read prefetch context
 *
 * @dev_hndl:	device handle
 * @hw_qid:	hardware qid of the queue
 * @ctxt:	pointer to the output context data
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_pfetch_context_read(void *dev_hndl, uint16_t hw_qid,
		struct qdma_descq_prefetch_ctxt *ctxt)
{
	int rv = 0;
	uint32_t pfetch_ctxt[QDMA_CPM_PFETCH_CONTEXT_NUM_WORDS] = {0};
	enum ind_ctxt_cmd_sel sel = QDMA_CTXT_SEL_PFTCH;
	uint32_t sw_crdt_l, sw_crdt_h;

	if (!dev_hndl || !ctxt) {
		qdma_log_error("%s: dev_hndl=%p pfetch_ctxt=%p, err:%d\n",
					   __func__, dev_hndl, ctxt,
					   -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	rv = qdma_cpm_indirect_reg_read(dev_hndl, sel, hw_qid,
			QDMA_CPM_PFETCH_CONTEXT_NUM_WORDS, pfetch_ctxt);
	if (rv < 0)
		return rv;

	ctxt->bypass =
		(uint8_t)(FIELD_GET(QDMA_PFTCH_CTXT_W0_BYPASS_MASK,
			pfetch_ctxt[0]));
	ctxt->bufsz_idx =
		(uint8_t)(FIELD_GET(QDMA_PFTCH_CTXT_W0_BUF_SIZE_IDX_MASK,
				pfetch_ctxt[0]));
	ctxt->port_id =
		(uint8_t)(FIELD_GET(QDMA_PFTCH_CTXT_W0_PORT_ID_MASK,
			pfetch_ctxt[0]));
	ctxt->err =
		(uint8_t)(FIELD_GET(QDMA_PFTCH_CTXT_W0_ERR_MASK,
			pfetch_ctxt[0]));
	ctxt->pfch_en =
		(uint8_t)(FIELD_GET(QDMA_PFTCH_CTXT_W0_PFETCH_EN_MASK,
			pfetch_ctxt[0]));
	ctxt->pfch =
		(uint8_t)(FIELD_GET(QDMA_PFTCH_CTXT_W0_Q_IN_PFETCH_MASK,
			pfetch_ctxt[0]));
	sw_crdt_l =
		FIELD_GET(QDMA_PFTCH_CTXT_W0_SW_CRDT_L_MASK, pfetch_ctxt[0]);

	sw_crdt_h =
		FIELD_GET(QDMA_PFTCH_CTXT_W1_SW_CRDT_H_MASK, pfetch_ctxt[1]);
	ctxt->valid =
		(uint8_t)(FIELD_GET(QDMA_PFTCH_CTXT_W1_VALID_MASK,
			pfetch_ctxt[1]));

	ctxt->sw_crdt =
		FIELD_SET(QDMA_PFTCH_CTXT_SW_CRDT_GET_L_MASK, sw_crdt_l) |
		FIELD_SET(QDMA_PFTCH_CTXT_SW_CRDT_GET_H_MASK, sw_crdt_h);

	return QDMA_SUCCESS;
}

/*****************************************************************************/
/**
 * qdma_cpm_pfetch_context_clear() - clear prefetch context
 *
 * @dev_hndl:	device handle
 * @hw_qid:	hardware qid of the queue
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_pfetch_context_clear(void *dev_hndl, uint16_t hw_qid)
{
	enum ind_ctxt_cmd_sel sel = QDMA_CTXT_SEL_PFTCH;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	return qdma_cpm_indirect_reg_clear(dev_hndl, sel, hw_qid);
}

/*****************************************************************************/
/**
 * qdma_cpm_pfetch_context_invalidate() - invalidate prefetch context
 *
 * @dev_hndl:	device handle
 * @hw_qid:	hardware qid of the queue
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_pfetch_context_invalidate(void *dev_hndl, uint16_t hw_qid)
{
	enum ind_ctxt_cmd_sel sel = QDMA_CTXT_SEL_PFTCH;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	return qdma_cpm_indirect_reg_invalidate(dev_hndl, sel, hw_qid);
}

/*****************************************************************************/
/**
 * qdma_cpm_pfetch_ctx_conf() - configure prefetch context
 *
 * @dev_hndl:	device handle
 * @hw_qid:	hardware qid of the queue
 * @ctxt:	pointer to context data
 * @access_type HW access type (qdma_hw_access_type enum) value
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
int qdma_cpm_pfetch_ctx_conf(void *dev_hndl, uint16_t hw_qid,
				struct qdma_descq_prefetch_ctxt *ctxt,
				enum qdma_hw_access_type access_type)
{
	int ret_val = 0;

	switch (access_type) {
	case QDMA_HW_ACCESS_READ:
		ret_val = qdma_cpm_pfetch_context_read(dev_hndl, hw_qid, ctxt);
		break;
	case QDMA_HW_ACCESS_WRITE:
		ret_val = qdma_cpm_pfetch_context_write(dev_hndl, hw_qid, ctxt);
		break;
	case QDMA_HW_ACCESS_CLEAR:
		ret_val = qdma_cpm_pfetch_context_clear(dev_hndl, hw_qid);
		break;
	case QDMA_HW_ACCESS_INVALIDATE:
		ret_val = qdma_cpm_pfetch_context_invalidate(dev_hndl, hw_qid);
		break;
	default:
		qdma_log_error("%s: access_type=%d is invalid, err:%d\n",
					   __func__, access_type,
					   -QDMA_ERR_INV_PARAM);
		ret_val = -QDMA_ERR_INV_PARAM;
		break;
	}

	return ret_val;
}

/*****************************************************************************/
/**
 * qdma_cpm_cmpt_context_write() - create completion context and program it
 *
 * @dev_hndl:	device handle
 * @hw_qid:	hardware qid of the queue
 * @ctxt:	pointer to the cmpt context data strucutre
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_cmpt_context_write(void *dev_hndl, uint16_t hw_qid,
			   const struct qdma_descq_cmpt_ctxt *ctxt)
{
	uint32_t cmpt_ctxt[QDMA_CPM_CMPT_CONTEXT_NUM_WORDS] = {0};
	uint16_t num_words_count = 0;
	uint32_t baddr_l, baddr_m, baddr_h;
	enum ind_ctxt_cmd_sel sel = QDMA_CTXT_SEL_CMPT;

	/* Input args check */
	if (!dev_hndl || !ctxt) {
		qdma_log_error("%s: dev_hndl=%p cmpt_ctxt=%p, err:%d\n",
					   __func__, dev_hndl, ctxt,
					   -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	if ((ctxt->desc_sz > QDMA_DESC_SIZE_32B) ||
		(ctxt->ringsz_idx >= QDMA_NUM_RING_SIZES) ||
		(ctxt->counter_idx >= QDMA_NUM_C2H_COUNTERS) ||
		(ctxt->timer_idx >= QDMA_NUM_C2H_TIMERS) ||
		(ctxt->trig_mode > QDMA_CMPT_UPDATE_TRIG_MODE_TMR_CNTR)) {
		qdma_log_error
		("%s Inv dsz(%d)/ridx(%d)/cntr(%d)/tmr(%d)/tm(%d), err:%d\n",
				__func__,
				ctxt->desc_sz,
				ctxt->ringsz_idx,
				ctxt->counter_idx,
				ctxt->timer_idx,
				ctxt->trig_mode,
				-QDMA_ERR_INV_PARAM);
		return QDMA_ERR_INV_PARAM;
	}

	baddr_l = (uint32_t)FIELD_GET(QDMA_CPM_COMPL_CTXT_BADDR_GET_L_MASK,
			ctxt->bs_addr);
	baddr_m = (uint32_t)FIELD_GET(QDMA_CPM_COMPL_CTXT_BADDR_GET_M_MASK,
			ctxt->bs_addr);
	baddr_h = (uint32_t)FIELD_GET(QDMA_CPM_COMPL_CTXT_BADDR_GET_H_MASK,
			ctxt->bs_addr);


	cmpt_ctxt[num_words_count++] =
		FIELD_SET(QDMA_COMPL_CTXT_W0_EN_STAT_DESC_MASK,
				ctxt->en_stat_desc) |
		FIELD_SET(QDMA_COMPL_CTXT_W0_EN_INT_MASK, ctxt->en_int) |
		FIELD_SET(QDMA_COMPL_CTXT_W0_TRIG_MODE_MASK, ctxt->trig_mode) |
		FIELD_SET(QDMA_COMPL_CTXT_W0_FNC_ID_MASK, ctxt->fnc_id) |
		FIELD_SET(QDMA_CPM_COMPL_CTXT_W0_COUNTER_IDX_MASK,
				ctxt->counter_idx) |
		FIELD_SET(QDMA_CPM_COMPL_CTXT_W0_TIMER_IDX_MASK,
				ctxt->timer_idx) |
		FIELD_SET(QDMA_CPM_COMPL_CTXT_W0_INT_ST_MASK, ctxt->in_st) |
		FIELD_SET(QDMA_CPM_COMPL_CTXT_W0_COLOR_MASK, ctxt->color) |
		FIELD_SET(QDMA_CPM_COMPL_CTXT_W0_RING_SZ_MASK,
				ctxt->ringsz_idx) |
		FIELD_SET(QDMA_CPM_COMPL_CTXT_W0_BADDR_64_L_MASK, baddr_l);

	cmpt_ctxt[num_words_count++] = (baddr_m) & 0xFFFFFFFFU;

	cmpt_ctxt[num_words_count++] =
		FIELD_SET(QDMA_CPM_COMPL_CTXT_W2_BADDR_64_H_MASK, baddr_h) |
		FIELD_SET(QDMA_CPM_COMPL_CTXT_W2_DESC_SIZE_MASK, ctxt->desc_sz);

	cmpt_ctxt[num_words_count++] =
		FIELD_SET(QDMA_CPM_COMPL_CTXT_W3_VALID_MASK, ctxt->valid);

	return qdma_cpm_indirect_reg_write(dev_hndl, sel, hw_qid,
			cmpt_ctxt, num_words_count);

}

/*****************************************************************************/
/**
 * qdma_cpm_cmpt_context_read() - read completion context
 *
 * @dev_hndl:	device handle
 * @hw_qid:	hardware qid of the queue
 * @ctxt:	    pointer to the context data
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_cmpt_context_read(void *dev_hndl, uint16_t hw_qid,
			   struct qdma_descq_cmpt_ctxt *ctxt)
{
	int rv = 0;
	uint32_t cmpt_ctxt[QDMA_CPM_CMPT_CONTEXT_NUM_WORDS] = {0};
	enum ind_ctxt_cmd_sel sel = QDMA_CTXT_SEL_CMPT;
	uint32_t baddr_l, baddr_m, baddr_h, pidx_l, pidx_h;

	if (!dev_hndl || !ctxt) {
		qdma_log_error("%s: dev_hndl=%p cmpt_ctxt=%p, err:%d\n",
					   __func__, dev_hndl, ctxt,
					   -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	rv = qdma_cpm_indirect_reg_read(dev_hndl, sel, hw_qid,
			QDMA_CPM_CMPT_CONTEXT_NUM_WORDS, cmpt_ctxt);
	if (rv < 0)
		return rv;

	ctxt->en_stat_desc =
		FIELD_GET(QDMA_COMPL_CTXT_W0_EN_STAT_DESC_MASK, cmpt_ctxt[0]);
	ctxt->en_int = FIELD_GET(QDMA_COMPL_CTXT_W0_EN_INT_MASK,
		cmpt_ctxt[0]);
	ctxt->trig_mode =
		FIELD_GET(QDMA_COMPL_CTXT_W0_TRIG_MODE_MASK, cmpt_ctxt[0]);
	ctxt->fnc_id =
		(uint8_t)(FIELD_GET(QDMA_COMPL_CTXT_W0_FNC_ID_MASK,
			cmpt_ctxt[0]));
	ctxt->counter_idx =
		(uint8_t)(FIELD_GET(QDMA_CPM_COMPL_CTXT_W0_COUNTER_IDX_MASK,
			  cmpt_ctxt[0]));
	ctxt->timer_idx =
		(uint8_t)(FIELD_GET(QDMA_CPM_COMPL_CTXT_W0_TIMER_IDX_MASK,
				cmpt_ctxt[0]));
	ctxt->in_st =
		(uint8_t)(FIELD_GET(QDMA_CPM_COMPL_CTXT_W0_INT_ST_MASK,
			cmpt_ctxt[0]));
	ctxt->color =
		(uint8_t)(FIELD_GET(QDMA_CPM_COMPL_CTXT_W0_COLOR_MASK,
			cmpt_ctxt[0]));
	ctxt->ringsz_idx =
		(uint8_t)(FIELD_GET(QDMA_CPM_COMPL_CTXT_W0_RING_SZ_MASK,
			cmpt_ctxt[0]));

	baddr_l =
		FIELD_GET(QDMA_CPM_COMPL_CTXT_W0_BADDR_64_L_MASK, cmpt_ctxt[0]);

	baddr_m =
		FIELD_GET(QDMA_CPM_COMPL_CTXT_W1_BADDR_64_M_MASK, cmpt_ctxt[1]);

	baddr_h =
		FIELD_GET(QDMA_CPM_COMPL_CTXT_W2_BADDR_64_H_MASK, cmpt_ctxt[2]);
	ctxt->desc_sz =
		(uint8_t)(FIELD_GET(QDMA_CPM_COMPL_CTXT_W2_DESC_SIZE_MASK,
			cmpt_ctxt[2]));
	pidx_l = FIELD_GET(QDMA_CPM_COMPL_CTXT_W2_PIDX_L_MASK, cmpt_ctxt[2]);

	pidx_h = FIELD_GET(QDMA_CPM_COMPL_CTXT_W3_PIDX_H_MASK, cmpt_ctxt[3]);
	ctxt->cidx =
		(uint16_t)(FIELD_GET(QDMA_CPM_COMPL_CTXT_W3_CIDX_MASK,
			cmpt_ctxt[3]));
	ctxt->valid =
		(uint8_t)(FIELD_GET(QDMA_CPM_COMPL_CTXT_W3_VALID_MASK,
			cmpt_ctxt[3]));
	ctxt->err =
		(uint8_t)(FIELD_GET(QDMA_CPM_COMPL_CTXT_W3_ERR_MASK,
			cmpt_ctxt[3]));
	ctxt->user_trig_pend =
		(uint8_t)(FIELD_GET(QDMA_CPM_COMPL_CTXT_W3_USR_TRG_PND_MASK,
			  cmpt_ctxt[3]));

	ctxt->timer_running =
		(uint8_t)(FIELD_GET(QDMA_CPM_COMPL_CTXT_W3_TMR_RUN_MASK,
			cmpt_ctxt[3]));
	ctxt->full_upd =
		(uint8_t)(FIELD_GET(QDMA_CPM_COMPL_CTXT_W3_FULL_UPDT_MASK,
			cmpt_ctxt[3]));

	ctxt->bs_addr =
		FIELD_SET(QDMA_CPM_COMPL_CTXT_BADDR_GET_L_MASK, baddr_l) |
		FIELD_SET(QDMA_CPM_COMPL_CTXT_BADDR_GET_M_MASK, baddr_m) |
		FIELD_SET(QDMA_CPM_COMPL_CTXT_BADDR_GET_H_MASK,
			  (uint64_t)baddr_h);

	ctxt->pidx =
		FIELD_SET(QDMA_COMPL_CTXT_PIDX_GET_L_MASK, pidx_l) |
		FIELD_SET(QDMA_COMPL_CTXT_PIDX_GET_H_MASK, pidx_h);

	return QDMA_SUCCESS;
}

/*****************************************************************************/
/**
 * qdma_cpm_cmpt_context_clear() - clear completion context
 *
 * @dev_hndl:	device handle
 * @hw_qid:	hardware qid of the queue
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_cmpt_context_clear(void *dev_hndl, uint16_t hw_qid)
{
	enum ind_ctxt_cmd_sel sel = QDMA_CTXT_SEL_CMPT;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	return qdma_cpm_indirect_reg_clear(dev_hndl, sel, hw_qid);
}

/*****************************************************************************/
/**
 * qdma_cpm_cmpt_context_invalidate() - invalidate completion context
 *
 * @dev_hndl:	device handle
 * @hw_qid:	hardware qid of the queue
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_cmpt_context_invalidate(void *dev_hndl, uint16_t hw_qid)
{
	enum ind_ctxt_cmd_sel sel = QDMA_CTXT_SEL_CMPT;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	return qdma_cpm_indirect_reg_invalidate(dev_hndl, sel, hw_qid);
}

/*****************************************************************************/
/**
 * qdma_cpm_cmpt_ctx_conf() - configure completion context
 *
 * @dev_hndl:	device handle
 * @hw_qid:	hardware qid of the queue
 * @ctxt:	pointer to context data
 * @access_type HW access type (qdma_hw_access_type enum) value
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
int qdma_cpm_cmpt_ctx_conf(void *dev_hndl, uint16_t hw_qid,
			struct qdma_descq_cmpt_ctxt *ctxt,
			enum qdma_hw_access_type access_type)
{
	int ret_val = 0;

	switch (access_type) {
	case QDMA_HW_ACCESS_READ:
		ret_val = qdma_cpm_cmpt_context_read(dev_hndl, hw_qid, ctxt);
		break;
	case QDMA_HW_ACCESS_WRITE:
		ret_val = qdma_cpm_cmpt_context_write(dev_hndl, hw_qid, ctxt);
		break;
	case QDMA_HW_ACCESS_CLEAR:
		ret_val = qdma_cpm_cmpt_context_clear(dev_hndl, hw_qid);
		break;
	case QDMA_HW_ACCESS_INVALIDATE:
		ret_val = qdma_cpm_cmpt_context_invalidate(dev_hndl, hw_qid);
		break;
	default:
		qdma_log_error("%s: access_type=%d is invalid, err:%d\n",
					   __func__, access_type,
					   -QDMA_ERR_INV_PARAM);
		ret_val = -QDMA_ERR_INV_PARAM;
		break;
	}

	return ret_val;
}

/*****************************************************************************/
/**
 * qdma_cpm_hw_context_read() - read hardware context
 *
 * @dev_hndl:	device handle
 * @c2h:	is c2h queue
 * @hw_qid:	hardware qid of the queue
 * @ctxt:	pointer to the output context data
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_hw_context_read(void *dev_hndl, uint8_t c2h,
			 uint16_t hw_qid, struct qdma_descq_hw_ctxt *ctxt)
{
	int rv = 0;
	uint32_t hw_ctxt[QDMA_CPM_HW_CONTEXT_NUM_WORDS] = {0};
	enum ind_ctxt_cmd_sel sel = c2h ? QDMA_CTXT_SEL_HW_C2H :
			QDMA_CTXT_SEL_HW_H2C;

	if (!dev_hndl || !ctxt) {
		qdma_log_error("%s: dev_hndl=%p hw_ctxt=%p, err:%d\n",
						__func__, dev_hndl, ctxt,
						-QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	rv = qdma_cpm_indirect_reg_read(dev_hndl, sel, hw_qid,
				    QDMA_CPM_HW_CONTEXT_NUM_WORDS, hw_ctxt);
	if (rv < 0)
		return rv;

	ctxt->cidx = FIELD_GET(QDMA_HW_CTXT_W0_CIDX_MASK, hw_ctxt[0]);
	ctxt->crd_use =
		(uint16_t)(FIELD_GET(QDMA_HW_CTXT_W0_CRD_USE_MASK, hw_ctxt[0]));

	ctxt->dsc_pend =
		(uint8_t)(FIELD_GET(QDMA_HW_CTXT_W1_DSC_PND_MASK, hw_ctxt[1]));
	ctxt->idl_stp_b =
		(uint8_t)(FIELD_GET(QDMA_HW_CTXT_W1_IDL_STP_B_MASK,
			hw_ctxt[1]));
	ctxt->fetch_pnd =
		(uint8_t)(FIELD_GET(QDMA_CPM_HW_CTXT_W1_FETCH_PEND_MASK,
			hw_ctxt[1]));

	return QDMA_SUCCESS;
}

/*****************************************************************************/
/**
 * qdma_cpm_hw_context_clear() - clear hardware context
 *
 * @dev_hndl:	device handle
 * @c2h:	is c2h queue
 * @hw_qid:	hardware qid of the queue
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_hw_context_clear(void *dev_hndl, uint8_t c2h,
			  uint16_t hw_qid)
{
	enum ind_ctxt_cmd_sel sel = c2h ? QDMA_CTXT_SEL_HW_C2H :
			QDMA_CTXT_SEL_HW_H2C;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	return qdma_cpm_indirect_reg_clear(dev_hndl, sel, hw_qid);
}

/*****************************************************************************/
/**
 * qdma_cpm_hw_context_invalidate() - invalidate hardware context
 *
 * @dev_hndl:	device handle
 * @c2h:	is c2h queue
 * @hw_qid:	hardware qid of the queue
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_hw_context_invalidate(void *dev_hndl, uint8_t c2h,
				   uint16_t hw_qid)
{
	enum ind_ctxt_cmd_sel sel = c2h ? QDMA_CTXT_SEL_HW_C2H :
			QDMA_CTXT_SEL_HW_H2C;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	return qdma_cpm_indirect_reg_invalidate(dev_hndl, sel, hw_qid);
}

/*****************************************************************************/
/**
 * qdma_cpm_hw_ctx_conf() - configure HW context
 *
 * @dev_hndl:	device handle
 * @c2h:	is c2h queue
 * @hw_qid:	hardware qid of the queue
 * @ctxt:	pointer to context data
 * @access_type HW access type (qdma_hw_access_type enum) value
 *		QDMA_HW_ACCESS_WRITE unsupported
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
int qdma_cpm_hw_ctx_conf(void *dev_hndl, uint8_t c2h, uint16_t hw_qid,
				struct qdma_descq_hw_ctxt *ctxt,
				enum qdma_hw_access_type access_type)
{
	int ret_val = 0;

	switch (access_type) {
	case QDMA_HW_ACCESS_READ:
		ret_val = qdma_cpm_hw_context_read(dev_hndl, c2h, hw_qid, ctxt);
		break;
	case QDMA_HW_ACCESS_CLEAR:
		ret_val = qdma_cpm_hw_context_clear(dev_hndl, c2h, hw_qid);
		break;
	case QDMA_HW_ACCESS_INVALIDATE:
		ret_val = qdma_cpm_hw_context_invalidate(dev_hndl, c2h, hw_qid);
		break;
	case QDMA_HW_ACCESS_WRITE:
	default:
		qdma_log_error("%s: access_type=%d is invalid, err:%d\n",
						__func__, access_type,
						-QDMA_ERR_INV_PARAM);
		ret_val = -QDMA_ERR_INV_PARAM;
		break;
	}


	return ret_val;
}

/*****************************************************************************/
/**
 * qdma_cpm_indirect_intr_context_write() - create indirect interrupt context
 *					and program it
 *
 * @dev_hndl:   device handle
 * @ring_index: indirect interrupt ring index
 * @ctxt:	pointer to the interrupt context data strucutre
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_indirect_intr_context_write(void *dev_hndl,
		uint16_t ring_index, const struct qdma_indirect_intr_ctxt *ctxt)
{
	uint32_t intr_ctxt[QDMA_CPM_IND_INTR_CONTEXT_NUM_WORDS] = {0};
	enum ind_ctxt_cmd_sel sel = QDMA_CTXT_SEL_INT_COAL;
	uint16_t num_words_count = 0;
	uint32_t baddr_l, baddr_h;

	if (!dev_hndl || !ctxt) {
		qdma_log_error("%s: dev_hndl=%p intr_ctxt=%p, err:%d\n",
						__func__, dev_hndl, ctxt,
						-QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	if (ctxt->page_size > QDMA_INDIRECT_INTR_RING_SIZE_32KB) {
		qdma_log_error("%s: ctxt->page_size=%u is too big, err:%d\n",
					   __func__, ctxt->page_size,
					   -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	baddr_l = (uint32_t)FIELD_GET(QDMA_CPM_INTR_CTXT_BADDR_GET_L_MASK,
			ctxt->baddr_4k);
	baddr_h = (uint32_t)FIELD_GET(QDMA_CPM_INTR_CTXT_BADDR_GET_H_MASK,
			ctxt->baddr_4k);

	intr_ctxt[num_words_count++] =
		FIELD_SET(QDMA_INTR_CTXT_W0_VALID_MASK, ctxt->valid) |
		FIELD_SET(QDMA_CPM_INTR_CTXT_W0_VEC_ID_MASK, ctxt->vec) |
		FIELD_SET(QDMA_CPM_INTR_CTXT_W0_INT_ST_MASK, ctxt->int_st) |
		FIELD_SET(QDMA_CPM_INTR_CTXT_W0_COLOR_MASK, ctxt->color) |
		FIELD_SET(QDMA_CPM_INTR_CTXT_W0_BADDR_64_MASK, baddr_l);

	intr_ctxt[num_words_count++] =
		FIELD_SET(QDMA_CPM_INTR_CTXT_W1_BADDR_64_MASK, baddr_h) |
		FIELD_SET(QDMA_CPM_INTR_CTXT_W1_PAGE_SIZE_MASK,
				ctxt->page_size);

	intr_ctxt[num_words_count++] =
		FIELD_SET(QDMA_CPM_INTR_CTXT_W2_PIDX_MASK, ctxt->pidx);

	return qdma_cpm_indirect_reg_write(dev_hndl, sel, ring_index,
			intr_ctxt, num_words_count);
}

/*****************************************************************************/
/**
 * qdma_indirect_intr_context_read() - read indirect interrupt context
 *
 * @dev_hndl:	device handle
 * @ring_index:	indirect interrupt ring index
 * @ctxt:	pointer to the output context data
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_indirect_intr_context_read(void *dev_hndl,
		uint16_t ring_index, struct qdma_indirect_intr_ctxt *ctxt)
{
	int rv = 0;
	uint32_t intr_ctxt[QDMA_CPM_IND_INTR_CONTEXT_NUM_WORDS] = {0};
	enum ind_ctxt_cmd_sel sel = QDMA_CTXT_SEL_INT_COAL;
	uint64_t baddr_l, baddr_h;

	if (!dev_hndl || !ctxt) {
		qdma_log_error("%s: dev_hndl=%p intr_ctxt=%p, err:%d\n",
					   __func__, dev_hndl, ctxt,
					   -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	rv = qdma_cpm_indirect_reg_read(dev_hndl, sel, ring_index,
			QDMA_CPM_IND_INTR_CONTEXT_NUM_WORDS, intr_ctxt);
	if (rv < 0)
		return rv;

	ctxt->valid = FIELD_GET(QDMA_INTR_CTXT_W0_VALID_MASK, intr_ctxt[0]);
	ctxt->vec = FIELD_GET(QDMA_CPM_INTR_CTXT_W0_VEC_ID_MASK, intr_ctxt[0]);
	ctxt->int_st = FIELD_GET(QDMA_CPM_INTR_CTXT_W0_INT_ST_MASK,
			intr_ctxt[0]);
	ctxt->color =
		(uint8_t)(FIELD_GET(QDMA_CPM_INTR_CTXT_W0_COLOR_MASK,
			intr_ctxt[0]));
	baddr_l = FIELD_GET(QDMA_CPM_INTR_CTXT_W0_BADDR_64_MASK, intr_ctxt[0]);

	baddr_h = FIELD_GET(QDMA_CPM_INTR_CTXT_W1_BADDR_64_MASK, intr_ctxt[1]);
	ctxt->page_size =
		(uint8_t)(FIELD_GET(QDMA_CPM_INTR_CTXT_W1_PAGE_SIZE_MASK,
			intr_ctxt[1]));
	ctxt->pidx = FIELD_GET(QDMA_CPM_INTR_CTXT_W2_PIDX_MASK, intr_ctxt[2]);

	ctxt->baddr_4k =
		FIELD_SET(QDMA_CPM_INTR_CTXT_BADDR_GET_L_MASK, baddr_l) |
		FIELD_SET(QDMA_CPM_INTR_CTXT_BADDR_GET_H_MASK, baddr_h);

	return QDMA_SUCCESS;
}

/*****************************************************************************/
/**
 * qdma_cpm_indirect_intr_context_clear() - clear indirect interrupt context
 *
 * @dev_hndl:	device handle
 * @ring_index:	indirect interrupt ring index
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_indirect_intr_context_clear(void *dev_hndl,
		uint16_t ring_index)
{
	enum ind_ctxt_cmd_sel sel = QDMA_CTXT_SEL_INT_COAL;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	return qdma_cpm_indirect_reg_clear(dev_hndl, sel, ring_index);
}

/*****************************************************************************/
/**
 * qdma_cpm_indirect_intr_context_invalidate() - invalidate indirect interrupt
 * context
 *
 * @dev_hndl:	device handle
 * @ring_index:	indirect interrupt ring index
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_indirect_intr_context_invalidate(void *dev_hndl,
					  uint16_t ring_index)
{
	enum ind_ctxt_cmd_sel sel = QDMA_CTXT_SEL_INT_COAL;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	return qdma_cpm_indirect_reg_invalidate(dev_hndl, sel, ring_index);
}

/*****************************************************************************/
/**
 * qdma_cpm_indirect_intr_ctx_conf() - configure indirect interrupt context
 *
 * @dev_hndl:	device handle
 * @ring_index:	indirect interrupt ring index
 * @ctxt:	pointer to context data
 * @access_type HW access type (qdma_hw_access_type enum) value
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
int qdma_cpm_indirect_intr_ctx_conf(void *dev_hndl, uint16_t ring_index,
				struct qdma_indirect_intr_ctxt *ctxt,
				enum qdma_hw_access_type access_type)
{
	int ret_val = 0;

	switch (access_type) {
	case QDMA_HW_ACCESS_READ:
		ret_val = qdma_cpm_indirect_intr_context_read(dev_hndl,
							      ring_index,
							      ctxt);
		break;
	case QDMA_HW_ACCESS_WRITE:
		ret_val = qdma_cpm_indirect_intr_context_write(dev_hndl,
							       ring_index,
							       ctxt);
		break;
	case QDMA_HW_ACCESS_CLEAR:
		ret_val = qdma_cpm_indirect_intr_context_clear(dev_hndl,
							   ring_index);
		break;
	case QDMA_HW_ACCESS_INVALIDATE:
		ret_val = qdma_cpm_indirect_intr_context_invalidate(dev_hndl,
								ring_index);
		break;
	default:
		qdma_log_error("%s: access_type=%d is invalid, err:%d\n",
					   __func__, access_type,
					   -QDMA_ERR_INV_PARAM);
		ret_val = -QDMA_ERR_INV_PARAM;
		break;
	}

	return ret_val;
}

/*****************************************************************************/
/**
 * qdma_cpm_set_default_global_csr() - function to set the global CSR register
 * to default values. The value can be modified later by using the set/get csr
 * functions
 *
 * @dev_hndl:	device handle
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
int qdma_cpm_set_default_global_csr(void *dev_hndl)
{
	/* Default values */
	uint32_t reg_val = 0;
	uint32_t rng_sz[QDMA_NUM_RING_SIZES] = {2049, 65, 129, 193, 257,
				385, 513, 769, 1025, 1537, 3073, 4097, 6145,
				8193, 12289, 16385};
	uint32_t tmr_cnt[QDMA_NUM_C2H_TIMERS] = {1, 2, 4, 5, 8, 10, 15, 20, 25,
				30, 50, 75, 100, 125, 150, 200};
	uint32_t cnt_th[QDMA_NUM_C2H_COUNTERS] = {64, 2, 4, 8, 16, 24,
				32, 48, 80, 96, 112, 128, 144, 160, 176, 192};
	uint32_t buf_sz[QDMA_NUM_C2H_BUFFER_SIZES] = {4096, 256, 512, 1024,
				2048, 3968, 4096, 4096, 4096, 4096, 4096, 4096,
				4096, 8192, 9018, 16384};
	struct qdma_dev_attributes *dev_cap = NULL;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	qdma_get_device_attr(dev_hndl, &dev_cap);

	/* Configuring CSR registers */
	/* Global ring sizes */
	qdma_write_csr_values(dev_hndl, QDMA_OFFSET_GLBL_RNG_SZ, 0,
					QDMA_NUM_RING_SIZES, rng_sz);

	if (dev_cap->st_en || dev_cap->mm_cmpt_en) {
		/* Counter thresholds */
		qdma_write_csr_values(dev_hndl, QDMA_OFFSET_C2H_CNT_TH, 0,
				      QDMA_NUM_C2H_COUNTERS, cnt_th);

		/* Timer Counters */
		qdma_write_csr_values(dev_hndl, QDMA_OFFSET_C2H_TIMER_CNT, 0,
						QDMA_NUM_C2H_TIMERS, tmr_cnt);


		/* Writeback Interval */
		reg_val =
			FIELD_SET(QDMA_GLBL_DSC_CFG_MAX_DSC_FETCH_MASK,
				  DEFAULT_MAX_DSC_FETCH) |
				  FIELD_SET(QDMA_GLBL_DSC_CFG_WB_ACC_INT_MASK,
				  DEFAULT_WRB_INT);
		qdma_reg_write(dev_hndl, QDMA_OFFSET_GLBL_DSC_CFG, reg_val);
	}

	if (dev_cap->st_en) {
		/* Buffer Sizes */
		qdma_write_csr_values(dev_hndl, QDMA_OFFSET_C2H_BUF_SZ, 0,
				      QDMA_NUM_C2H_BUFFER_SIZES, buf_sz);

		/* Prefetch Configuration */
		reg_val =
			FIELD_SET(QDMA_C2H_PFCH_FL_TH_MASK,
				DEFAULT_PFCH_STOP_THRESH) |
				FIELD_SET(QDMA_C2H_NUM_PFCH_MASK,
				DEFAULT_PFCH_NUM_ENTRIES_PER_Q) |
				FIELD_SET(QDMA_C2H_PFCH_QCNT_MASK,
				DEFAULT_PFCH_MAX_Q_CNT) |
				FIELD_SET(QDMA_C2H_EVT_QCNT_TH_MASK,
				DEFAULT_C2H_INTR_TIMER_TICK);
		qdma_reg_write(dev_hndl, QDMA_OFFSET_C2H_PFETCH_CFG, reg_val);

		/* C2H interrupt timer tick */
		qdma_reg_write(dev_hndl, QDMA_OFFSET_C2H_INT_TIMER_TICK,
						DEFAULT_C2H_INTR_TIMER_TICK);

		/* C2h Completion Coalesce Configuration */
		reg_val =
			FIELD_SET(QDMA_C2H_TICK_CNT_MASK,
				DEFAULT_CMPT_COAL_TIMER_CNT) |
				FIELD_SET(QDMA_C2H_TICK_VAL_MASK,
				DEFAULT_CMPT_COAL_TIMER_TICK) |
				FIELD_SET(QDMA_C2H_MAX_BUF_SZ_MASK,
				DEFAULT_CMPT_COAL_MAX_BUF_SZ);
		qdma_reg_write(dev_hndl, QDMA_OFFSET_C2H_WRB_COAL_CFG, reg_val);

#if 0
		/* H2C throttle Configuration*/
		reg_val =
			FIELD_SET(QDMA_H2C_DATA_THRESH_MASK,
				DEFAULT_H2C_THROT_DATA_THRESH) |
				FIELD_SET(QDMA_H2C_REQ_THROT_EN_DATA_MASK,
				DEFAULT_THROT_EN_DATA);
		qdma_reg_write(dev_hndl, QDMA_OFFSET_H2C_REQ_THROT, reg_val);
#endif
	}

	return QDMA_SUCCESS;
}

/*****************************************************************************/
/**
 * qdma_cpm_queue_pidx_update() - function to update the desc PIDX
 *
 * @dev_hndl:	device handle
 * @is_vf:	Whether PF or VF
 * @qid:	Queue id relative to the PF/VF calling this API
 * @is_c2h:	Queue direction. Set 1 for C2H and 0 for H2C
 * @reg_info:	data needed for the PIDX register update
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
int qdma_cpm_queue_pidx_update(void *dev_hndl, uint8_t is_vf, uint16_t qid,
		uint8_t is_c2h, const struct qdma_q_pidx_reg_info *reg_info)
{
	uint32_t reg_addr = 0;
	uint32_t reg_val = 0;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	if (!is_vf) {
		reg_addr = (is_c2h) ?  QDMA_CPM_OFFSET_DMAP_SEL_C2H_DSC_PIDX :
			QDMA_CPM_OFFSET_DMAP_SEL_H2C_DSC_PIDX;
	} else {
		reg_addr = (is_c2h) ?  QDMA_OFFSET_VF_DMAP_SEL_C2H_DSC_PIDX :
			QDMA_OFFSET_VF_DMAP_SEL_H2C_DSC_PIDX;
	}

	reg_addr += (qid * QDMA_PIDX_STEP);

	reg_val = FIELD_SET(QDMA_DMA_SEL_DESC_PIDX_MASK, reg_info->pidx) |
			  FIELD_SET(QDMA_DMA_SEL_IRQ_EN_MASK, reg_info->irq_en);

	qdma_reg_write(dev_hndl, reg_addr, reg_val);

	return QDMA_SUCCESS;
}

/*****************************************************************************/
/**
 * qdma_cpm_queue_cmpt_cidx_update() - function to update the CMPT CIDX update
 *
 * @dev_hndl:	device handle
 * @is_vf:	Whether PF or VF
 * @qid:	Queue id relative to the PF/VF calling this API
 * @reg_info:	data needed for the CIDX register update
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
int qdma_cpm_queue_cmpt_cidx_update(void *dev_hndl, uint8_t is_vf, uint16_t qid,
		const struct qdma_q_cmpt_cidx_reg_info *reg_info)
{
	uint32_t reg_addr = (is_vf) ? QDMA_OFFSET_VF_DMAP_SEL_CMPT_CIDX :
		QDMA_CPM_OFFSET_DMAP_SEL_CMPT_CIDX;
	uint32_t reg_val = 0;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	reg_addr += (qid * QDMA_CMPT_CIDX_STEP);

	reg_val =
		FIELD_SET(QDMA_DMAP_SEL_CMPT_WRB_CIDX_MASK,
				reg_info->wrb_cidx) |
		FIELD_SET(QDMA_DMAP_SEL_CMPT_CNT_THRESH_MASK,
				reg_info->counter_idx) |
		FIELD_SET(QDMA_DMAP_SEL_CMPT_TMR_CNT_MASK,
				reg_info->timer_idx) |
		FIELD_SET(QDMA_DMAP_SEL_CMPT_TRG_MODE_MASK,
				reg_info->trig_mode) |
		FIELD_SET(QDMA_DMAP_SEL_CMPT_STS_DESC_EN_MASK,
				reg_info->wrb_en) |
		FIELD_SET(QDMA_DMAP_SEL_CMPT_IRQ_EN_MASK, reg_info->irq_en);

	qdma_reg_write(dev_hndl, reg_addr, reg_val);

	return QDMA_SUCCESS;
}

/*****************************************************************************/
/**
 * qdma_cpm_queue_intr_cidx_update() - function to update the CMPT CIDX update
 *
 * @dev_hndl:	device handle
 * @is_vf:	Whether PF or VF
 * @qid:	Queue id relative to the PF/VF calling this API
 * @reg_info:	data needed for the CIDX register update
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
int qdma_cpm_queue_intr_cidx_update(void *dev_hndl, uint8_t is_vf, uint16_t qid,
		const struct qdma_intr_cidx_reg_info *reg_info)
{
	uint32_t reg_addr = (is_vf) ? QDMA_OFFSET_VF_DMAP_SEL_INT_CIDX :
		QDMA_CPM_OFFSET_DMAP_SEL_INT_CIDX;
	uint32_t reg_val = 0;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	reg_addr += qid * QDMA_INT_CIDX_STEP;

	reg_val =
		FIELD_SET(QDMA_DMA_SEL_INT_SW_CIDX_MASK, reg_info->sw_cidx) |
		FIELD_SET(QDMA_DMA_SEL_INT_RING_IDX_MASK, reg_info->rng_idx);

	qdma_reg_write(dev_hndl, reg_addr, reg_val);

	return QDMA_SUCCESS;
}

/*****************************************************************************/
/**
 * qdma_cmp_get_user_bar() - Function to get the user bar number
 *
 * @dev_hndl:	device handle
 * @is_vf:	Whether PF or VF
 * @user_bar:	pointer to hold the user bar number
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
int qdma_cmp_get_user_bar(void *dev_hndl, uint8_t is_vf, uint8_t *user_bar)
{
	uint8_t bar_found = 0;
	uint8_t bar_idx = 0;
	uint32_t func_id = 0;
	uint32_t user_bar_id = 0;
	uint32_t reg_addr = (is_vf) ?  QDMA_OFFSET_GLBL2_PF_VF_BARLITE_EXT :
			QDMA_OFFSET_GLBL2_PF_BARLITE_EXT;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}



	if (!is_vf) {
		user_bar_id = qdma_reg_read(dev_hndl, reg_addr);
		func_id = qdma_reg_read(dev_hndl,
				QDMA_OFFSET_GLBL2_CHANNEL_FUNC_RET);
		user_bar_id = (user_bar_id >> (6 * func_id)) & 0x3F;
	} else {
		*user_bar = QDMA_CPM_VF_USER_BAR_ID;
		return QDMA_SUCCESS;
	}

	for (bar_idx = 0; bar_idx < QDMA_BAR_NUM; bar_idx++) {
		if (user_bar_id & (1 << bar_idx)) {
			*user_bar = bar_idx;
			bar_found = 1;
			break;
		}
	}
	if (bar_found == 0) {
		*user_bar = 0;
		qdma_log_error("%s: Bar not found, err:%d\n",
					__func__,
					-QDMA_ERR_HWACC_BAR_NOT_FOUND);
		return -QDMA_ERR_HWACC_BAR_NOT_FOUND;
	}

	return QDMA_SUCCESS;
}

/*****************************************************************************/
/**
 * qdma_cpm_get_device_attributes() - Function to get the qdma device attributes
 *
 * @dev_hndl:	device handle
 * @dev_info:	pointer to hold the device info
 *
 * Return:	0   - success and < 0 - failure
 *****************************************************************************/
int qdma_cpm_get_device_attributes(void *dev_hndl,
		struct qdma_dev_attributes *dev_info)
{
	uint8_t count = 0;
	uint32_t reg_val = 0;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	/* number of PFs */
	reg_val = qdma_reg_read(dev_hndl, QDMA_OFFSET_GLBL2_PF_BARLITE_INT);
	if (FIELD_GET(QDMA_GLBL2_PF0_BAR_MAP_MASK, reg_val))
		count++;
	if (FIELD_GET(QDMA_GLBL2_PF1_BAR_MAP_MASK, reg_val))
		count++;
	if (FIELD_GET(QDMA_GLBL2_PF2_BAR_MAP_MASK, reg_val))
		count++;
	if (FIELD_GET(QDMA_GLBL2_PF3_BAR_MAP_MASK, reg_val))
		count++;
	dev_info->num_pfs = count;

	/* Number of Qs */
	reg_val = qdma_reg_read(dev_hndl, QDMA_OFFSET_GLBL2_CHANNEL_QDMA_CAP);
	dev_info->num_qs = (FIELD_GET(QDMA_GLBL2_MULTQ_MAX_MASK, reg_val));

	/* FLR present */
	reg_val = qdma_reg_read(dev_hndl, QDMA_OFFSET_GLBL2_MISC_CAP);
	dev_info->mailbox_en  = FIELD_GET(QDMA_GLBL2_MAILBOX_EN_MASK, reg_val);
	dev_info->flr_present = FIELD_GET(QDMA_GLBL2_FLR_PRESENT_MASK, reg_val);
	dev_info->mm_cmpt_en  = 0;

	/* ST/MM enabled? */
	reg_val = qdma_reg_read(dev_hndl, QDMA_OFFSET_GLBL2_CHANNEL_MDMA);
	dev_info->mm_en = (FIELD_GET(QDMA_GLBL2_MM_C2H_MASK, reg_val)
			&& FIELD_GET(QDMA_GLBL2_MM_H2C_MASK, reg_val)) ? 1 : 0;
	dev_info->st_en = (FIELD_GET(QDMA_GLBL2_ST_C2H_MASK, reg_val)
			&& FIELD_GET(QDMA_GLBL2_ST_H2C_MASK, reg_val)) ? 1 : 0;

	/* num of mm channels for Versal Hard is 2 */
	dev_info->mm_channel_max = 2;

	dev_info->qid2vec_ctx = 1;
	dev_info->cmpt_ovf_chk_dis = 0;
	dev_info->mailbox_intr = 0;
	dev_info->sw_desc_64b = 0;
	dev_info->cmpt_desc_64b = 0;
	dev_info->dynamic_bar = 0;
	dev_info->legacy_intr = 0;
	dev_info->cmpt_trig_count_timer = 0;

	return QDMA_SUCCESS;
}


/*****************************************************************************/
/**
 * qdma_cpm_credit_context_read() - read credit context
 *
 * @dev_hndl:	device handle
 * @c2h     :	is c2h queue
 * @hw_qid  :	hardware qid of the queue
 * @ctxt    :	pointer to the context data
 *
 * Return   :	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_credit_context_read(void *dev_hndl, uint8_t c2h,
			 uint16_t hw_qid,
			 struct qdma_descq_credit_ctxt *ctxt)
{
	int rv = QDMA_SUCCESS;
	uint32_t cr_ctxt[QDMA_CPM_CR_CONTEXT_NUM_WORDS] = {0};
	enum ind_ctxt_cmd_sel sel = c2h ? QDMA_CTXT_SEL_CR_C2H :
			QDMA_CTXT_SEL_CR_H2C;

	if (!dev_hndl || !ctxt) {
		qdma_log_error("%s: dev_hndl=%p credit_ctxt=%p, err:%d\n",
						__func__, dev_hndl, ctxt,
					   -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	rv = qdma_cpm_indirect_reg_read(dev_hndl, sel, hw_qid,
			QDMA_CPM_CR_CONTEXT_NUM_WORDS, cr_ctxt);
	if (rv < 0)
		return rv;

	ctxt->credit = FIELD_GET(QDMA_CPM_CR_CTXT_W0_CREDT_MASK, cr_ctxt[0]);

	qdma_log_debug("%s: credit=%u\n", __func__, ctxt->credit);

	return QDMA_SUCCESS;
}

/*****************************************************************************/
/**
 * qdma_cpm_credit_context_clear() - clear credit context
 *
 * @dev_hndl:	device handle
 * @c2h     :	is c2h queue
 * @hw_qid  :	hardware qid of the queue
 *
 * Return   :	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_credit_context_clear(void *dev_hndl, uint8_t c2h,
			  uint16_t hw_qid)
{
	enum ind_ctxt_cmd_sel sel = c2h ? QDMA_CTXT_SEL_CR_C2H :
			QDMA_CTXT_SEL_CR_H2C;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n", __func__,
					   -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	return qdma_cpm_indirect_reg_clear(dev_hndl, sel, hw_qid);
}

/*****************************************************************************/
/**
 * qdma_cpm_credit_context_invalidate() - invalidate credit context
 *
 * @dev_hndl:	device handle
 * @c2h     :	is c2h queue
 * @hw_qid  :	hardware qid of the queue
 *
 * Return   :	0   - success and < 0 - failure
 *****************************************************************************/
static int qdma_cpm_credit_context_invalidate(void *dev_hndl, uint8_t c2h,
				   uint16_t hw_qid)
{
	enum ind_ctxt_cmd_sel sel = c2h ? QDMA_CTXT_SEL_CR_C2H :
			QDMA_CTXT_SEL_CR_H2C;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n", __func__,
					   -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	return qdma_cpm_indirect_reg_invalidate(dev_hndl, sel, hw_qid);
}

/*****************************************************************************/
/**
 * qdma_cpm_credit_ctx_conf() - configure credit context
 *
 * @dev_hndl    :	device handle
 * @c2h         :	is c2h queue
 * @hw_qid      :	hardware qid of the queue
 * @ctxt        :	pointer to the context data
 * @access_type :	HW access type (qdma_hw_access_type enum) value
 *		QDMA_HW_ACCESS_WRITE Not supported
 *
 * Return       :	0   - success and < 0 - failure
 *****************************************************************************/
int qdma_cpm_credit_ctx_conf(void *dev_hndl, uint8_t c2h, uint16_t hw_qid,
			struct qdma_descq_credit_ctxt *ctxt,
			enum qdma_hw_access_type access_type)
{
	int rv = QDMA_SUCCESS;

	switch (access_type) {
	case QDMA_HW_ACCESS_READ:
		rv = qdma_cpm_credit_context_read(dev_hndl, c2h, hw_qid, ctxt);
		break;
	case QDMA_HW_ACCESS_CLEAR:
		rv = qdma_cpm_credit_context_clear(dev_hndl, c2h, hw_qid);
		break;
	case QDMA_HW_ACCESS_INVALIDATE:
		rv = qdma_cpm_credit_context_invalidate(dev_hndl, c2h, hw_qid);
		break;
	case QDMA_HW_ACCESS_WRITE:
	default:
		qdma_log_error("%s: Invalid access type=%d, err:%d\n",
					   __func__, access_type,
					   -QDMA_ERR_INV_PARAM);
		rv = -QDMA_ERR_INV_PARAM;
		break;
	}

	return rv;
}


/*****************************************************************************/
/**
 * qdma_cpm_dump_config_regs() - Function to get qdma config register dump in a
 * buffer
 *
 * @dev_hndl:	device handle
 * @buf :	pointer to buffer to be filled
 * @buflen :	Length of the buffer
 *
 * Return:	Length up-till the buffer is filled -success and < 0 - failure
 *****************************************************************************/
int qdma_cpm_dump_config_regs(void *dev_hndl, uint8_t is_vf,
		char *buf, uint32_t buflen)
{
	unsigned int i = 0, j = 0;
	struct xreg_info *reg_info;
	uint32_t num_regs =
	    sizeof(qdma_cpm_config_regs) / sizeof((qdma_cpm_config_regs)[0]);
	uint32_t len = 0, val = 0;
	int rv = QDMA_SUCCESS;
	char name[DEBGFS_GEN_NAME_SZ] = "";
	struct qdma_dev_attributes *dev_cap;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					   __func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	if (buflen < qdma_reg_dump_buf_len()) {
		qdma_log_error("%s: Buffer too small, err:%d\n", __func__,
					   -QDMA_ERR_NO_MEM);
		return -QDMA_ERR_NO_MEM;
	}
	/*TODO : VF register space to be added later.*/
	if (is_vf) {
		qdma_log_error("%s: Not supported for VF, err:%d\n",
				__func__,
				-QDMA_ERR_HWACC_FEATURE_NOT_SUPPORTED);
		return -QDMA_ERR_HWACC_FEATURE_NOT_SUPPORTED;
	}

	qdma_get_device_attr(dev_hndl, &dev_cap);

	reg_info = qdma_cpm_config_regs;

	for (i = 0; i < num_regs - 1; i++) {
		if ((GET_CAPABILITY_MASK(dev_cap->mm_en, dev_cap->st_en,
				dev_cap->mm_cmpt_en, dev_cap->mailbox_en)
				& reg_info[i].mode) == 0)
			continue;

		for (j = 0; j < reg_info[i].repeat; j++) {
			QDMA_SNPRINTF(name, DEBGFS_GEN_NAME_SZ,
					"%s_%d", reg_info[i].name, j);
			val = 0;
			val = qdma_reg_read(dev_hndl,
					(reg_info[i].addr + (j * 4)));
			rv = dump_reg(buf + len, buflen - len,
					(reg_info[i].addr + (j * 4)),
					name, val);
			if (rv < 0) {
				qdma_log_error(
				"%s Buff too small, err:%d\n",
				__func__,
				-QDMA_ERR_NO_MEM);
				return -QDMA_ERR_NO_MEM;
			}
			len += rv;
		}
	}

	return len;
}

/*****************************************************************************/
/**
 * qdma_cpm_dump_queue_context() - Function to get qdma queue context dump
 * in a buffer
 *
 * @dev_hndl:   device handle
 * @hw_qid:     queue id
 * @buf :       pointer to buffer to be filled
 * @buflen :    Length of the buffer
 *
 * Return:	Length up-till the buffer is filled -success and < 0 - failure
 *****************************************************************************/
int qdma_cpm_dump_queue_context(void *dev_hndl, uint16_t hw_qid, uint8_t st,
	uint8_t c2h, char *buf, uint32_t buflen)
{
	int rv = 0;
	char pfetch_valid = 0;
	char cmpt_valid = 0;
	struct qdma_dev_attributes *dev_cap;
	struct qdma_descq_sw_ctxt sw_ctx;
	struct qdma_descq_hw_ctxt hw_ctx;
	struct qdma_descq_credit_ctxt credit_ctx;
	struct qdma_descq_prefetch_ctxt prefetch_ctx;
	struct qdma_descq_cmpt_ctxt cmpt_ctx;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
			__func__, -QDMA_ERR_INV_PARAM);

		return -QDMA_ERR_INV_PARAM;
	}

	qdma_get_device_attr(dev_hndl, &dev_cap);

	if (st && c2h)
		pfetch_valid = 1;
	if ((st && c2h) || (!st && dev_cap->mm_cmpt_en))
		cmpt_valid = 1;

	if (buflen < qdma_context_buf_len(pfetch_valid, cmpt_valid)) {
		qdma_log_error("%s: Buffer too small failure, err:%d\n",
			__func__, -QDMA_ERR_NO_MEM);
		return -QDMA_ERR_NO_MEM;
	}

	if (!dev_cap->st_en && !dev_cap->mm_en) {
		qdma_log_error(
			"%s: ST or MM mode must be enabled, err:%d\n",
			__func__, -QDMA_ERR_HWACC_FEATURE_NOT_SUPPORTED);
		return -QDMA_ERR_HWACC_FEATURE_NOT_SUPPORTED;
	}

	/* sw context read */
	rv = qdma_cpm_sw_ctx_conf(dev_hndl, c2h,
		hw_qid, &sw_ctx, QDMA_HW_ACCESS_READ);
	if (rv < 0) {
		qdma_log_error("%s: Failed to read sw context, err:%d\n",
			__func__, rv);
		return rv;
	}
	qdma_acc_fill_sw_ctxt(&sw_ctx);

	/* hw context read */
	rv = qdma_cpm_hw_ctx_conf(dev_hndl, c2h,
		hw_qid, &hw_ctx, QDMA_HW_ACCESS_READ);
	if (rv < 0) {
		qdma_log_error("%s: Failed to read hw context, err:%d\n",
			__func__, rv);
		return rv;
	}
	qdma_acc_fill_hw_ctxt(&hw_ctx);

	/* credit context read */
	rv = qdma_cpm_credit_ctx_conf(dev_hndl, c2h,
		hw_qid, &credit_ctx, QDMA_HW_ACCESS_READ);
	if (rv < 0) {
		qdma_log_error("%s: Failed to read credit context, err:%d\n",
			__func__, rv);
		return rv;
	}
	qdma_acc_fill_credit_ctxt(&credit_ctx);

	if (pfetch_valid) {
		/* credit context read */
		rv = qdma_cpm_pfetch_ctx_conf(dev_hndl,
			hw_qid, &prefetch_ctx, QDMA_HW_ACCESS_READ);
		if (rv < 0) {
			qdma_log_error(
				"%s: Failed to read prefetch context, err:%d",
				__func__, rv);
			return rv;
		}
		qdma_acc_fill_pfetch_ctxt(&prefetch_ctx);
	}

	if (cmpt_valid) {
		/* completion context read */
		rv = qdma_cpm_cmpt_ctx_conf(dev_hndl,
			hw_qid, &cmpt_ctx, QDMA_HW_ACCESS_READ);
		if (rv < 0) {
			qdma_log_error(
				"%s: Failed to read completion context, err:%d",
				__func__, rv);
			return rv;
		}
		qdma_acc_fill_cmpt_ctxt(&cmpt_ctx);
	}

	rv = dump_context(buf, buflen, pfetch_valid, cmpt_valid);

	return rv;
}

/*****************************************************************************/
/**
 * qdma_cpm_init_ctxt_memory() - Initialize the context for all queues
 *
 * @dev_hndl    :	device handle
 *
 * Return       :	0   - success and < 0 - failure
 *****************************************************************************/

int qdma_cpm_init_ctxt_memory(void *dev_hndl)
{
#ifdef ENABLE_INIT_CTXT_MEMORY
	uint32_t data[QDMA_REG_IND_CTXT_REG_COUNT];
	uint16_t i = 0;
	struct qdma_dev_attributes dev_info;

	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}

	qdma_memset(data, 0, sizeof(uint32_t) * QDMA_REG_IND_CTXT_REG_COUNT);
	qdma_cpm_get_device_attributes(dev_hndl, &dev_info);
	qdma_log_info("%s: clearing the context for all qs",
			__func__);
	for (; i < dev_info.num_qs; i++) {
		int sel = QDMA_CTXT_SEL_SW_C2H;
		int rv;

		for (; sel <= QDMA_CTXT_SEL_PFTCH; sel++) {
			/** if the st mode(h2c/c2h) not enabled
			 *  in the design, then skip the PFTCH
			 *  and CMPT context setup
			 */
			if ((dev_info.st_en == 0) &&
			    (sel == QDMA_CTXT_SEL_PFTCH ||
				sel == QDMA_CTXT_SEL_CMPT)) {
				qdma_log_debug("%s: ST context is skipped:",
					__func__);
				qdma_log_debug(" sel = %d", sel);
				continue;
			}

			rv = qdma_cpm_indirect_reg_clear(dev_hndl,
					(enum ind_ctxt_cmd_sel)sel, i);
			if (rv < 0)
				return rv;
		}
	}

	/* fmap */
	for (i = 0; i < dev_info.num_pfs; i++)
		qdma_cpm_fmap_clear(dev_hndl, i);
#else
	if (!dev_hndl) {
		qdma_log_error("%s: dev_handle is NULL, err:%d\n",
					__func__, -QDMA_ERR_INV_PARAM);
		return -QDMA_ERR_INV_PARAM;
	}
#endif
	return 0;
}
