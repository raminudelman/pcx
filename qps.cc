
// TODO: Add license
#include "qps.h"

PCX_ERROR(QPCreateFailed);
PCX_ERROR(QPInitFailed);
PCX_ERROR(QpFailedRTR);
PCX_ERROR(QpFailedRTS);
PCX_ERROR(CQCreateFailed);
PCX_ERROR(CQModifyFailed);
PCX_ERROR(MissingContext);

CommGraph::CommGraph(VerbCtx *vctx) : ctx(vctx), mqp(NULL), iq(), qp_cnt(0) {
  ctx->mtx.lock();
  mqp = new ManagementQp(this);
}

CommGraph::~CommGraph() { 
  delete (mqp); 
}

void CommGraph::regQp(PcxQp *qp) {
  qps.push_back(qp);
  qp->id = qp_cnt;
  ++qp_cnt;
}

void CommGraph::enqueue(LambdaInstruction &ins) { 
  iq.push(std::move(ins)); 
}

void CommGraph::wait(PcxQp *slave_qp, bool wait_scq) { 
  mqp->cd_wait(slave_qp, wait_scq); 
}

void CommGraph::db() { 
  mqp->qp->db(); 
}

void CommGraph::finish() {
  for (GraphQpsIt it = qps.begin(); it != qps.end(); ++it) {
    (*it)->init();
  }
  PRINT("Starting to write WQEs");
  while (!iq.empty()) {
    LambdaInstruction &instruction = iq.front();
    instruction();
    iq.pop();
  }
  PRINT("Finalizing");
  for (GraphQpsIt it = qps.begin(); it != qps.end(); ++it) {
    (*it)->fin();
  }
  mqp->db(mqp->recv_enables);
  PRINT("READY!");
  ctx->mtx.unlock();
}

PcxQp::PcxQp(CommGraph *cgraph)
    : wqe_count(0), cqe_count(0), recv_enables(0),
      graph(cgraph), ctx(cgraph->ctx), qp(NULL), ibqp(NULL), ibcq(NULL) {
  cgraph->regQp(this);
}

PcxQp::~PcxQp() {
}

void PcxQp::db(uint32_t k) { 
  this->qp->db(k); 
}

void PcxQp::poll() { 
  this->qp->poll(); 
}

void PcxQp::print() {
  this->qp->print_sq();
  this->qp->print_cq();
}

void PcxQp::fin() { 
  this->qp->fin(); 
}

