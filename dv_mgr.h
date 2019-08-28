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

#include "udma_barrier.h"
#include "pcx_mem.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <map>

#define memory_store_fence() asm volatile("" ::: "memory")
#define pci_store_fence() asm volatile("sfence" ::: "memory")

#define CE 0 // TODO: Why is this value is the correct value?

enum mlx5dv_vector_calc_op {
  MLX5DV_VECTOR_CALC_OP_NOP = 0,
  MLX5DV_VECTOR_CALC_OP_ADD,
  MLX5DV_VECTOR_CALC_OP_MAXLOC,
  MLX5DV_VECTOR_CALC_OP_BAND,
  MLX5DV_VECTOR_CALC_OP_BOR,
  MLX5DV_VECTOR_CALC_OP_BXOR,
  MLX5DV_VECTOR_CALC_OP_MIN,
  MLX5DV_VECTOR_CALC_OP_NUMBER
};

enum mlx5dv_vector_calc_data_type {
  MLX5DV_VECTOR_CALC_DATA_TYPE_INT32 = 0,
  MLX5DV_VECTOR_CALC_DATA_TYPE_UINT32,
  MLX5DV_VECTOR_CALC_DATA_TYPE_FLOAT32,
  MLX5DV_VECTOR_CALC_DATA_TYPE_INT64 = 4,
  MLX5DV_VECTOR_CALC_DATA_TYPE_UINT64,
  MLX5DV_VECTOR_CALC_DATA_TYPE_FLOAT64,
  MLX5DV_VECTOR_CALC_DATA_TYPE_NUMBER
};

enum mlx5dv_vector_calc_chunks { // TODO: Check if used somewhere and if not where did it come from?
  MLX5DV_VECTOR_CALC_CHUNK_64 = 0,
  MLX5DV_VECTOR_CALC_CHUNK_128,
  MLX5DV_VECTOR_CALC_CHUNK_256,
  MLX5DV_VECTOR_CALC_CHUNK_512,
  MLX5DV_VECTOR_CALC_CHUNK_1024,
  MLX5DV_VECTOR_CALC_CHUNK_2048,
  MLX5DV_VECTOR_CALC_CHUNK_4096,
  MLX5DV_VECTOR_CALC_CHUNK_8192,
  MLX5DV_VECTOR_CALC_CHUNK_NUMBER
};

struct mlx5_wqe_coredirect_seg {
  uint64_t rsvd;
  uint32_t index;
  uint32_t number;
};

struct mlx5_wqe_vectorcalc_seg {
  uint32_t calc_operation;
  uint32_t rsvd1[2];
  uint32_t options;
  uint32_t rsvd3;
  uint32_t mat_lkey;
  uint64_t mat_addr;
};

struct mlx5_db_seg {
  __be32 opmod_idx_opcode;
  __be32 qpn_ds;
};

struct cqe64 { // TODO: This struct is very similar to the struct mlx5_cqe64 defined in mlxdv.h // TODO: Why cant we take the struct as is from mlx5dv.h?
  uint8_t rsvd0[2];
  __be16 wqe_id;
  uint8_t rsvd4[13];
  uint8_t ml_path;
  uint8_t rsvd20[4];
  __be16 slid;
  __be32 flags_rqpn;
  uint8_t hds_ip_ext;
  uint8_t l4_hdr_type_etc;
  __be16 vlan_info;
  /* TMH is scattered to CQE upon match */
  __be32 srqn_uidx;
  __be32 imm_inValpkey;
  uint8_t app;
  uint8_t app_op;
  __be16 app_info;
  __be32 byte_cnt;
  __be32 rsvd34;
  uint8_t hw_syn;
  uint8_t rsvd38;
  uint8_t vendor_syn;
  uint8_t syn;
  __be32 sop_drop_qpn;
  __be16 wqe_counter;
  uint8_t signature;
  volatile uint8_t op_own;
};

class ValRearmTasks {
public:
  ValRearmTasks();
  ~ValRearmTasks();
  void add(uint32_t *ptr);
  void expand();

  void exec(uint32_t increment, uint32_t src_offset, uint32_t dst_offset);

  size_t size;
  size_t buf_size;
  uint32_t **ptrs;
};

