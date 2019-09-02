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
  
  void wait(PcxQp *slave_qp, bool wait_scq = false);

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

  void poll();

  void db(uint32_t k = 0);

  void print();

  // Holds how many WQEs will be executed during a single collective operation 
  int wqe_count;

  // Holds how many CQE are expected to be generated during a single collective
  // operation 
  int cqe_count; // Required for all QP types (both transport and non transport)

  int recv_enables;

  // Holds a unique number for the QP.
  // Every QP within the CommmGraph has a unique number 
  // which is given during CommmGraph::reqQp.
  uint16_t id;

  qp_ctx *qp;

protected:
  CommGraph *graph;

  struct ibv_qp *ibqp;

  // Completion Queue. Initialized during init()
  // Used as the Completion Queue of both Receive Queue and Send Queue of the
  // QP unless a dedicated Send Completion Queue is specified for the Send
  // Queue of the QP. In case a Send Completion Queue, this Completion Queue
  // is used only for the Receive Queue of the QP.
  struct ibv_cq *ibcq;

  VerbCtx *ctx;

  struct ibv_cq *cd_create_cq(VerbCtx *verb_ctx, int cqe, 
                              void *cq_context = NULL,
                              struct ibv_comp_channel *channel = NULL,
                              int comp_vector = 0);

};

typedef int (*p2p_exchange_func)(void *, volatile void *, volatile void *,
                                 size_t, uint32_t, uint32_t);
typedef std::function<void(volatile void *, volatile void *, size_t)> LambdaExchange;

// Each QP which is of type TransportQp is a QP which has a tranport
// and it responsible for transferring data from one place to another.
class TransportQp : public PcxQp {
public:
  TransportQp(CommGraph *cgraph);
  virtual ~TransportQp() = 0;
  
  virtual void init() = 0;

  int scqe_count;

  void send_credit();

  // Sends the local memory to remote memory using RDMA write
  void write(NetMem *local, NetMem *remote, bool require_cmpl);

  // Performs reduce operation on the local memory and sends the result data
  // to remote memory using RDMA write. 
  // In case CQE is needed, the argument require_cmpl should be set to 'true'
  void reduce_write(NetMem *local, NetMem *remote, uint16_t num_vectors,
                    uint8_t op, uint8_t type, bool require_cmpl);

  // Enable to change the peer of the QP
  void set_pair(PcxQp *pair_); // TODO: Change the name to peer

  LambdaExchange exchange;
  LambdaExchange barrier;

protected:

  PcxQp *pair; // TODO: Change this name to "peer"

  bool has_scq;

  // Completion Queue for Send Queue.
  struct ibv_cq *ibscq;

  struct ibv_qp *rc_qp_create(struct ibv_cq *cq, VerbCtx *verb_ctx,
                              uint16_t send_wq_size, uint16_t recv_rq_size,
                              struct ibv_cq *s_cq = NULL, int slaveRecv = 1,
                              int slaveSend = 1);


};

class ManagementQp : public PcxQp {
public:
  void cd_send_enable(PcxQp *slave_qp);
  void cd_recv_enable(PcxQp *slave_qp);
  void cd_wait(PcxQp *slave_qp, bool wait_scq = false);

  ManagementQp(CommGraph *cgraph);
  ~ManagementQp();
  void init();

  LambdaInstruction stack; // TODO: Check if used. If not used remove!
  uint16_t last_qp; // TODO: Check if used. If not used remove!
  bool has_stack; // TODO: Check if used. If not used remove!

private:
  struct ibv_qp *create_management_qp(struct ibv_cq *cq, VerbCtx *verb_ctx,
                                      uint16_t send_wq_size);
};

class LoopbackQp : public TransportQp {
public:
  LoopbackQp(CommGraph *cgraph);
  ~LoopbackQp();
  void init();
};

class DoublingQp : public TransportQp { // TODO: Move to new file pcx_doubling.h
public:
  DoublingQp(CommGraph *cgraph, p2p_exchange_func func, void *comm, uint32_t peer, uint32_t tag, NetMem *incomingBuffer);
  ~DoublingQp();

  void init();
  void write(NetMem *local, bool require_cmpl);

protected:
  RemoteMem *remote;
  NetMem *incoming;
};

class RingQp : public TransportQp { // TODO: Move to new file pcx_ring.h
public:
  RingQp(CommGraph *cgraph, p2p_exchange_func func, void *comm, uint32_t peer,
         uint32_t tag, PipeMem *incomingBuffer);
  ~RingQp();

  void init();
  void write(NetMem *local, size_t pos = 0, bool require_cmpl = false);
  void reduce_write(NetMem *local, size_t pos, uint16_t num_vectors, uint8_t op,
                    uint8_t type, bool require_cmpl);

protected:
  PipeMem *remote;
  PipeMem *incoming;
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
