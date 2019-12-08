/*
 * Copyright (c) 2019-present, Mellanox Technologies Ltd. 
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// Used for registering the UMR
#include "pcx_verbs_ctx.h"

// Used for ibv_mr, ibv_sge, etc.
#include <infiniband/verbs.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>

enum PCX_MEMORY_TYPE {
    PCX_MEMORY_TYPE_HOST,
    PCX_MEMORY_TYPE_MEMIC,
    PCX_MEMORY_TYPE_REMOTE,
    PCX_MEMORY_TYPE_NIM,
    PCX_MEMORY_TYPE_USER,
};

// Network Memory
class NetMem { // TODO: Change class name to "PcxMem" or "PcxBaseMem"
  public:
    NetMem(){};
    virtual ~NetMem() = 0;
    struct ibv_sge *sg() {
        return &sge;
    };
    struct ibv_mr *getMr() {
        return mr;
    };

  protected:
    // Scatter-Gather Element
    struct ibv_sge sge;

    // Memory Region
    struct ibv_mr *mr;
};

// Host Memory
class HostMem : public NetMem {
  public:
    HostMem(size_t length, VerbCtx *ctx);
    ~HostMem();

  private:
    void *buf;
};

class Memic : public NetMem {
    /*
     * This is ConnectX-5 device memory mapped to the host memory.
     * Using this memory is about 200ns faster than using host memory.
     * So it should reduce latency in around 0.2us per step.
     */
  public:
    Memic(size_t length, VerbCtx *ctx);
    ~Memic();

  private:
    PcxDeviceMemory pcx_dm;
};

class UsrMem : public NetMem {
  public:
    UsrMem(void *buf, size_t length, VerbCtx *ctx);
    ~UsrMem();
};

class RefMem : public NetMem {
  public:
    RefMem(NetMem *mem, uint64_t byte_offset, uint32_t length);
    RefMem(const RefMem &srcRef);
    ~RefMem();
};

class UmrMem : public NetMem {
  public:
    UmrMem(std::vector<NetMem *> &mem_reg, VerbCtx *ctx);
    ~UmrMem();
};

class RemoteMem : public NetMem {
  public:
    RemoteMem(uint64_t addr, uint32_t rkey);
    ~RemoteMem();
};

class PipeMem {
  public:
    PipeMem(size_t length_, size_t depth_, VerbCtx *ctx,
            int mem_type_ = PCX_MEMORY_TYPE_HOST);
    PipeMem(size_t length_, size_t depth_, RemoteMem *remote);
    PipeMem(void *buf, size_t length_, size_t depth_, VerbCtx *ctx);
    ~PipeMem();
    RefMem operator[](size_t idx);

    RefMem next();
    void print();

    size_t getLength() { return length; };
    size_t getDepth() { return depth; };

  private:
    NetMem *mem;
    size_t length;
    size_t depth;
    int mem_type;
    size_t cur;
};
