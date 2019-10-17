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

#include "pcx_mem.h"

NetMem::~NetMem() {}

HostMem::HostMem(size_t length, VerbCtx *ctx) {
    this->buf = malloc(length);

    if (!this->buf) {
        PERR(AllocateMemoryFailed);
    }

    this->sge.addr = (uint64_t)buf;
    this->mr = ibv_reg_mr(ctx->pd, this->buf, length, IB_ACCESS_FLAGS);
    if (!this->mr) {
        PERR(RegMrFailed);
    }
    this->sge.length = length;
    this->sge.lkey = this->mr->lkey;
}

HostMem::~HostMem() {
    ibv_dereg_mr(this->mr);
    free(this->buf);
}

Memic::Memic(size_t length, VerbCtx *ctx) {
    ctx->register_dm(length, IB_ACCESS_FLAGS, &(this->pcx_dm), &(this->mr));

    this->sge.addr = 0;
    this->sge.length = length;
    this->sge.lkey = this->mr->lkey;
}

Memic::~Memic() {
    ibv_dereg_mr(this->mr);
}

UsrMem::UsrMem(void *buf, size_t length, VerbCtx *ctx) {
    this->sge.addr = (uint64_t)buf;
    this->mr = ibv_reg_mr(ctx->pd, buf, length, IB_ACCESS_FLAGS);

    if (!this->mr) {
        PERR(RegMrFailed);
    }
    this->sge.length = length;
    this->sge.lkey = this->mr->lkey;
}

UsrMem::~UsrMem() { ibv_dereg_mr(this->mr); }

RefMem::RefMem(NetMem *mem, uint64_t offset, uint32_t length) {
    this->sge = *(mem->sg());
    this->sge.addr += offset;
    this->sge.length = length;
    this->mr = mem->getMr();
}

RefMem::RefMem(const RefMem &srcRef) {
    this->sge = srcRef.sge;
    this->mr = srcRef.mr;
}

RefMem::~RefMem() {}

UmrMem::UmrMem(std::vector<NetMem *> &iov, VerbCtx *ctx) {

    std::vector<PcxMemRegion*> pcx_mem_region_vec;
    for (int buf_idx = 0; buf_idx < iov.size(); ++buf_idx) {
        struct ibv_mr* mr = iov[buf_idx]->getMr();
        pcx_mem_region_vec.push_back(new PcxMemRegion(iov[buf_idx]->sg()->addr, iov[buf_idx]->sg()->length, &mr));
    }

    int ret = ctx->register_umr(pcx_mem_region_vec, &(this->mr));

    for (auto it = pcx_mem_region_vec.begin(); it != pcx_mem_region_vec.end(); ++it) {
         delete (*it);
    }

    this->sge.lkey = mr->lkey;
    this->sge.length = iov[0]->sg()->length;
    this->sge.addr = iov[0]->sg()->addr;
}

UmrMem::~UmrMem() { ibv_dereg_mr(this->mr); }

RemoteMem::RemoteMem(uint64_t addr, uint32_t rkey) {
    sge.addr = addr;
    sge.lkey = rkey;
    this->mr = NULL;
}

RemoteMem::~RemoteMem() {};

PipeMem::PipeMem(size_t length_, size_t depth_, VerbCtx *ctx, int mem_type_)
    : length(length_), depth(depth_), mem_type(mem_type_), cur(0) {

    switch (mem_type) {
    
    // Device memory allocation
    case(PCX_MEMORY_TYPE_MEMIC) : {
        bool success = true;
        try {
            mem = new Memic(length * depth, ctx);
        }
        catch (const PCX_ERR_AllocateDeviceMemoryFailed &e) {
            success = false;
        }
        if (success) {
            PRINT("Memic allocated");
            break;
        }
        PRINT("Memic allocation failed, falling-back to using host memory...");
    }
        
    // Host memory allocation
    case(PCX_MEMORY_TYPE_HOST) : {
        mem = new HostMem(length * depth, ctx);
        break;
    }
    default:
        PERR(MemoryNotSupported);
    }
}

PipeMem::PipeMem(size_t length_, size_t depth_, RemoteMem *remote)
    : length(length_), depth(depth_), mem_type(PCX_MEMORY_TYPE_REMOTE), cur(0) {

    mem = new RemoteMem(remote->sg()->addr, remote->sg()->lkey);
}

PipeMem::PipeMem(void *buf, size_t length_, size_t depth_, VerbCtx *ctx)
    : length(length_), depth(depth_), mem_type(PCX_MEMORY_TYPE_USER), cur(0) {

    mem = new UsrMem(buf, length * depth, ctx);
}

PipeMem::~PipeMem() { delete (mem); }

RefMem PipeMem::operator[](size_t idx) {
    return RefMem(this->mem, length * (idx % depth), length);
}

RefMem PipeMem::next() {
    ++cur;
    return RefMem(this->mem, length * ((cur - 1) % depth), length);
}

void PipeMem::print() {
    fprintf(stderr, "Pipelined Memory:\n");
    volatile float *buf = (volatile float *)this->mem->sg()->addr;
    int count = length * depth / 4;

    int i = 0;
    for (i = 0; i < count; ++i) {
        if (i % 8 == 0) {
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "%.1f\t", buf[i]);
    }
    fprintf(stderr, "\n");
}