typedef std::map<int, ValRearmTasks> UpdateMap;
typedef UpdateMap::iterator MapIt;

class RearmTasks {
public:
  RearmTasks(){};
  ~RearmTasks(){};
  void add(uint32_t *ptr, int inc);
  void exec(uint32_t offset, int phase, int dups);

private:
  UpdateMap map;
};

class cq_ctx {
public:
  cq_ctx(struct ibv_cq *cq, size_t num_of_cqes);
  ~cq_ctx();
  uint32_t cmpl_cnt;

  struct mlx5dv_cq *cq;
  size_t cqes;
};

class qp_ctx {
public:
  qp_ctx(struct ibv_qp *qp, struct ibv_cq *cq, size_t num_of_wqes,
         size_t num_of_cqes);
  qp_ctx(struct ibv_qp *qp, struct ibv_cq *cq, size_t num_of_wqes,
         size_t num_of_cqes, struct ibv_cq *scq, size_t num_of_send_cqes);
  ~qp_ctx();

  // Send a DoorBell
  void db(uint32_t k = 0);

  // Return *all* the credits to peer QP
  void send_credit();

  void write(const struct ibv_sge *local, const struct ibv_sge *remote);
  void write(NetMem *local, NetMem *remote) {
    write(local->sg(), remote->sg());
  };
  void write(NetMem *local, RefMem remote) { 
    write(local->sg(), remote.sg()); 
  };

  void write_cmpl(const struct ibv_sge *local, const struct ibv_sge *remote);
  void write_cmpl(NetMem *local, NetMem *remote) {
    write_cmpl(local->sg(), remote->sg());
  };
  void write_cmpl(NetMem *local, RefMem remote) {
    write_cmpl(local->sg(), remote.sg());
  };

  void reduce_write(const struct ibv_sge *local, const struct ibv_sge *remote,
                    uint16_t num_vectors, uint8_t op, uint8_t type);
  void reduce_write(NetMem *local, NetMem *remote, uint16_t num_vectors,
                    uint8_t op, uint8_t type) {
    reduce_write(local->sg(), remote->sg(), num_vectors, op, type);
  };
  void reduce_write(NetMem *local, RefMem &remote, uint16_t num_vectors,
                    uint8_t op, uint8_t type) {
    reduce_write(local->sg(), remote.sg(), num_vectors, op, type);
  };

  void reduce_write_cmpl(const struct ibv_sge *local,
                         const struct ibv_sge *remote, uint16_t num_vectors,
                         uint8_t op, uint8_t type);
  void reduce_write_cmpl(NetMem *local, NetMem *remote, uint16_t num_vectors,
                         uint8_t op, uint8_t type) {
    reduce_write_cmpl(local->sg(), remote->sg(), num_vectors, op, type);
  };

  void cd_send_enable(qp_ctx *slave_qp);
  void cd_recv_enable(qp_ctx *slave_qp);
  void cd_wait(qp_ctx *slave_qp);
  void cd_wait_send(qp_ctx *slave_qp);
  void cd_wait_signal(qp_ctx *slave_qp);
  void nop(size_t num_pad);

  void pad(int half = 0);
  void dup();
  void fin();

  int poll();

  void rearm();

  void print_sq();
  void print_rq();
  void print_cq();

  void set_pair(qp_ctx *qp) { 
    this->pair = qp; 
  };

  // Holds the peer's QP.
  qp_ctx *pair; // TODO: Rename this to "peer"

  cq_ctx *scq;

  uint32_t get_poll_cnt() { 
    return this->poll_cnt; 
  };

private:
  uint32_t write_cnt;
  uint32_t cmpl_cnt;
  uint32_t poll_cnt;

  uint64_t qpn;

  int offset;
  struct mlx5dv_qp *qp;
  struct mlx5dv_cq *cq;

  uint32_t number_of_duplicates;

  int phase;
  uint32_t exe_cnt;
  RearmTasks tasks;

  // DoorBell segment
  struct mlx5_db_seg dbseg;

  // The number of WQEs that are expected to be executed 
  // during a single collective operation
  size_t wqes;

  // The number of CQEs that are expected after a single
  // collective operation is done
  size_t cqes;
  volatile struct cqe64 *cur_cqe;
};
