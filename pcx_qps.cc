
// TODO: Add license
#include "pcx_qps.h"

PcxQp::PcxQp(VerbCtx *ctx)
    : wqe_count(0), cqe_count(0), recv_enables(0), ctx(ctx), qp(NULL),
      ibqp(NULL), ibcq(NULL) {}

PcxQp::~PcxQp() {}

void PcxQp::db() { this->qp->db(); }

void PcxQp::poll() { this->qp->poll(); }

void PcxQp::print() {
    this->qp->print_sq();
    this->qp->print_cq();
}

void PcxQp::fin() { this->qp->fin(); }

struct ibv_cq *PcxQp::cd_create_cq(VerbCtx *verb_ctx, int cqe, void *cq_context,
                                   struct ibv_comp_channel *channel,
                                   int comp_vector) {
    if (cqe == 0) {
        ++cqe;
    }

    struct ibv_cq *cq =
        ibv_create_cq(verb_ctx->context, cqe, cq_context, channel, comp_vector);

    if (!cq) {
        PERR(CQCreateFailed);
    }

    struct ibv_exp_cq_attr attr;
    attr.cq_cap_flags = IBV_EXP_CQ_IGNORE_OVERRUN;
    attr.comp_mask = IBV_EXP_CQ_ATTR_CQ_CAP_FLAGS;

    int res = ibv_exp_modify_cq(cq, &attr, IBV_EXP_CQ_CAP_FLAGS);
    if (res) {
        PERR(CQModifyFailed);
    }

    return cq;
}

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

void TransportQp::set_pair(PcxQp *pair_) {
    this->pair = pair_;
};

const PcxQp *TransportQp::get_pair() {
    return this->pair;
};

struct ibv_qp *TransportQp::rc_qp_create(struct ibv_cq *cq, VerbCtx *verb_ctx,
                                         uint16_t send_wq_size,
                                         uint16_t recv_rq_size,
                                         struct ibv_cq *s_cq, int slaveRecv,
                                         int slaveSend) {
    struct ibv_exp_qp_init_attr init_attr;
    struct ibv_qp_attr attr;
    memset(&init_attr, 0, sizeof(init_attr));
    init_attr.qp_context = NULL;
    init_attr.send_cq = (s_cq == NULL) ? cq : s_cq;
    init_attr.recv_cq = cq;
    init_attr.cap.max_send_wr = send_wq_size;
    init_attr.cap.max_recv_wr = recv_rq_size;
    init_attr.cap.max_send_sge = 1;
    init_attr.cap.max_recv_sge = 1;
    init_attr.qp_type = IBV_QPT_RC;
    init_attr.pd = verb_ctx->pd;
    init_attr.comp_mask =
        IBV_QP_INIT_ATTR_PD | IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS;
    init_attr.exp_create_flags = IBV_EXP_QP_CREATE_CROSS_CHANNEL |
                                 IBV_EXP_QP_CREATE_IGNORE_SQ_OVERFLOW |
                                 IBV_EXP_QP_CREATE_IGNORE_RQ_OVERFLOW;

    if (slaveSend) {
        init_attr.exp_create_flags |= IBV_EXP_QP_CREATE_MANAGED_SEND;
    }

    if (slaveRecv) {
        init_attr.exp_create_flags |= IBV_EXP_QP_CREATE_MANAGED_RECV;
    }

    struct ibv_qp *qp = ibv_exp_create_qp(verb_ctx->context, &init_attr);

    struct ibv_qp_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.pkey_index = 0;
    qp_attr.port_num = 1;
    qp_attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;

    if (ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                                        IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        PERR(QPInitFailed);
    }

    return qp;
}

ManagementQp::ManagementQp(VerbCtx *ctx)
    : PcxQp(ctx), last_qp(0), has_stack(false) {}

ManagementQp::~ManagementQp() {
    delete (qp);
    ibv_destroy_qp(ibqp);
    ibv_destroy_cq(ibcq);
}

