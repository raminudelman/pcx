// TODO: Add license 
#pragma once

#include "dv_mgr.h"

#include <functional>
#include <queue>
#include <vector>

// CORE-Direct (CD) status 
enum cd_statuses { 
  PCOLL_SUCCESS = 0, 
  PCOLL_ERROR = 1 
};

typedef std::function<void()> LambdaInstruction;
typedef std::queue<LambdaInstruction> InsQueue;

class PcxQp;
class ManagementQp;

typedef std::vector<PcxQp *> GraphQps;
typedef GraphQps::iterator GraphQpsIt;

// Communication Graph class.
// Holds all the communication graph of a collective operation.
// The Communication Graph holds a management QP which acts as a 
// management unit which in charge of executing all the operation that
// where defined in advance. 
class CommGraph {
public:
  CommGraph(VerbCtx *vctx);
  ~CommGraph();

  void enqueue(LambdaInstruction &ins);

  // Register a QP to the graph. 
  void regQp(PcxQp *qp);
  
  void wait(PcxQp *slave_qp);
  void wait_send(PcxQp *slave_qp);

  void db();

  void finish();

  ManagementQp *mqp; // mqp stands for "Managment Queue Pair"

  VerbCtx *ctx;

  // Instructions queue
  InsQueue iq;
  
  GraphQps qps;
  uint16_t qp_cnt;
};

class PcxQp {
public:
  PcxQp(CommGraph *cgraph);
  virtual ~PcxQp() = 0;
  
  virtual void init() = 0;
  void fin();

  void send_credit();

  // Sends the local memory to remote memory using RDMA write
  void write(NetMem *local, NetMem *remote, bool require_cmpl);

  // Performs reduce operation on the local memory and sends the result data
  // to remote memory using RDMA write. 
  // In case CQE is needed, the argument require_cmpl should be set to 'true'
  void reduce_write(NetMem *local, NetMem *remote, uint16_t num_vectors,
                    uint8_t op, uint8_t type, bool require_cmpl);

  void poll();

  void db(uint32_t k = 0);

  void print();

  // Holds how many WQEs will be executed during a single collective operation 
  int wqe_count;

  // Holds how many CQE are expected to be generated during a single collective
  // operation 
  int cqe_count;

  int scqe_count;
  int recv_enables;

  uint16_t id;
  qp_ctx *qp;

  void set_pair(PcxQp *pair_) { 
    this->pair = pair_; 
  };

protected:
  CommGraph *graph;

  struct ibv_qp *ibqp;

  // Completion Queue. Initialized during init()
  // Used as the Completion Queue of both Receive Queue and Send Queue of the
  // QP unless a dedicated Send Completion Queue is specified for the Send
  // Queue of the QP. In case a Send Completion Queue, this Completion Queue
  // is used only for the Receive Queue of the QP.
  struct ibv_cq *ibcq;

  // Completion Queue for Send Queue.
  struct ibv_cq *ibscq;

  PcxQp *pair; // TODO: Change this name to "peer"

  bool initiated;
  bool has_scq;
  VerbCtx *ctx;

  struct ibv_qp *rc_qp_create(struct ibv_cq *cq, VerbCtx *verb_ctx,
                              uint16_t send_wq_size, uint16_t recv_rq_size,
                              struct ibv_cq *s_cq = NULL, int slaveRecv = 1,
                              int slaveSend = 1);

  struct ibv_cq *cd_create_cq(VerbCtx *verb_ctx, int cqe, 
                              void *cq_context = NULL,
                              struct ibv_comp_channel *channel = NULL,
                              int comp_vector = 0);

};

class ManagementQp : public PcxQp {
public:
  void cd_send_enable(PcxQp *slave_qp);
  void cd_recv_enable(PcxQp *slave_qp);
  void cd_wait(PcxQp *slave_qp);
  void cd_wait_send(PcxQp *slave_qp);

  ManagementQp(CommGraph *cgraph);
  ~ManagementQp();
  void init();

  LambdaInstruction stack;
  uint16_t last_qp;
  bool has_stack;

private:
  struct ibv_qp *create_management_qp(struct ibv_cq *cq, VerbCtx *verb_ctx,
                                      uint16_t send_wq_size);
};

class LoopbackQp : public PcxQp {
public:
  LoopbackQp(CommGraph *cgraph);
  ~LoopbackQp();
  void init();
};

typedef int (*p2p_exchange_func)(void *, volatile void *, volatile void *,
                                 size_t, uint32_t, uint32_t);
typedef std::function<void(volatile void *, volatile void *, size_t)> LambdaExchange;


class DoublingQp : public PcxQp { // TODO: Move to new file pcx_doubling.h
public:
  DoublingQp(CommGraph *cgraph, p2p_exchange_func func, void *comm, uint32_t peer, uint32_t tag, NetMem *incomingBuffer);
  ~DoublingQp();
  void init();
  void write(NetMem *local, bool require_cmpl);

  LambdaExchange exchange;
  LambdaExchange barrier;

protected:
  RemoteMem *remote;
  NetMem *incoming;
};

class RcQp : public PcxQp { // TODO: This is used only for RingQp.. maybe need to delete this class and use only RingQp which will inherit PcxQp directly.
public:
  RcQp(CommGraph *cgraph, PipeMem *incomingBuffer)
      : PcxQp(cgraph), incoming(incomingBuffer) {};
  virtual ~RcQp();

  void write(NetMem *local, size_t pos = 0, bool require_cmpl = false);
  void reduce_write(NetMem *local, size_t pos, uint16_t num_vectors, uint8_t op,
                    uint8_t type, bool require_cmpl);

protected:
  PipeMem *remote;
  PipeMem *incoming;
};

class RingQp : public RcQp { // TODO: Move to new file pcx_ring.h
public:
  RingQp(CommGraph *cgraph, p2p_exchange_func func, void *comm, uint32_t peer,
         uint32_t tag, PipeMem *incomingBuffer);

  void init();
  ~RingQp();
  LambdaExchange exchange;
  LambdaExchange barrier;
};

class RingPair { // TODO: Move to new file pcx_ring.h
public:
  RingPair(CommGraph *cgraph, p2p_exchange_func func, void *comm,
           uint32_t myRank, uint32_t commSize, uint32_t tag1, uint32_t tag2,
           PipeMem *incoming);
  ~RingPair();

  RingQp *right;
  RingQp *left;
};
