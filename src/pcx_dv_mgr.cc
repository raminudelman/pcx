/*
 * Copyright (c) 2019-present, Mellanox Technologies Ltd. 
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "pcx_dv_mgr.h"
#include "assert.h"

static inline void mlx5dv_set_remote_data_seg(
    struct mlx5_wqe_raddr_seg *seg, uint64_t addr,
    uint32_t rkey) { // TODO: This function should be added to mlx5dv.h !
    seg->raddr = htobe64(addr);
    seg->rkey = htonl(rkey);
    seg->reserved = 0;
}

static void mlx5dv_set_vectorcalc_seg(
    struct mlx5_wqe_vectorcalc_seg *vseg, uint8_t op, uint8_t operand_type,
    uint8_t chunk_size,
    uint16_t
        num_of_vectors) { // TODO: This function should be added to mlx5dv.h !
    vseg->calc_operation = htobe32(op << 24);
    vseg->options = htobe32(operand_type << 24 |
#if __BYTE_ORDER == __LITTLE_ENDIAN
                            1UL << 22 |
#elif __BYTE_ORDER == __BIG_ENDIAN
#else
#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN
#endif
                            chunk_size << 16 | num_of_vectors);
}

static inline void
mlx5dv_set_coredirect_seg(struct mlx5_wqe_coredirect_seg *seg, uint32_t index,
                          uint32_t number) {
    seg->index = htonl(index);
    seg->number = htonl(number);
}

ValRearmTasks::ValRearmTasks() {
    size = 0;
    buf_size = 4;
    ptrs = (uint32_t **)malloc(buf_size * sizeof(uint32_t *));
}

ValRearmTasks::~ValRearmTasks() { free(ptrs); }

void ValRearmTasks::add(uint32_t *ptr) {
    ++size;
    if (size > buf_size) {
        uint32_t **tmp_ptrs =
            (uint32_t **)malloc((buf_size * 2) * sizeof(uint32_t *));
        int i = 0;
        for (i = 0; i < buf_size; ++i) {
            tmp_ptrs[i] = ptrs[i];
        }
        buf_size = buf_size * 2;
        free(ptrs);
        ptrs = tmp_ptrs;
    }
    ptrs[size - 1] = ptr;
}

void ValRearmTasks::exec(uint32_t increment, uint32_t src_offset,
                         uint32_t dst_offset) {
    for (int i = 0; i < size; ++i) {
        ptrs[i][dst_offset] = htonl(ntohl(ptrs[i][src_offset]) + increment);
    }
}

void RearmTasks::exec(uint32_t offset, int phase, int dups) {
    uint32_t src_offset = phase * offset;
    uint32_t dst_offset = ((phase + 1) % dups) * offset;
    for (MapIt it = this->map.begin(); it != map.end(); ++it) {
        it->second.exec(it->first, src_offset, dst_offset);
    }
}

void RearmTasks::add(uint32_t *ptr, int inc) {
    // MapIt it =  map.find(inc);
    // if (it != map.end()){
    //	map[inc].add(ptr);
    //}
    map[inc].add(ptr);
}

qp_ctx::cq_ctx::cq_ctx(struct ibv_cq *cq, size_t num_of_cqes) {
    this->cmpl_cnt = 0;
    int ret;
    struct mlx5dv_obj dv_obj = {};
    memset((void *)&dv_obj, 0, sizeof(struct mlx5dv_obj));

    this->cq = (struct mlx5dv_cq *)malloc(sizeof(struct mlx5dv_cq));
    dv_obj.cq.in = cq;
    dv_obj.cq.out = this->cq;

    ret = mlx5dv_init_obj(&dv_obj, MLX5DV_OBJ_CQ);
    this->cqes = num_of_cqes;
}

qp_ctx::cq_ctx::~cq_ctx() { free(this->cq); }

qp_ctx::qp_ctx(struct ibv_qp *qp, struct ibv_cq *cq, size_t num_of_wqes,
               size_t num_of_cqes, struct ibv_cq *scq, size_t num_of_send_cqes)
    : qp_ctx(qp, cq, num_of_wqes, num_of_cqes) {
    this->scq = new cq_ctx(scq, num_of_send_cqes);
}

qp_ctx::qp_ctx(
    struct ibv_qp *qp, struct ibv_cq *cq, size_t num_of_wqes,
    size_t num_of_cqes) { // TODO: Consider useing a single constructor of
                          // qp_ctx with default value on scq argument as
                          // nullptr and 0 for num_of_send_cqes.

    int ret; // TODO: Not is use. Consider removing.

    struct mlx5dv_obj dv_obj = {};
    memset((void *)&dv_obj, 0, sizeof(struct mlx5dv_obj));

    this->qp = (struct mlx5dv_qp *)malloc(sizeof(struct mlx5dv_qp));
    this->cq = (struct mlx5dv_cq *)malloc(sizeof(struct mlx5dv_cq));
    this->qpn = qp->qp_num;

    dv_obj.qp.in = qp;
    dv_obj.qp.out = this->qp;
    dv_obj.cq.in = cq;
    dv_obj.cq.out = this->cq;

    // pingpong_context
    ret = mlx5dv_init_obj(&dv_obj, MLX5DV_OBJ_QP | MLX5DV_OBJ_CQ);
    this->write_cnt = 0;
    this->current_wqe_index_to_exec = 0;
    this->bf_reg.qpn_ds = htobe32(qpn << 8);
    this->phase = 0;
    this->cmpl_cnt = 0;
    this->pair = this; // default pair for qp will be itself.
    this->scq = NULL;
    int rounded_num_of_wqes = num_of_wqes;

    if (num_of_wqes > this->qp->sq.wqe_cnt) {
        fprintf(stderr,
                "ERROR - SQ size is not big enough to hold all wqes!\n");
        return;
    }

    // Find the minimal number of wqes that is larger than num_of_wqes
    // that devides the SQ size without a reminder.
    // Note: wqe_cnt is always of power of 2.
    while (rounded_num_of_wqes && this->qp->sq.wqe_cnt % rounded_num_of_wqes) {
        ++rounded_num_of_wqes;
    }

    this->wqes = rounded_num_of_wqes;
    this->cqes = num_of_cqes;

    this->number_of_duplicates = (this->qp->sq.wqe_cnt / rounded_num_of_wqes);
    this->offset =
        (this->qp->sq.stride * rounded_num_of_wqes) / sizeof(uint32_t);

    // printf("wqe_cnt = %d, stride =
    // %d\n",this->qp->sq.wqe_cnt,this->qp->sq.stride);
    // printf("cqn num = %d\n", this->cq->cqn);

    // Convert the num_of_cqes into an index, that starts from 0.
    this->poll_cnt = num_of_cqes - 1;
    volatile void *tar =
        (volatile void *)((volatile char *)this->cq->buf +
                          ((this->poll_cnt) & (this->cq->cqe_cnt - 1)) *
                              this->cq->cqe_size);
    this->cur_cqe = (volatile struct mlx5_cqe64 *)tar;
}

qp_ctx::~qp_ctx() {
    if (scq) {
        delete (scq);
    }
    free(this->qp);
    free(this->cq);
}

void qp_ctx::set_pair(qp_ctx *qp) { this->pair = qp; };

int qp_ctx::poll() { // TODO: Change the name of this function to: is_finished()
                     // or something like it.

    // The opcode if the CQE indicates the state of the QCE. For example, the
    // states can be:
    //     MLX5_CQE_INVALID  : The HW did not written this CQE into the CQ
    //     MLX5_CQE_RESP_ERR : The CQE contains an error
    //     MLX5_CQE_REQ_ERR  : The request that generated this CQE had an error
    //
    // According to the 64B CQE format (PRM v0.49 External, Section 8.18.1.1,
    // Table 114), the opcode of the CQE is located in bits 4:7 in the 0x3C DW.
    uint8_t opcode =
        this->cur_cqe->op_own >>
        4; // TODO: Why not use the function mlx5dv_get_cqe_opcode(struct
           // mlx5_cqe64*) @ Line 754 @
           // https://github.com/linux-rdma/rdma-core/blob/7a7ef7a247959e41b09e6b12ae0190cfc572002b/providers/mlx5/mlx5dv.h
           // ? For using the mlx5dv_get_cqe_opcode function, the cur_cqe cannot
           // be defined as volatile (does it matters?).

    // First need to check if the CQE is not invalid, meaning the hardware
    // completed the WR that generated the CQE.
    // In case the CQE was generated by the hardware (opcode != invalid), need
    // to
    // make sure that the hardware finished writting the CQE into the CQ and
    // passed the ownership of the CQE to the software. This is does using the
    // 'owner' bit that is defined in the CQE. CQE Ownership is explained in
    // the PRM v0.49 External, Section 8.18.3.1, CQE Ownership.
    if (opcode != MLX5_CQE_INVALID &&
        !((cur_cqe->op_own & MLX5_CQE_OWNER_MASK) ^
          !!((this->poll_cnt) &
             (this->cq
                  ->cqe_cnt)))) { // TODO: The following 'if' statement look
                                  // very similar to the function
                                  // get_sw_cqe(...) @ Line122 @
                                  // https://github.com/linux-rdma/rdma-core/blob/f3df671d32696be9d3e755986afbb470bc859f65/providers/mlx5/cq.c
                                  // . Consider using that function instead of
                                  // explicitly writing this code again.

        // Updating the Consumer Index (CI) in CQ Doorbell record as described
        // in
        // the PRM v0.49 External, Section 8.18.2, CQ DoorBell Record.
        this->cq->dbrec[0] = htobe32(
            (this->poll_cnt) &
            0xffffff); // TODO: Why not use the function update_cons_index(...)
                       // @ Line 142 @
                       // https://github.com/linux-rdma/rdma-core/blob/f3df671d32696be9d3e755986afbb470bc859f65/providers/mlx5/cq.c
                       // ?

        // Updating the poll_cnt variable to hold the index of the next CQE that
        // will indicate that the next collective operation is finished.
        (this->poll_cnt) += cqes; // Adding the expected number of CQEs that
                                  // will be generated during the next all
                                  // reduce.

        // Find the location of the index of the next expected CQE.
        // Notice: The cqe_cnt always equals to some number which is some power
        // 2.
        //         This means that (cqe_cnt - 1) has a binary representation of
        //         only 1's, meaning (cqe_cnt - 1) = 0b111...111.
        //         The expression (this->poll_cnt & (this->cq->cqe_cnt - 1))
        //         is the same as doing (this->poll_cnt % this->cq->cqe_cnt) //
        // TODO: Verify this is correct.
        volatile void *next_expected_cqe_location =
            (volatile void *)((volatile char *)this->cq->buf +
                              (this->poll_cnt & (this->cq->cqe_cnt - 1)) *
                                  this->cq->cqe_size);

        // Updating the pointer to the location where the next expected CQE
        // will be written by the hardware (in case the hardware will execute
        // another collective operation)
        this->cur_cqe =
            (volatile struct mlx5_cqe64 *)next_expected_cqe_location;
        return 1;
    } else {
        return 0;
    }
}

void qp_ctx::db() {
    current_wqe_index_to_exec += (this->wqes);
    //  First 4 bytes in the "Ctrl Segment Format" (See PRM Table 42, "General
    // - Ctrl Segment Format")
    bf_reg.opmod_idx_opcode = htobe32(current_wqe_index_to_exec << 8); 

    pcx_device_barrier();
    // Ring the Doorbell
    qp->dbrec[1] = htobe32(current_wqe_index_to_exec); 
    pcx_pci_store_fence();

    // Send DoorBells are rung in blue flame registers by writing the first 8 
    // bytes of the WQE (that contains post counter, DS and QP/SQ number) to 
    // offset 0 of the blue flame register (See PRM Table 42, "General - Ctrl 
    // Segment Format"). This post counter points to the beginning of the WQE 
    // to be executed. Note that this is different than the counter in the DB 
    // record which points to the next empty WQEBB. Send DBs of a QP/SQ can be 
    // rung on specific Blue Flame registers and cannot be interleaved on
    // multiple Blue Flame registers.
    *(uint64_t *)qp->bf.reg = *(uint64_t *)&(bf_reg);
    pcx_pci_store_fence();
}

void qp_ctx::send_credit() {
    struct mlx5_wqe_ctrl_seg *ctrl;
    struct mlx5_wqe_data_seg *dseg;
    // DS holds the WQE size in octowords (16-byte units).
    // DS accounts for all the segments in the WQE as summarized in WQE
    // construction
    const uint8_t ds = 1;
    int wqe_count = qp->sq.wqe_cnt;
    ctrl =
        (struct mlx5_wqe_ctrl_seg *)((char *)qp->sq.buf +
                                     qp->sq.stride * ((write_cnt) % wqe_count));
    mlx5dv_set_ctrl_seg(ctrl, (write_cnt), MLX5_OPCODE_SEND_IMM, 0, qpn, 0, ds,
                        0, 0);
    write_cnt += 1;
    pair->cmpl_cnt += 1;
}

void qp_ctx::write(const struct ibv_sge *local, const struct ibv_sge *remote,
                   bool require_cmpl) {
    struct mlx5_wqe_ctrl_seg *ctrl;
    struct mlx5_wqe_raddr_seg *rseg;
    struct mlx5_wqe_data_seg *dseg;
    // DS holds the WQE size in octowords (16-byte units).
    // DS accounts for all the segments in the WQE as summarized in WQE
    // construction
    const uint8_t ds = 3;
    int wqe_count = qp->sq.wqe_cnt;
    ctrl =
        (struct mlx5_wqe_ctrl_seg *)((char *)qp->sq.buf +
                                     qp->sq.stride * ((write_cnt) % wqe_count));
    uint8_t fm_ce_se = ((require_cmpl) ? 8 : 0);
    mlx5dv_set_ctrl_seg(ctrl, (write_cnt), MLX5_OPCODE_RDMA_WRITE_IMM, 0, qpn,
                        fm_ce_se, ds, 0, 0);
    rseg = (struct mlx5_wqe_raddr_seg *)(ctrl + 1);
    mlx5dv_set_remote_data_seg(rseg, remote->addr, remote->lkey);
    dseg = (struct mlx5_wqe_data_seg *)(rseg + 1);
    mlx5dv_set_data_seg(dseg, local->length, local->lkey, local->addr);
    write_cnt += 1;

    if (require_cmpl) {
        if (!scq) {
            this->cmpl_cnt += 1;
        } else {
            scq->cmpl_cnt += 1;
        }
    }

    pair->cmpl_cnt += 1;
}

void qp_ctx::write(NetMem *local, NetMem *remote, bool require_cmpl) {
    write(local->sg(), remote->sg(), require_cmpl);
};

void qp_ctx::write(NetMem *local, RefMem remote, bool require_cmpl) {
    write(local->sg(), remote.sg(), require_cmpl);
};

void qp_ctx::reduce_write(const struct ibv_sge *local,
                          const struct ibv_sge *remote, uint16_t num_vectors,
                          uint8_t op, uint8_t type, bool require_cmpl) {
    struct mlx5_wqe_ctrl_seg *ctrl;       // 1
    struct mlx5_wqe_raddr_seg *rseg;      // 1
    struct mlx5_wqe_vectorcalc_seg *vseg; // 2
    struct mlx5_wqe_data_seg *dseg;       // 1
    // DS holds the WQE size in octowords (16-byte units).
    // DS accounts for all the segments in the WQE as summarized in WQE
    // construction
    const uint8_t ds = 5; // 1 + 1 + 2 + 1
    const uint8_t chunk_size = 4;
    int wqe_count = qp->sq.wqe_cnt;
    ctrl =
        (struct mlx5_wqe_ctrl_seg *)((char *)qp->sq.buf +
                                     qp->sq.stride * ((write_cnt) % wqe_count));
    uint8_t fm_ce_se = ((require_cmpl) ? 8 : 0);
    mlx5dv_set_ctrl_seg(ctrl, (write_cnt), MLX5_OPCODE_RDMA_WRITE_IMM, 0xff,
                        qpn, fm_ce_se, ds, 0, 0);
    rseg = (struct mlx5_wqe_raddr_seg *)(ctrl + 1);
    mlx5dv_set_remote_data_seg(rseg, remote->addr, remote->lkey);
    vseg = (struct mlx5_wqe_vectorcalc_seg *)(rseg + 1);
    mlx5dv_set_vectorcalc_seg(vseg, op, type, chunk_size, num_vectors);
    dseg = (struct mlx5_wqe_data_seg *)(vseg + 1);
    mlx5dv_set_data_seg(dseg, local->length, local->lkey, local->addr);
    write_cnt += 2;

    if (require_cmpl) {
        if (!scq) {
            this->cmpl_cnt += 1;
        } else {
            scq->cmpl_cnt += 1;
        }
    }

    pair->cmpl_cnt += 1;
}

void qp_ctx::reduce_write(NetMem *local, NetMem *remote, uint16_t num_vectors,
                          uint8_t op, uint8_t type, bool require_cmpl) {
    reduce_write(local->sg(), remote->sg(), num_vectors, op, type,
                 require_cmpl);
};

void qp_ctx::reduce_write(NetMem *local, RefMem &remote, uint16_t num_vectors,
                          uint8_t op, uint8_t type, bool require_cmpl) {
    reduce_write(local->sg(), remote.sg(), num_vectors, op, type, require_cmpl);
};

void qp_ctx::cd_send_enable(qp_ctx *slave_qp) {
    struct mlx5_wqe_ctrl_seg *ctrl;       // 1
    struct mlx5_wqe_coredirect_seg *wseg; // 1
    int ctrl_seg_size_in_bytes = 16;
    int wait_seg_size_in_bytes = 16;
    // DS holds the WQE size in octowords (16-byte units).
    // DS accounts for all the segments in the WQE as summarized in WQE
    // construction
    const uint8_t ds = (ctrl_seg_size_in_bytes + wait_seg_size_in_bytes) /
                       16; // WQE size in octaword size (16-Byte)
    int wqe_count = qp->sq.wqe_cnt;
    ctrl =
        (struct mlx5_wqe_ctrl_seg *)((char *)qp->sq.buf +
                                     qp->sq.stride * ((write_cnt) % wqe_count));
    mlx5dv_set_ctrl_seg(ctrl, (write_cnt), 0x17, 0x00, qpn, CE, ds, 0, 0);
    wseg = (struct mlx5_wqe_coredirect_seg *)(ctrl + 1);
    mlx5dv_set_coredirect_seg(wseg, slave_qp->write_cnt, slave_qp->qpn);
    this->tasks.add((uint32_t *)&(wseg->index), slave_qp->wqes);
    write_cnt += 1;
}

void qp_ctx::cd_recv_enable(qp_ctx *slave_qp) {
    struct mlx5_wqe_ctrl_seg *ctrl;       // 1
    struct mlx5_wqe_coredirect_seg *wseg; // 1
    // DS holds the WQE size in octowords (16-byte units).
    // DS accounts for all the segments in the WQE as summarized in WQE
    // construction
    const uint8_t ds = 2;
    int wqe_count = qp->sq.wqe_cnt;
    ctrl =
        (struct mlx5_wqe_ctrl_seg *)((char *)qp->sq.buf +
                                     qp->sq.stride * ((write_cnt) % wqe_count));
    mlx5dv_set_ctrl_seg(ctrl, (write_cnt), 0x16, 0x00, qpn, CE, ds, 0, 0);
    wseg = (struct mlx5_wqe_coredirect_seg *)(ctrl + 1);
    mlx5dv_set_coredirect_seg(wseg, 0x6fff, slave_qp->qpn);
    this->tasks.add((uint32_t *)&(wseg->index), slave_qp->cqes);
    write_cnt += 1;
}

void qp_ctx::cd_wait(qp_ctx *slave_qp, bool wait_scq) {
    if (wait_scq) {
        if (!slave_qp->scq) {
            this->cd_wait(slave_qp, false);
            return;
        }
    }

    struct mlx5_wqe_ctrl_seg *ctrl;       // 1
    struct mlx5_wqe_coredirect_seg *wseg; // 1
    // DS holds the WQE size in octowords (16-byte units).
    // DS accounts for all the segments in the WQE as summarized in WQE
    // construction
    const uint8_t ds = 2;
    int wqe_count = qp->sq.wqe_cnt;
    ctrl =
        (struct mlx5_wqe_ctrl_seg *)((char *)qp->sq.buf +
                                     qp->sq.stride * ((write_cnt) % wqe_count));
    mlx5dv_set_ctrl_seg(ctrl, (write_cnt), 0x0f, 0x00, qpn, CE, ds, 0, 0);
    wseg = (struct mlx5_wqe_coredirect_seg *)(ctrl + 1);
    uint32_t index =
        ((wait_scq) ? (slave_qp->scq->cmpl_cnt - 1) : (slave_qp->cmpl_cnt - 1));
    uint32_t number = ((wait_scq) ? slave_qp->scq->cq->cqn : slave_qp->cq->cqn);
    mlx5dv_set_coredirect_seg(wseg, index, number);
    int cqes_inc = ((wait_scq) ? slave_qp->scq->cqes : slave_qp->cqes);
    this->tasks.add((uint32_t *)&(wseg->index), cqes_inc);
    write_cnt += 1;
}

void qp_ctx::nop(size_t num_pad) {
    struct mlx5_wqe_ctrl_seg *ctrl; // 1
    // DS holds the WQE size in octowords (16-byte units).
    // DS accounts for all the segments in the WQE as summarized in WQE
    // construction
    const uint8_t ds = (num_pad * (qp->sq.stride / 16));
    int wqe_count = qp->sq.wqe_cnt;
    ctrl =
        (struct mlx5_wqe_ctrl_seg *)((char *)qp->sq.buf +
                                     qp->sq.stride * ((write_cnt) % wqe_count));
    mlx5dv_set_ctrl_seg(ctrl, write_cnt, 0x00, 0x00, qpn, 0, ds, 0, 0);
    write_cnt += num_pad;
}

void qp_ctx::pad(int half) {
    int wqe_count = qp->sq.wqe_cnt;
    int pad_size = 8;
    int target_count = half ? (wqe_count / 2) : (wqe_count);
    if (write_cnt + pad_size > target_count) {
        printf("ERROR = wqe buffer exceeded! should be at least %d, is %d\n",
               write_cnt, target_count);
    } else if (write_cnt + pad_size < target_count / 2) {
        printf("ERROR = wqe buffer too big!\n");
    }

    while (write_cnt + pad_size < target_count) {
        this->nop(pad_size);
    }
    this->nop(target_count - write_cnt);
}

void qp_ctx::fin() {
    int pad_size = 8;
    int target_count = this->wqes;

    while (write_cnt + pad_size < target_count) {
        this->nop(pad_size);
    }
    this->nop(target_count - write_cnt);

    void *original = qp->sq.buf;
    size_t dup_size = qp->sq.stride * target_count;
    for (int dups = 1; dups < number_of_duplicates; ++dups) {
        void *duplicate_dst = (void *)((char *)original + dup_size * dups);
        memcpy(duplicate_dst, original, dup_size);
    }

    for (int dups = 1; dups < number_of_duplicates - 2; ++dups) {
        this->rearm();
    }
}

void qp_ctx::dup() {
    int wqe_count = qp->sq.wqe_cnt;
    void *first_half = qp->sq.buf;
    void *second_half =
        (void *)((char *)qp->sq.buf + qp->sq.stride * (wqe_count / 2));
    memcpy(second_half, first_half, qp->sq.stride * (wqe_count / 2));
}

void qp_ctx::rearm() {
    this->tasks.exec(this->offset, this->phase, this->number_of_duplicates);
    phase = (phase + 1) % this->number_of_duplicates;
}

void qp_ctx::print_sq() {
    fprintf(stderr, "SQ %lX: %dX%d (WQE Size [Bytes] X WQE Count)\n", qpn,
            qp->sq.stride, qp->sq.wqe_cnt);
    print_buffer(this->qp->sq.buf, qp->sq.stride * qp->sq.wqe_cnt);
    fprintf(stderr, "\n");
}

void qp_ctx::print_rq() {
    fprintf(stderr, "RQ %lX: %dX%d (WQE Size [Bytes] X WQE Count)\n", qpn,
            qp->rq.stride, qp->rq.wqe_cnt);
    print_buffer(this->qp->rq.buf, qp->rq.stride * qp->rq.wqe_cnt);
    fprintf(stderr, "\n");
}

void qp_ctx::print_cq() {
    fprintf(stderr, "CQ %X:\n", cq->cqn);
    print_buffer(this->cq->buf, cq->cqe_size * cq->cqe_cnt);
    fprintf(stderr, "\n");
    if (this->scq) {
        fprintf(stderr, "Send CQ %X:\n", scq->cq->cqn);
        print_buffer(this->scq->cq->buf, cq->cqe_size * cq->cqe_cnt);
        fprintf(stderr, "\n");
    }
}

// count is the number of *bytes* within the buffer
void qp_ctx::print_buffer(volatile void *buf,
                          int count) { // TODO: Move to utils.cc file
    int i = 0;
    int line_width = 16;
    int line_count = (count / sizeof(int)) / line_width;
    int line_seperator = 16;
    for (int line = 0; line < line_count; ++line) {
        // After every 'line_seperator' lines, print \n
        if ((line > 0) && (line >= line_seperator) &&
            (line % line_seperator == 0)) {
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "#%02d: ", line);
        for (int column = 0; column < 16; ++column) {
            fprintf(stderr, "%08X  ",
                    ntohl(((int *)buf)[line * line_width + column]));
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n");
}
