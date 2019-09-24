// TODO: Add license 
#pragma once

#include "dv_mgr.h"

// Used because every QP has a pointer to a verbs context
#include "verbs_ctx.h"

// Used for creating RC QPs that support CORE-Direct.
// Note: Search for "ibv_exp" in the pcx_mem.cc file.
#include <infiniband/verbs_exp.h>

#include <functional>
#include <queue>
#include <vector>

#ifdef QP_DEBUG
#define QP_PRINT(args...) fprintf(stderr, "(%s: %d) in function %s: " \
                       ,__FILE__,__LINE__,__func__); fprintf(stderr, args)
#else
#define QP_PRINT(args...)
#endif

// CORE-Direct (CD) status 
enum cd_statuses { 
  PCOLL_SUCCESS = 0, 
  PCOLL_ERROR = 1 
};

PCX_ERROR(QPCreateFailed);
PCX_ERROR(QPInitFailed);
PCX_ERROR(QpFailedRTR);
PCX_ERROR(QpFailedRTS);
PCX_ERROR(CQCreateFailed);
PCX_ERROR(CQModifyFailed);
PCX_ERROR(MissingContext);

typedef std::function<void()> LambdaInstruction;
typedef std::queue<LambdaInstruction> InsQueue;

class GraphObj {
public:
  // Holds a unique number for the Obj.
  // Every Obj within the CommmGraph has a unique number 
  // which is given during CommmGraph::reqQp.
  uint16_t id;
  virtual void init() = 0;
  virtual void fin() = 0;
};

class PcxQp : public GraphObj {
public:
  PcxQp(VerbCtx *ctx);
  virtual ~PcxQp() = 0;
  
  virtual void init() = 0;
  void fin();

  void poll();

  void db();

  void print();

  // Holds how many WQEs will be executed during a single collective operation 
  int wqe_count;

  // The number of WQEs that are expected to be executed 
  // during a single collective operation
  int cqe_count; // Required for all QP types (both transport and non transport)

  int recv_enables;

  qp_ctx *qp;

  VerbCtx *ctx;
protected:

  struct ibv_qp *ibqp;

  // Completion Queue. Initialized during init()
  // Used as the Completion Queue of both Receive Queue and Send Queue of the
  // QP unless a dedicated Send Completion Queue is specified for the Send
  // Queue of the QP. In case a Send Completion Queue, this Completion Queue
  // is used only for the Receive Queue of the QP.
  struct ibv_cq *ibcq;



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
  TransportQp(VerbCtx *ctx);
  virtual ~TransportQp() = 0;
  
  virtual void init() = 0;

  int scqe_count;

  LambdaInstruction send_credit();

  // Enable to change the peer of the QP
  void set_pair(PcxQp *pair_); // TODO: Change the name to peer
  const PcxQp* get_pair();

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
  ManagementQp(VerbCtx *ctx);
  ~ManagementQp();
  void init();

  LambdaInstruction cd_send_enable(PcxQp *slave_qp);
  LambdaInstruction cd_recv_enable(PcxQp *slave_qp);
  LambdaInstruction cd_wait(PcxQp *slave_qp, bool wait_scq = false);
 
  LambdaInstruction stack; // TODO: Check if used. If not used remove!
  uint16_t last_qp; // TODO: Check if used. If not used remove!
  bool has_stack; // TODO: Check if used. If not used remove!

private:
  struct ibv_qp *create_management_qp(struct ibv_cq *cq, VerbCtx *verb_ctx,
                                      uint16_t send_wq_size);
};

class LoopbackQp : public TransportQp {
public:
  LoopbackQp(VerbCtx *ctx);
  ~LoopbackQp();
  void init();

  LambdaInstruction write(NetMem *local, RefMem *remote, bool require_cmpl);
  LambdaInstruction write(NetMem *local, NetMem *remote, bool require_cmpl);

  LambdaInstruction reduce_write(UmrMem *local, NetMem *remote, uint16_t num_vectors,
                    uint8_t op, uint8_t type, bool require_cmpl);
  LambdaInstruction reduce_write(NetMem *local, NetMem *remote, uint16_t num_vectors,
                    uint8_t op, uint8_t type, bool require_cmpl);
};

class DoublingQp : public TransportQp { // TODO: Move to new file pcx_doubling.h
public:
  DoublingQp(VerbCtx *ctx, p2p_exchange_func func, void *comm, uint32_t peer, uint32_t tag, NetMem *incomingBuffer);
  ~DoublingQp();

  void init();
  LambdaInstruction write(NetMem *local, bool require_cmpl);

  // Sends the local memory to remote memory using RDMA write
  // where the receiving side will *always* get a CQE.
  // The sending side will receive CQE iff require_cmpl == true.
  LambdaInstruction write(NetMem *local, NetMem *remote, bool require_cmpl);

  // Performs reduce operation on the local memory and sends the result data
  // to remote memory using RDMA write. 
  // In case CQE is needed, the argument require_cmpl should be set to 'true'
  LambdaInstruction reduce_write(NetMem *local, NetMem *remote, uint16_t num_vectors,
                    uint8_t op, uint8_t type, bool require_cmpl);


protected:
  RemoteMem *remote;
  NetMem *incoming;
};
