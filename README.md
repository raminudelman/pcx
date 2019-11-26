# Persistent Collectives X (PCX)

Persistent Collectives X (PCX) is a collective communication library aimed at providing high performance, low latency persistent collectives over RDMA devices, while minimizing CPU/GPU utilization to near-zero. 

> **Note**: PCX is currently in a PoC stage.

## Introduction

PCX is a communication library that provides standard collective communication operations for CPUs and GPUs using RDMA over InfiniBand Verbs. PCX supports communication for a single multi-threaded process or multi-process applications that run within a single node or distributed across an arbitrary number of nodes.

PCX targeted at accelerating deep learning distributed training and any other HPC application that requires high performant communication.

## Supported Collective Operations

Currently PCX supports only all-reduce operation. Other operations like all-gather, reduce, broadcast, and reduce-scatter will be supported in the future. 

### All-Reduce

There are two algorithms (Ring and Recursive Doubling) implementing the all-reduce collective operation and user can choose either one of them for the all-reduce collective operations. 

## How It's Done

PCX takes advantage of the persistent attributes of the communication and uses several different technologies including: RDMA, CORE-Direct, NIC calculation capabilities, GPU-Direct RDMA (also known as PeerDirect) and network device memory (when available), in combination with (almost) complete software bypass for the data-path.

## API

PCX provides an easy high-level, C++ based interface for using built-in algorithm for the collective operations.

In addition, PCX can be used as a low-level library for one who wants to implement some custom collective operation algorithms using low-level "building blocks" that PCX exposes. 

## Requirements

### HW Requirements

PCX requires Mellanox's ConnectX-5 (or later) NICs.

### SW Requirements

The basic requirements for PCX are:
* MLNX_OFED 4.5

In order to take full advantage of the PCX library, PCX requires:
* GPU-Direct RDMA (In case the data for the collective operation resides in the GPU)

> Note: There is work in progress to remove the dependency on MLNX_OFED 4.5 and use only [rdma-core](https://github.com/linux-rdma/rdma-core).

## Install

<TBD>

## Tests

<TBD>

## Usage Examples

### High Level API

The example shows how [Gloo](https://github.com/facebookincubator/gloo) can use PCX for all-reduce operation (using the Ring algorithm):

```c++
#include "third-party/pcx/pcx_allreduce_alg_chunked_ring.h"

namespace gloo {

template <typename T>
class PcxAllreduceRing : public Algorithm {
public:
  PcxAllreduceRing(
      const std::shared_ptr<Context> &context,
      const std::vector<T *> &ptrs,
      const int count,
      const ReductionFunction<T> *fn = ReductionFunction<T>::sum)
      : Algorithm(context),
        pcxAlg(contextSize_, 
               contextRank_, 
               ptrs, 
               count, 
               this->context_->nextSlot(), 
               this->context_->nextSlot(), 
               (void *)&(this->context_)) {}

  void run() {
    pcxAlg.run();
  }


private:
  PcxAllreduceChunkedRing<T> pcxAlg;
};

} // namespace gloo

```

### Low Level API

Examples for implementation of collectives with low-level API of PCX can be seen here:

* [Recursive Doubling](pcx_allreduce_alg_king.h)
* [Ring](pcx_allreduce_alg_chunked_ring.h)

## Copyright

<TBD>
