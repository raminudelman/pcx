#pragma once
#include "qps.h"

class RingQp : public TransportQp
{ // TODO: Move to new file pcx_ring.h
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

enum {RingQpLeft, RingQpRight};

class RingQps : public GraphObj
{
private:
    RingQp *left;
    RingQp *right;

public:
    RingQps(p2p_exchange_func func, void *comm,
             uint32_t myRank, uint32_t commSize, uint32_t tag1,
             uint32_t tag2, PipeMem *incoming, VerbCtx *ctx) {
      uint32_t rightRank = (myRank + 1) % commSize;
      uint32_t leftRank = (myRank - 1 + commSize) % commSize;

      if (myRank % 2) { // Odd rank
        this->right = new RingQp(ctx, func, comm, rightRank, tag1, incoming);
//        cgraph->regQp(this->right);
        this->left = new RingQp(ctx, func, comm, leftRank, tag2, incoming);
//        cgraph->regQp(this->left);
      } else { // Even rank
        this->left = new RingQp(ctx, func, comm, leftRank, tag1, incoming);
//        cgraph->regQp(this->left);
        this->right = new RingQp(ctx, func, comm, rightRank, tag2, incoming);
 //       cgraph->regQp(this->right);
      }
      right->set_pair(left);
      left->set_pair(right);
    }
    RingQp* getLeftQp(){
        return left;
    }
    RingQp* getRightQp(){
        return right;
    }

    ~RingQps() {
      delete (right);
      delete (left);
    }
    void init();
    void fin();
};

//   class RingPair { // TODO: Move to new file pcx_ring.h
//   public:
//     RingPair(CommGraph *cgraph, p2p_exchange_func func, void *comm, // TODO: Move to some "ring algorithms qps" file
//              uint32_t myRank, uint32_t commSize, uint32_t tag1,
//              uint32_t tag2, PipeMem *incoming, VerbCtx *ctx) {
//       uint32_t rightRank = (myRank + 1) % commSize;
//       uint32_t leftRank = (myRank - 1 + commSize) % commSize;

//       if (myRank % 2) { // Odd rank
//         this->right = new RingQp(ctx, func, comm, rightRank, tag1, incoming);
//         cgraph->regQp(this->right);
//         this->left = new RingQp(ctx, func, comm, leftRank, tag2, incoming);
//         cgraph->regQp(this->left);
//       } else { // Even rank
//         this->left = new RingQp(ctx, func, comm, leftRank, tag1, incoming);
//         cgraph->regQp(this->left);
//         this->right = new RingQp(ctx, func, comm, rightRank, tag2, incoming);
//         cgraph->regQp(this->right);
//       }
//       right->set_pair(left);
//       left->set_pair(right);
//     }

//     ~RingPair() {
//       delete (right);
//       delete (left);
//     }

//     RingQp *right;
//     RingQp *left;
//   };