struct ibv_cq *PcxQp::cd_create_cq(VerbCtx *verb_ctx, int cqe, void *cq_context,
                            struct ibv_comp_channel *channel, int comp_vector) {
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

TransportQp::TransportQp(CommGraph *cgraph)
    : PcxQp(cgraph), scqe_count(0), ibscq(NULL), pair(this) {}

TransportQp::~TransportQp() {
}

void TransportQp::init() {
}

void TransportQp::send_credit() {
  ++wqe_count;
  ++pair->cqe_count;
  LambdaInstruction lambda = [this]() { qp->send_credit(); };
  graph->enqueue(lambda);
  graph->mqp->cd_send_enable(this);
}

void TransportQp::write(NetMem *local, NetMem *remote, bool require_cmpl) {
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

  this->graph->enqueue(lambda);
  graph->mqp->cd_send_enable(this);
}

void TransportQp::reduce_write(NetMem *local, NetMem *remote, uint16_t num_vectors, // TODO: Does someone use this function?
                         uint8_t op, uint8_t type, bool require_cmpl) {
  wqe_count += 2;
  ++this->pair->cqe_count;
  LambdaInstruction lambda = [this, local, remote, num_vectors, op, type, require_cmpl]() {
    this->qp->reduce_write(local, remote, num_vectors, op, type, require_cmpl);
  };
  this->graph->enqueue(lambda);
  graph->mqp->cd_send_enable(this);
}

void TransportQp::set_pair(PcxQp *pair_) { 
    this->pair = pair_; 
};

struct ibv_qp *TransportQp::rc_qp_create(struct ibv_cq *cq, VerbCtx *verb_ctx,
                            uint16_t send_wq_size, uint16_t recv_rq_size,
                            struct ibv_cq *s_cq, int slaveRecv, int slaveSend) {
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
  init_attr.comp_mask = IBV_QP_INIT_ATTR_PD | IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS;
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


ManagementQp::ManagementQp(CommGraph *cgraph)
    : PcxQp(cgraph), last_qp(0), has_stack(false) {}

ManagementQp::~ManagementQp() {
  delete (qp);
  ibv_destroy_qp(ibqp);
  ibv_destroy_cq(ibcq);
}

void ManagementQp::init() {

  if (!ctx) {
    PERR(MissingContext);
  }

  this->ibcq = cd_create_cq(graph->ctx, cqe_count);
  if (!this->ibcq) {
    PERR(CQCreateFailed);
  }

  this->ibqp = create_management_qp(this->ibcq, ctx, wqe_count);
  this->qp = new qp_ctx(this->ibqp, this->ibcq, wqe_count, cqe_count);

  PRINT("ManagementQP initiated");
}

void ManagementQp::cd_send_enable(PcxQp *slave_qp) {
  ++wqe_count;
  LambdaInstruction lambda = [this, slave_qp]() {
    this->qp->cd_send_enable(slave_qp->qp);
  };
  this->graph->enqueue(lambda);
}

void ManagementQp::cd_recv_enable(
    PcxQp *slave_qp) { // call from PcxQp constructor
  ++wqe_count;
  LambdaInstruction lambda = [this, slave_qp]() {
    this->qp->cd_recv_enable(slave_qp->qp);
  };
  this->graph->enqueue(lambda);
  ++recv_enables;
}

void ManagementQp::cd_wait(PcxQp *slave_qp, bool wait_scq) {
  ++wqe_count;
  LambdaInstruction lambda = [this, slave_qp, wait_scq]() {
    this->qp->cd_wait(slave_qp->qp, wait_scq);
  };
  this->graph->enqueue(lambda);
}

struct ibv_qp *ManagementQp::create_management_qp(struct ibv_cq *cq, VerbCtx *verb_ctx,
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
  init_attr.comp_mask = IBV_QP_INIT_ATTR_PD | IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS;
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

    rc = ibv_modify_qp(_mq, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                                       IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                                       IBV_QP_MAX_DEST_RD_ATOMIC |
                                       IBV_QP_MIN_RNR_TIMER);

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
                                       IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
    if (rc) {
      rc = PCOLL_ERROR;
    }
  }
  return _mq;
}

LoopbackQp::LoopbackQp(CommGraph *cgraph) : TransportQp(cgraph) {
  this->has_scq = false;
  cgraph->mqp->cd_recv_enable(this);
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

  PRINT("Loopback RC QP initiated");
}

DoublingQp::DoublingQp(CommGraph *cgraph, p2p_exchange_func func, void *comm,
                       uint32_t peer, uint32_t tag, NetMem *incomingBuffer)
    : TransportQp(cgraph), incoming(incomingBuffer) {
  this->has_scq = true;
  cgraph->mqp->cd_recv_enable(this);
  using namespace std::placeholders;

  exchange = std::bind(func, comm, _1, _2, _3, peer, tag);
  barrier = std::bind(func, comm, _1, _2, _3, peer, tag + ((unsigned int) 0xf000) );
}

DoublingQp::~DoublingQp() {
  delete (remote);
  delete (qp);
  ibv_destroy_qp(ibqp);
  ibv_destroy_cq(ibcq);
}

void DoublingQp::init() {

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
  
  local_info.buf = (uintptr_t)incoming->sg()->addr;
  local_info.rkey = incoming->getMr()->rkey;
  rc_qp_get_addr(ibqp, &local_info.addr);

  ctx->mtx.unlock();
  exchange((void *)&local_info, (void *)&remote_info, sizeof(local_info));
  ctx->mtx.lock();

  /*
    fprintf(stderr,"sent: buf = %lu, rkey = %u, qpn = %lu, lid = %u , gid = %u,
    psn = %ld\n", local_info.buf,
                  local_info.rkey,
    local_info.addr.qpn,local_info.addr.lid,local_info.addr.gid,local_info.addr.psn
    );
    fprintf(stderr,"recv: buf = %lu, rkey = %u, qpn = %lu, lid = %u , gid = %u,
    psn = %ld\n", remote_info.buf, remote_info.rkey, remote_info.addr.qpn ,
                                                          remote_info.addr.lid,
    remote_info.addr.gid, remote_info.addr.psn );

  */

  remote = new RemoteMem(remote_info.buf, remote_info.rkey);
  rc_qp_connect(&remote_info.addr, ibqp);

  //barrier
  int ack;
  ctx->mtx.unlock();
  barrier((void*) &ack, (void*) &ack, sizeof(int));
  ctx->mtx.lock();

  qp = new qp_ctx(ibqp, ibcq, wqe_count, cqe_count, ibscq, scqe_count);

  PRINT("DoublingQP initiated");
}

void DoublingQp::write(NetMem *local, bool require_cmpl) {
  ++wqe_count;
  ++this->pair->cqe_count;

  LambdaInstruction lambda = [this, local, require_cmpl]() {
    this->qp->write(local, this->remote, require_cmpl);
  };

  this->graph->enqueue(lambda);
  graph->mqp->cd_send_enable(this);
}

RingQp::RingQp(CommGraph *cgraph, p2p_exchange_func func, void *comm,
               uint32_t peer, uint32_t tag, PipeMem *incomingBuffer)
    : TransportQp(cgraph), incoming(incomingBuffer) {
  this->has_scq = true;
  cgraph->mqp->cd_recv_enable(this);
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

  PRINT("RingQP initiated");
}

void RingQp::write(NetMem *local, size_t pos, bool require_cmpl) {
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

  this->graph->enqueue(lambda);
  graph->mqp->cd_send_enable(this);
}

void RingQp::reduce_write(NetMem *local, size_t pos, uint16_t num_vectors,
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
  this->graph->enqueue(lambda);
  graph->mqp->cd_send_enable(this);
}

RingPair::RingPair(CommGraph *cgraph, p2p_exchange_func func, void *comm, // TODO: Move to some "ring algorithms qps" file
                   uint32_t myRank, uint32_t commSize, uint32_t tag1,
                   uint32_t tag2, PipeMem *incoming) {
  uint32_t rightRank = (myRank + 1) % commSize;
  uint32_t leftRank = (myRank - 1 + commSize) % commSize;

  if (myRank % 2) { // Odd rank
    this->right = new RingQp(cgraph, func, comm, rightRank, tag1, incoming);
    this->left = new RingQp(cgraph, func, comm, leftRank, tag2, incoming);
  } else { // Even rank
    this->left = new RingQp(cgraph, func, comm, leftRank, tag1, incoming);
    this->right = new RingQp(cgraph, func, comm, rightRank, tag2, incoming);
  }
  right->set_pair(left);
  left->set_pair(right);
}

RingPair::~RingPair() {
  delete (right);
  delete (left);
}
