/*
 * Copyright (c) 2019-present, Mellanox Technologies Ltd. 
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include "pcx_qps.h"

typedef int (*p2p_exchange_func)(void *, volatile void *, volatile void *,
                                 size_t, uint32_t, uint32_t);

class DoublingQp : public TransportQp {
  public:
    DoublingQp(VerbCtx *ctx, p2p_exchange_func func, void *comm, uint32_t peer,
               uint32_t tag, NetMem *incomingBuffer);
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
    LambdaInstruction reduce_write(NetMem *local, NetMem *remote,
                                   uint16_t num_vectors, uint8_t op,
                                   uint8_t type, bool require_cmpl);

  protected:
    RemoteMem *remote;
    NetMem *incoming;

  private:
    uint32_t tag_;
    void *comm_;
};
