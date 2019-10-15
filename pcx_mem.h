/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#pragma once

#include "verbs_ctx.h"

// Used creating UMR (User Memory Region) and DM (Device Memory).
// Note: Search for "ibv_exp" in the pcx_mem.cc file.
#include <infiniband/verbs_exp.h>

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
  struct ibv_exp_dm *dm; // TODO: Remove the dependency of verbs_exp ("Experimental Verbs")
};

class UsrMem : public NetMem {
public:
  UsrMem(void *buf, size_t length, VerbCtx *ctx);
  ~UsrMem();
};

class RefMem : public NetMem {
public:
  RefMem(NetMem *mem, uint64_t byte_offset, uint32_t length);
  RefMem(const RefMem &srcRef) {
    this->sge = srcRef.sge;
    this->mr = srcRef.mr;
  }
  ~RefMem();
};

class UmrMem : public NetMem {
public:
  UmrMem(std::vector<NetMem *> &mem_reg, VerbCtx *ctx);
  ~UmrMem();
private:
  struct ibv_mr *register_umr(std::vector<NetMem *> &iov, VerbCtx *ctx);
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

