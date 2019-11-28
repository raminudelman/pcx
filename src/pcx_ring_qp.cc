// TODO: Add liscense
#include "pcx_ring_qp.h"

RingQp::RingQp(VerbCtx *ctx, PipeMem *incomingBuffer)
    : TransportQp(ctx), incoming(incomingBuffer) {
    this->has_scq = true;
}

RingQp::~RingQp() {
    delete (qp);
    ibv_destroy_qp(ibqp);
    ibv_destroy_cq(ibcq);
}

void RingQp::init_rc_qp() {
    ibcq = ctx_->create_coredirect_cq(cqe_count);
    if (!ibcq) {
        PERR(CQCreateFailed);
    }

    ibscq = ctx_->create_coredirect_cq(scqe_count);
    if (!ibscq) {
        PERR(CQCreateFailed);
    }

    ibqp =
        ctx_->create_coredirect_slave_rc_qp(ibcq, wqe_count, cqe_count, ibscq);
    if (!ibqp) {
        PERR(QPCreateFailed);
    }
}

void RingQp::init_qp_ctx() {
    qp = new qp_ctx(ibqp, ibcq, wqe_count, cqe_count, ibscq, scqe_count);

    if (this->pair->qp) {
        this->pair->qp->set_pair(this->qp);
        this->qp->set_pair(this->pair->qp);
    }
    QP_PRINT("RingQP initiated (ID = %d, wqe_count = %d, cqe_count = %d, "
             "scqe_count = %d, peer QP ID = %d) \n",
             id, wqe_count, cqe_count, scqe_count, pair->id);
}

rd_peer_info_t RingQp::get_local_info() {
    rd_peer_info_t local_info;
    local_info.buf = (uintptr_t)(*incoming)[0].sg()->addr;
    local_info.rkey = (*incoming)[0].getMr()->rkey;
    rc_qp_get_addr(ibqp, &local_info.addr);
    return local_info;
}

void RingQp::set_remote_info(rd_peer_info_t remote_info) {
    RemoteMem tmp_remote(remote_info.buf, remote_info.rkey);
    remote =
        new PipeMem(incoming->getLength(), incoming->getDepth(), &tmp_remote);
    rc_qp_connect(&remote_info.addr, ibqp);
}

LambdaInstruction RingQp::write(NetMem *local, size_t pos, bool require_cmpl) {
    ++wqe_count;
    ++this->pair->cqe_count;

    if (require_cmpl) {
        if (has_scq) {
            ++scqe_count;
        } else {
            ++cqe_count;
        }
    }

    // TODO: Consider calling here the PcxQp::write(NetMem *local, RefMem
    // *remote, bool require_cmpl); (Overload if this function does not exist)
    struct ibv_sge lsg = (*local->sg());

    LambdaInstruction lambda = [this, lsg, pos, require_cmpl]() {
        this->qp->write(&lsg, (*this->remote)[pos].sg(), require_cmpl);
    };

    return lambda;
}

LambdaInstruction RingQp::reduce_write(NetMem *local, size_t pos,
                                       uint16_t num_vectors, uint8_t op,
                                       uint8_t type, bool require_cmpl) {
    wqe_count += 2;
    ++this->pair->cqe_count;

    if (require_cmpl) {
        if (has_scq) {
            ++scqe_count;
        } else {
            ++cqe_count;
        }
    }

    // TODO: Consider using PcxQp::reduce_write(NetMem *local, RefMem *remote,
    // uint16_t num_vectors, uint8_t op, uint8_t type, bool require_cmpl). Added
    // this function in case it does not exist

    LambdaInstruction lambda = [this, local, num_vectors, op, type, pos,
                                require_cmpl]() {
        RefMem ref = ((*this->remote)[pos]);
        this->qp->reduce_write(local, &ref, num_vectors, op, type,
                               require_cmpl);
    };
    return lambda;
}

void RingQps::init() {
    rd_peer_info_t local_info_left, remote_info_left;
    rd_peer_info_t local_info_right, remote_info_right;

    left->init_rc_qp();
    right->init_rc_qp();

    local_info_left = left->get_local_info();
    local_info_right = right->get_local_info();
    left->ctx_->mtx.unlock();
    (*ring_exchange)(comm, &local_info_left, &local_info_right,
                     &remote_info_left, &remote_info_right,
                     sizeof(rd_peer_info_t), leftRank, rightRank, tag1, tag2);
    left->ctx_->mtx.lock();

    left->set_remote_info(remote_info_left);
    right->set_remote_info(remote_info_right);

    // TODO: Do we need lock and barrier here?

    left->init_qp_ctx();
    right->init_qp_ctx();
}
