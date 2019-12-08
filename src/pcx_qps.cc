/*
 * Copyright (c) 2019-present, Mellanox Technologies Ltd. 
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "pcx_qps.h"

PcxQp::PcxQp(VerbCtx *ctx)
    : wqe_count(0), cqe_count(0), recv_enables(0), ctx_(ctx), qp(NULL),
      ibqp(NULL), ibcq(NULL) {}

PcxQp::~PcxQp() {}

void PcxQp::db() { this->qp->db(); }

void PcxQp::poll() { this->qp->poll(); }

void PcxQp::print() {
    this->qp->print_sq();
    this->qp->print_cq();
}

void PcxQp::fin() { this->qp->fin(); }

TransportQp::TransportQp(VerbCtx *ctx)
    : PcxQp(ctx), scqe_count(0), ibscq(NULL), pair(this) {}

TransportQp::~TransportQp() {}

void TransportQp::init() {}

LambdaInstruction TransportQp::send_credit() {
    ++wqe_count;
    ++pair->cqe_count;
    LambdaInstruction lambda = [this]() { qp->send_credit(); };
    return lambda;
}

void TransportQp::set_pair(PcxQp *pair_) { this->pair = pair_; };

const PcxQp *TransportQp::get_pair() { return this->pair; };

ManagementQp::ManagementQp(VerbCtx *ctx)
    : PcxQp(ctx), last_qp(0), has_stack(false) {}

ManagementQp::~ManagementQp() {
    delete (qp);
    ibv_destroy_qp(ibqp);
    ibv_destroy_cq(ibcq);
}

void ManagementQp::init() {

    if (!ctx_) {
        PERR(MissingContext);
    }

    this->ibcq = ctx_->create_coredirect_cq(cqe_count);
    if (!this->ibcq) {
        PERR(CQCreateFailed);
    }

    this->ibqp = ctx_->create_coredirect_master_qp(this->ibcq, wqe_count);
    this->qp = new qp_ctx(this->ibqp, this->ibcq, wqe_count, cqe_count);

    QP_PRINT(
        "ManagementQP initiated (ID = %d, wqe_count = %d, cqe_count = %d) \n",
        id, wqe_count, cqe_count);
}

LambdaInstruction ManagementQp::cd_send_enable(PcxQp *slave_qp) {
    ++wqe_count;
    LambdaInstruction lambda = [this, slave_qp]() {
        this->qp->cd_send_enable(slave_qp->qp);
    };
    return lambda;
}

LambdaInstruction ManagementQp::cd_recv_enable(PcxQp *slave_qp) {
    ++wqe_count;
    LambdaInstruction lambda = [this, slave_qp]() {
        this->qp->cd_recv_enable(slave_qp->qp);
    };
    ++recv_enables;
    return lambda;
}

LambdaInstruction ManagementQp::cd_wait(PcxQp *slave_qp, bool wait_scq) {
    ++wqe_count;
    LambdaInstruction lambda = [this, slave_qp, wait_scq]() {
        this->qp->cd_wait(slave_qp->qp, wait_scq);
    };
    return lambda;
}

LoopbackQp::LoopbackQp(VerbCtx *ctx) : TransportQp(ctx) {
    this->has_scq = false;
}

LoopbackQp::~LoopbackQp() {
    delete (qp);
    ibv_destroy_qp(ibqp);
    ibv_destroy_cq(ibcq);
}

void LoopbackQp::init() {
    this->ibcq = ctx_->create_coredirect_cq(cqe_count);
    if (!ibcq) {
        PERR(CQCreateFailed);
    }

    ibqp = ctx_->create_coredirect_slave_rc_qp(ibcq, wqe_count, cqe_count);
    peer_addr_t loopback_addr;
    rc_qp_get_addr(ibqp, &loopback_addr);
    rc_qp_connect(&loopback_addr, ibqp);
    qp = new qp_ctx(ibqp, ibcq, wqe_count, cqe_count);

    QP_PRINT("LoopbackQP initiated (ID = %d, wqe_count = %d, cqe_count = %d, "
             "peer QP ID = %d) \n",
             id, wqe_count, cqe_count, pair->id);
}

LambdaInstruction LoopbackQp::write(NetMem *local, RefMem *remote,
                                    bool require_cmpl) {
    ++wqe_count;
    ++this->pair->cqe_count;

    if (require_cmpl) {
        if (has_scq) {
            ++scqe_count;
        } else {
            ++cqe_count;
        }
    }

    struct ibv_sge lsg = (*local->sg());
    struct ibv_sge rsg = (*remote->sg());

    LambdaInstruction lambda = [this, lsg, rsg, require_cmpl]() {
        this->qp->write(&lsg, &rsg, require_cmpl);
    };

    return lambda;
}

LambdaInstruction LoopbackQp::write(NetMem *local, NetMem *remote,
                                    bool require_cmpl) {
    ++wqe_count;
    ++this->pair->cqe_count;

    if (require_cmpl) {
        if (has_scq) {
            ++scqe_count;
        } else {
            ++cqe_count;
        }
    }

    struct ibv_sge lsg = (*local->sg());
    struct ibv_sge rsg = (*remote->sg());

    LambdaInstruction lambda = [this, lsg, rsg, require_cmpl]() {
        this->qp->write(&lsg, &rsg, require_cmpl);
    };

    return lambda;
}

LambdaInstruction LoopbackQp::reduce_write(
    UmrMem *local, NetMem *remote,
    uint16_t num_vectors, // TODO: Does someone use this function?
    uint8_t op, uint8_t type, bool require_cmpl) {
    wqe_count += 2;
    ++this->pair->cqe_count;
    LambdaInstruction lambda = [this, local, remote, num_vectors, op, type,
                                require_cmpl]() {
        this->qp->reduce_write(local, remote, num_vectors, op, type,
                               require_cmpl);
    };
    return lambda;
}

LambdaInstruction LoopbackQp::reduce_write(
    NetMem *local, NetMem *remote,
    uint16_t num_vectors, // TODO: Does someone use this function?
    uint8_t op, uint8_t type, bool require_cmpl) {
    wqe_count += 2;
    ++this->pair->cqe_count;
    LambdaInstruction lambda = [this, local, remote, num_vectors, op, type,
                                require_cmpl]() {
        this->qp->reduce_write(local, remote, num_vectors, op, type,
                               require_cmpl);
    };
    return lambda;
}