void ManagementQp::init() {

    if (!ctx) {
        PERR(MissingContext);
    }

    this->ibcq = cd_create_cq(ctx, cqe_count);
    if (!this->ibcq) {
        PERR(CQCreateFailed);
    }

    this->ibqp = create_management_qp(this->ibcq, ctx, wqe_count);
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

LambdaInstruction
ManagementQp::cd_recv_enable(PcxQp *slave_qp) { // called from PcxQp constructor
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

struct ibv_qp *ManagementQp::create_management_qp(struct ibv_cq *cq,
                                                  VerbCtx *verb_ctx,
                                                  uint16_t send_wq_size) {

    int rc = PCOLL_SUCCESS;
    struct ibv_exp_qp_init_attr init_attr;
    struct ibv_qp_attr attr;
    struct ibv_qp *_mq;

    memset(&init_attr, 0, sizeof(init_attr));
    init_attr.qp_context = NULL;
    init_attr.send_cq = cq;
    init_attr.recv_cq = cq;
    init_attr.srq = NULL;
    init_attr.cap.max_send_wr = send_wq_size;
    init_attr.cap.max_recv_wr = 0;
    init_attr.cap.max_send_sge = 1;
    init_attr.cap.max_recv_sge = 1;
    init_attr.cap.max_inline_data = 0;
    init_attr.qp_type = IBV_QPT_RC;
    init_attr.sq_sig_all = 0;
    init_attr.pd = verb_ctx->pd;
    init_attr.comp_mask =
        IBV_QP_INIT_ATTR_PD | IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS;
    init_attr.exp_create_flags = IBV_EXP_QP_CREATE_CROSS_CHANNEL |
                                 IBV_EXP_QP_CREATE_IGNORE_SQ_OVERFLOW |
                                 IBV_EXP_QP_CREATE_IGNORE_RQ_OVERFLOW;
    ;
    _mq = ibv_exp_create_qp(verb_ctx->context, &init_attr);

    if (NULL == _mq) {
        rc = PCOLL_ERROR;
    }

    if (rc == PCOLL_SUCCESS) {
        attr.qp_state = IBV_QPS_INIT;
        attr.pkey_index = 0;
        attr.port_num = 1;
        attr.qp_access_flags = 0;

        rc = ibv_modify_qp(_mq, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                                           IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
        if (rc) {
            rc = PCOLL_ERROR;
        }
    }

    if (rc == PCOLL_SUCCESS) {
        union ibv_gid gid;
        memset(&attr, 0, sizeof(attr));
        attr.qp_state = IBV_QPS_RTR;
        attr.path_mtu = IBV_MTU_1024;
        attr.dest_qp_num = _mq->qp_num;
        attr.rq_psn = 0;
        attr.max_dest_rd_atomic = 1;
        attr.min_rnr_timer = 12;
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.grh.sgid_index = GID_INDEX;
        attr.ah_attr.dlid = 0;
        attr.ah_attr.sl = 0;
        attr.ah_attr.src_path_bits = 0;
        attr.ah_attr.port_num = 1;

        if (ibv_query_gid(verb_ctx->context, 1, GID_INDEX, &gid)) {
            fprintf(stderr, "can't read sgid of index %d\n", GID_INDEX);
            // PERR(CantReadsGid);
        }

        attr.ah_attr.grh.dgid = gid;

        rc = ibv_modify_qp(
            _mq, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                            IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                            IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);

        if (rc) {
            PERR(QpFailedRTR);
        }
    }

    if (rc == PCOLL_SUCCESS) {
        attr.qp_state = IBV_QPS_RTS;
        attr.timeout = 14;
        attr.retry_cnt = 7;
        attr.rnr_retry = 7;
        attr.sq_psn = 0;
        attr.max_rd_atomic = 1;
        rc = ibv_modify_qp(_mq, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT |
                                           IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                                           IBV_QP_SQ_PSN |
                                           IBV_QP_MAX_QP_RD_ATOMIC);
        if (rc) {
            rc = PCOLL_ERROR;
        }
    }
    return _mq;
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
    ibcq = cd_create_cq(ctx, cqe_count);
    if (!ibcq) {
        PERR(CQCreateFailed);
    }

    ibqp = rc_qp_create(ibcq, ctx, wqe_count, cqe_count);
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
    LambdaInstruction lambda =
        [this, local, remote, num_vectors, op, type, require_cmpl]() {
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
    LambdaInstruction lambda =
        [this, local, remote, num_vectors, op, type, require_cmpl]() {
        this->qp->reduce_write(local, remote, num_vectors, op, type,
                               require_cmpl);
    };
    return lambda;
}
