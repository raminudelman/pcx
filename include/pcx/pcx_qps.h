/*
 * Copyright (c) 2019-present, Mellanox Technologies Ltd. 
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// Used because every QP has a qp_ctx member
#include "pcx_dv_mgr.h"

// Used because every QP has a pointer to a verbs context
#include "pcx_verbs_ctx.h"

#include <functional>
#include <queue>
#include <vector>

#ifdef QP_DEBUG
#define QP_PRINT(args...)                                                      \
    fprintf(stderr, "(%s: %d) in function %s: ", __FILE__, __LINE__,           \
            __func__);                                                         \
    fprintf(stderr, args)
#else
#define QP_PRINT(args...)
#endif

PCX_ERROR(QPCreateFailed);
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
    // This number is in WQEBBs (WQE Basic Block) units.
    // Note: WQEBB size is 64 Bytes.
    int wqe_count;

    // The number of WQEs that are expected to be compeleted
    // during a single collective operation
    int cqe_count; // Required for all QP types (both transport and non
                   // transport)

    int recv_enables;

    qp_ctx *qp;

    VerbCtx *ctx_;

  protected:
    struct ibv_qp *ibqp;

    // Completion Queue. Initialized during init()
    // Used as the Completion Queue of both Receive Queue and Send Queue of the
    // QP unless a dedicated Send Completion Queue is specified for the Send
    // Queue of the QP. In case a Send Completion Queue, this Completion Queue
    // is used only for the Receive Queue of the QP.
    struct ibv_cq *ibcq;
};

typedef std::function<void(volatile void *, volatile void *, size_t)>
    LambdaExchange;

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
    const PcxQp *get_pair();

    LambdaExchange exchange;
    LambdaExchange barrier;

  protected:
    PcxQp *pair; // TODO: Change this name to "peer"

    bool has_scq;

    // Completion Queue for Send Queue.
    struct ibv_cq *ibscq;
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
    uint16_t last_qp;        // TODO: Check if used. If not used remove!
    bool has_stack;          // TODO: Check if used. If not used remove!
};

class LoopbackQp : public TransportQp {
  public:
    LoopbackQp(VerbCtx *ctx);
    ~LoopbackQp();
    void init();

    LambdaInstruction write(NetMem *local, RefMem *remote, bool require_cmpl);
    LambdaInstruction write(NetMem *local, NetMem *remote, bool require_cmpl);

    LambdaInstruction reduce_write(UmrMem *local, NetMem *remote,
                                   uint16_t num_vectors, uint8_t op,
                                   uint8_t type, bool require_cmpl);
    LambdaInstruction reduce_write(NetMem *local, NetMem *remote,
                                   uint16_t num_vectors, uint8_t op,
                                   uint8_t type, bool require_cmpl);
};
