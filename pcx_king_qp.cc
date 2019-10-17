// TODO: Add liscense
#include "pcx_king_qp.h"

DoublingQp::DoublingQp(VerbCtx *ctx, p2p_exchange_func func, void *comm,
                       uint32_t peer, uint32_t tag, NetMem *incomingBuffer)
    : TransportQp(ctx), incoming(incomingBuffer), comm_(comm), tag_(tag) {
    this->has_scq = true;
    using namespace std::placeholders;
    exchange = std::bind(func, comm, _1, _2, _3, peer, tag);
    barrier =
        std::bind(func, comm, _1, _2, _3, peer, tag + ((unsigned int)0xf000));
}

DoublingQp::~DoublingQp() {
    delete (remote);
    delete (qp);
    ibv_destroy_qp(ibqp);
    ibv_destroy_cq(ibcq);
}

void DoublingQp::init() {

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

    rd_peer_info_t local_info, remote_info;

    local_info.buf = (uintptr_t)incoming->sg()->addr;
    local_info.rkey = incoming->getMr()->rkey;
    rc_qp_get_addr(ibqp, &local_info.addr);

    ctx_->mtx.unlock();
    exchange((void *)&local_info, (void *)&remote_info, sizeof(local_info));
    ctx_->mtx.lock();

    /*
      fprintf(stderr,"sent: buf = %lu, rkey = %u, qpn = %lu, lid = %u , gid =
      %u,
      psn = %ld\n", local_info.buf,
                    local_info.rkey,
      local_info.addr.qpn,local_info.addr.lid,local_info.addr.gid,local_info.addr.psn
      );
      fprintf(stderr,"recv: buf = %lu, rkey = %u, qpn = %lu, lid = %u , gid =
      %u,
      psn = %ld\n", remote_info.buf, remote_info.rkey, remote_info.addr.qpn ,
                                                            remote_info.addr.lid,
      remote_info.addr.gid, remote_info.addr.psn );

    */

    remote = new RemoteMem(remote_info.buf, remote_info.rkey);
    rc_qp_connect(&remote_info.addr, ibqp);

    // barrier
    int ack;
    ctx_->mtx.unlock();
    barrier((void *)&ack, (void *)&ack, sizeof(int));
    ctx_->mtx.lock();

    qp = new qp_ctx(ibqp, ibcq, wqe_count, cqe_count, ibscq, scqe_count);

    QP_PRINT("DoublingQP initiated (ID = %d, wqe_count = %d, cqe_count = %d, "
             "scqe_count = %d, peer QP ID = %d) \n",
             id, wqe_count, cqe_count, scqe_count, pair->id);
}

LambdaInstruction DoublingQp::write(NetMem *local, bool require_cmpl) {
    ++wqe_count;
    ++this->pair->cqe_count;

    if (require_cmpl) {
        if (has_scq) {
            ++scqe_count;
        } else {
            ++cqe_count;
        }
    }

    LambdaInstruction lambda = [this, local, require_cmpl]() {
        this->qp->write(local, this->remote, require_cmpl);
    };
    return lambda;
}

LambdaInstruction DoublingQp::write(NetMem *local, NetMem *remote,
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

LambdaInstruction DoublingQp::reduce_write(
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
