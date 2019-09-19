#include "pcx_ring_qp.h"

RingQp::RingQp(VerbCtx *ctx, p2p_exchange_func func, void *comm,
               uint32_t peer, uint32_t tag, PipeMem *incomingBuffer)
    : TransportQp(ctx), incoming(incomingBuffer) {
  this->has_scq = true;
  using namespace std::placeholders;

  exchange = std::bind(func, comm, _1, _2, _3, peer, tag);
  barrier = std::bind(func, comm, _1, _2, _3, peer, tag + ((unsigned int) 0xf000) );
}

RingQp::~RingQp() {
  delete (qp);
  ibv_destroy_qp(ibqp);
  ibv_destroy_cq(ibcq);
}

void RingQp::init() {

  ibcq = cd_create_cq(ctx, cqe_count);
  if (!ibcq) {
    PERR(CQCreateFailed);
  }

  ibscq = cd_create_cq(ctx, scqe_count);
  if (!ibscq) {
    PERR(CQCreateFailed);
  }

  ibqp = rc_qp_create(ibcq, ctx, wqe_count, cqe_count, ibscq);

  if (!ibqp) {
    PERR(QPCreateFailed);
  }

  rd_peer_info_t local_info, remote_info;

  local_info.buf = (uintptr_t)(*incoming)[0].sg()->addr;
  local_info.rkey = (*incoming)[0].getMr()->rkey;
  rc_qp_get_addr(ibqp, &local_info.addr);

  //  fprintf(stderr,"sent: buf = %lu, rkey = %u, qpn = %lu, lid = %u , gid =
  //  %u, psn = %ld\n", local_info.buf,
  //                local_info.rkey, local_info.addr.qpn,local_info.addr.lid
  //                ,local_info.addr.gid ,local_info.addr.psn);

  ctx->mtx.unlock();
  exchange((void *)&local_info, (void *)&remote_info, sizeof(local_info));
  ctx->mtx.lock();

  //  fprintf(stderr,"recv: buf = %lu, rkey = %u, qpn = %lu, lid = %u , gid =
  //  %u, psn = %ld\n", remote_info.buf, remote_info.rkey, remote_info.addr.qpn
  //  ,
  //                                                        remote_info.addr.lid,
  //                                                        remote_info.addr.gid,
  //                                                        remote_info.addr.psn
  //                                                        );

  RemoteMem tmp_remote(remote_info.buf, remote_info.rkey);
  remote =
      new PipeMem(incoming->getLength(), incoming->getDepth(), &tmp_remote);

  rc_qp_connect(&remote_info.addr, ibqp);


  //barrier

  int ack;
  ctx->mtx.unlock();
  barrier((void*) &ack, (void*) &ack, sizeof(int));
  ctx->mtx.lock();

  qp = new qp_ctx(ibqp, ibcq, wqe_count, cqe_count, ibscq, scqe_count);

  if (this->pair->qp) {
    this->pair->qp->set_pair(this->qp);
    this->qp->set_pair(this->pair->qp);
  }

  QP_PRINT("RingQP initiated (ID = %d, wqe_count = %d, cqe_count = %d, scqe_count = %d, peer QP ID = %d) \n", id, wqe_count, cqe_count, scqe_count, pair->id);
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

  // TODO: Consider calling here the PcxQp::write(NetMem *local, RefMem *remote, bool require_cmpl); (Overload if this function does not exist)
  struct ibv_sge lsg = (*local->sg());

  LambdaInstruction lambda = [this, lsg, pos,require_cmpl]() {
    this->qp->write(&lsg, (*this->remote)[pos].sg(), require_cmpl);
  };

  return lambda;
}

LambdaInstruction RingQp::reduce_write(NetMem *local, size_t pos, uint16_t num_vectors,
                        uint8_t op, uint8_t type, bool require_cmpl) {
  wqe_count += 2;
  ++this->pair->cqe_count;

  if (require_cmpl) {
    if (has_scq) {
      ++scqe_count;
    } else {
      ++cqe_count;
    }
  }

  // TODO: Consider using PcxQp::reduce_write(NetMem *local, RefMem *remote, uint16_t num_vectors, uint8_t op, uint8_t type, bool require_cmpl). Added this function in case it does not exist

  LambdaInstruction lambda = [this, local, num_vectors, op, type, pos, require_cmpl]() {
    RefMem ref = ((*this->remote)[pos]);
    this->qp->reduce_write(local, &ref, num_vectors, op, type, require_cmpl);
  };
  return lambda;
}

void RingQps::init(){
    fprintf(stderr, "ringqps init\n");
}

void RingQps::fin(){
    fprintf(stderr, "ringqps fin\n");
}