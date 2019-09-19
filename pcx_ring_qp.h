#pragma once
#include "qps.h"

class RingQp : public TransportQp { // TODO: Move to new file pcx_ring.h
public:
  RingQp(VerbCtx *ctx, p2p_exchange_func func, void *comm, uint32_t peer,
         uint32_t tag, PipeMem *incomingBuffer);
  ~RingQp();

  void init();
  LambdaInstruction write(NetMem *local, size_t pos = 0, bool require_cmpl = false);
  LambdaInstruction reduce_write(NetMem *local, size_t pos, uint16_t num_vectors, uint8_t op,
                    uint8_t type, bool require_cmpl);

protected:
  PipeMem *remote;
  PipeMem *incoming;
};

class RingQps : public GraphObj
{
private:
    TransportQp *left;
    TransportQp *right;
public:
    //   RingQp(VerbCtx *ctx, p2p_exchange_func func, void *comm, uint32_t peer,
    //          uint32_t tag, PipeMem *incomingBuffer);
    //   ~RingQp();

    void init();
    void fin();
    //   LambdaInstruction write(NetMem *local, size_t pos = 0, bool require_cmpl = false);
    //   LambdaInstruction reduce_write(NetMem *local, size_t pos, uint16_t num_vectors, uint8_t op,
    //                     uint8_t type, bool require_cmpl);

    // protected:
    //   PipeMem *remote;
    //   PipeMem *incoming;
};