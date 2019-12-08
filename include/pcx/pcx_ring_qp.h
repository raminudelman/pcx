/*
 * Copyright (c) 2019-present, Mellanox Technologies Ltd. 
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include "pcx_qps.h"

class RingQp : public TransportQp {
  public:
    RingQp(VerbCtx *ctx, PipeMem *incomingBuffer);
    ~RingQp();

    void init_rc_qp();
    void init_qp_ctx();
    rd_peer_info_t get_local_info();
    void set_remote_info(rd_peer_info_t remote_info);

    // Must provide some implementation to pure virtual function.
    void init() {}

    LambdaInstruction write(NetMem *local, size_t pos = 0,
                            bool require_cmpl = false);
    LambdaInstruction reduce_write(NetMem *local, size_t pos,
                                   uint16_t num_vectors, uint8_t op,
                                   uint8_t type, bool require_cmpl);

  protected:
    PipeMem *remote;
    PipeMem *incoming;
};

typedef int (*ring_exchange_func)(void *, volatile void *, volatile void *,
                                  volatile void *, volatile void *, size_t,
                                  uint32_t, uint32_t, uint32_t, uint32_t);
class RingQps : public GraphObj {
  private:
    RingQp *left;
    RingQp *right;
    uint32_t rightRank;
    uint32_t leftRank;
    ring_exchange_func ring_exchange;
    uint32_t tag1;
    uint32_t tag2;
    void *comm;

  public:
    RingQps(ring_exchange_func func, void *_comm, uint32_t myRank,
            uint32_t commSize, uint32_t _tag1, uint32_t _tag2,
            PipeMem *incoming, VerbCtx *ctx)
        : tag1(_tag1), tag2(_tag2), comm(_comm) {
        leftRank = (myRank - 1 + commSize) % commSize;
        rightRank = (myRank + 1) % commSize;
        ring_exchange = func;

        this->right = new RingQp(ctx, incoming);
        this->left = new RingQp(ctx, incoming);
        right->set_pair(left);
        left->set_pair(right);
    }
    RingQp *getLeftQp() { return left; }
    RingQp *getRightQp() { return right; }

    ~RingQps() {
        delete (right);
        delete (left);
    }
    void init();
    void fin() {
        // TODO: check if order is important
        left->fin();
        right->fin();
    }
};
