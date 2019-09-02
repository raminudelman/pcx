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


extern "C" {
#include <cstring> // TODO: This include should be moved to mlx5dv. mlx5dv uses memcpy function without including propely the library!
#include <infiniband/mlx5dv.h>
}

// Needed for creating QPs that support Cross-Channel (CORE-Direct).
#include <infiniband/verbs_exp.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mutex>

#define VALIDITY_CHECKX
#define HANG_REPORTX

#define GID_INDEX 3

#define IB_ACCESS_FLAGS                                                        \
  (IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ)

#define PCX_ERROR(exp)                                                         \
  class PCX_ERR_##exp : public std::exception {                                \
    const char *what() const throw() { return #exp; };                         \
  };

#define PCX_ERROR_RES(exp)                                                     \
  class PCX_ERR_##exp : public std::exception {                                \
  public:                                                                      \
    PCX_ERR_##exp(int val) : std::exception(), x(val){};                       \
    const char *what() const throw() {                                         \
      /*sprintf( str, \"%s %d\n\", #exp , x );*/                               \
      return #exp;                                                             \
    };                                                                         \
                                                                               \
  private:                                                                     \
    int x;                                                                     \
    char str[50];                                                              \
  };

#define PERR(exp) throw PCX_ERR_##exp();
#define RES_ERR(exp, val) throw PCX_ERR_##exp(val);

#ifdef DEBUG
#define PRINT(x) fprintf(stderr, "%s\n", x);
#define PRINTF(f_, ...) fprintf(stderr, (f_), ##__VA_ARGS__)
#else
#define PRINT(x)
#define PRINTF(f_, ...)
#endif

// Used in pcx_mem.cc
PCX_ERROR(NotEnoughKLMs)
PCX_ERROR(NoUMRKey)
PCX_ERROR(CreateMRFailed)
PCX_ERROR(UMR_PollFailed)
PCX_ERROR(UMR_CompletionInError)
PCX_ERROR_RES(UMR_PostFailed)
PCX_ERROR(EmptyUMR)
PCX_ERROR(MemoryNotSupported)
PCX_ERROR(AllocateDeviceMemoryFailed)
PCX_ERROR(AllocateMemoryFailed)
PCX_ERROR(RegMrFailed)
PCX_ERROR(ExpRegMrFailed)

// Used in verbs_ctx.cc
PCX_ERROR(CouldNotCreateQP)
PCX_ERROR(CouldNotDestroyQP)
PCX_ERROR(CouldNotCreateUmrQP)
PCX_ERROR(CouldNotInitUmrQp)
PCX_ERROR(CouldNotCreateCQ)
PCX_ERROR(CouldNotDestroyCQ)
PCX_ERROR(CouldNotDeallocatePD)
PCX_ERROR(CouldNotReleaseContext)
PCX_ERROR(VerbsCtxInitiatedTwice)
PCX_ERROR(FailedToGetIbDeviceList)
PCX_ERROR(NoIbDevicesFound)
PCX_ERROR(NoEnvIbDeviceFound)
PCX_ERROR(FailedToOpenIbDevice)
PCX_ERROR(CouldNotQueryDevice)
PCX_ERROR(CouldNotModifyQpToRTR)
PCX_ERROR(CouldNotModifyQpToRTS)

//#define RX_SIZE 16 // TODO: Not used. What was the purpose? Should be removed?
#define CX_SIZE 16 // TODO: Should be removed

class VerbCtx {
private:
  VerbCtx(); // TODO: Need to make this public and use some way of allocating only a single instance. The function "getInstance" is redandent
  static VerbCtx *instance;
  static int ref;
  static bool safeFlag;
  static std::mutex iniMtx;

public:
  static VerbCtx *getInstance();
  static void remInstance();

  ~VerbCtx();
  struct ibv_context *context;
  struct ibv_pd *pd;
  struct ibv_cq *umr_cq;
  struct ibv_qp *umr_qp;
  struct ibv_comp_channel *channel;
  struct ibv_exp_device_attr attrs;
  std::mutex mtx;

  size_t maxMemic;
};

typedef struct peer_addr { // TODO: Change struct name to somthing more informative
  int lid;
  int qpn;
  int psn;
  union ibv_gid gid;
} peer_addr_t;

typedef struct rd_peer_info {
  uintptr_t buf;
  union {
    uint32_t rkey;
    uint32_t lkey;
  };
  peer_addr_t addr;
} rd_peer_info_t;

int rc_qp_get_addr(struct ibv_qp *qp, peer_addr_t *addr);
int rc_qp_connect(peer_addr_t *addr, struct ibv_qp *qp);

// Debug functions
void print_values(volatile float *buf, int count);
void print_buffer(volatile void *buf, int count);
