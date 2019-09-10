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

#include "verbs_ctx.h"

VerbCtx *VerbCtx::instance = NULL; // TODO: Consider moving this variable into the class and make it static and private.
bool VerbCtx::safeFlag = false; // TODO: Consider moving this variable into the class and make it static and private.
std::mutex VerbCtx::iniMtx; // TODO: Consider moving this variable into the class and make it static and private.

int VerbCtx::ref = 0; // TODO: Consider moving this variable into the class and make it static and private.

VerbCtx *VerbCtx::getInstance() {
  iniMtx.lock();
  if (ref == 0) {
    instance = new VerbCtx();
  }
  ++ref;
  iniMtx.unlock();
  return instance;
}

void VerbCtx::remInstance() {
  iniMtx.lock();
  --ref;
  if (ref == 0) {
    delete (instance);
    instance = NULL;
    safeFlag = false;
  }
  iniMtx.unlock();
}

VerbCtx::~VerbCtx() {
  if (ibv_destroy_qp(this->umr_qp)) {
    PERR(CouldNotDestroyQP); // throw("Couldn't destroy QP");    
  }

  if (ibv_destroy_cq(this->umr_cq)) {
    PERR(CouldNotDestroyCQ); // throw("Couldn't destroy CQ");
  }

  if (ibv_dealloc_pd(this->pd)) {
    PERR(CouldNotDeallocatePD); // throw("Couldn't deallocate PD");
  }

  if (ibv_close_device(this->context)) {
    PERR(CouldNotReleaseContext); // throw("Couldn't release context");
  }
}

// VerbCtx::VerbCtx(char *ib_devname){
VerbCtx::VerbCtx() {

  if (safeFlag) {
    fprintf(stderr, "ERROR - verb context initiated twice!");
    PERR(VerbsCtxInitiatedTwice); // throw "ERROR - verb context initiated twice!";
  }

  PRINTF("VerbCtx C'tor: Current Time: %s \n", __TIME__);

  safeFlag = true;

  const char *ib_devname = std::getenv("PCX_DEVICE");

  int number_of_ib_devices;
  struct ibv_device **dev_list = ibv_get_device_list(&number_of_ib_devices);
  struct ibv_device *ib_dev;
  if (!dev_list) {
    PERR(FailedToGetIbDeviceList); // throw("Failed to get IB devices list");
  }

  for (int i = 0; i<number_of_ib_devices; i++) {
      PRINTF("Available device #%d: %s.\n", i, ibv_get_device_name(dev_list[i]));  
  }

  if (!ib_devname) {
    ib_dev = dev_list[0]; // Get the first available device // TODO: Need to fix this?
    if (!ib_dev) {
      PERR(NoIbDevicesFound); // throw("No IB devices found");
    }
    PRINTF("Will use first available IB device: %s \n", ibv_get_device_name(ib_dev));
  } else {
    int i;
    for (i = 0; dev_list[i]; ++i)
      if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
        break;
    ib_dev = dev_list[i];
    if (!ib_dev) {
      PERR(NoEnvIbDeviceFound); // throw("IB device from ENV not found");
    }
  }

  PRINTF("Using IB device: %s \n", ibv_get_device_name(ib_dev));

  PRINT(ibv_get_device_name(ib_dev));
  this->context = ibv_open_device(ib_dev);
  ibv_free_device_list(dev_list);
  if (!this->context) {
    PERR(FailedToOpenIbDevice); // throw "Couldn't get context (failed to open an IB device)";
  }

  this->pd = ibv_alloc_pd(this->context);
  if (!this->pd) {
    PRINT("Couldn't allocate PD");
    goto clean_comp_channel;
  }

  this->channel = NULL; // TODO

  this->umr_cq = ibv_create_cq(this->context, CX_SIZE, NULL, NULL, 0);
  if (!this->umr_cq) {
    PERR(CouldNotCreateCQ); // throw "Couldn't create CQ";
  }
  memset(&this->attrs, 0, sizeof(this->attrs));
  this->attrs.comp_mask = IBV_EXP_DEVICE_ATTR_UMR;
  this->attrs.comp_mask |= IBV_EXP_DEVICE_ATTR_MAX_DM_SIZE;

  if (ibv_exp_query_device(this->context, &this->attrs)) {
    PERR(CouldNotQueryDevice); // throw "Couldn't query device attributes";
  }

  if (!(this->attrs.comp_mask & IBV_EXP_DEVICE_ATTR_MAX_DM_SIZE) ||
      !(this->attrs.max_dm_size)) {
    this->maxMemic = 0;
  } else {
    this->maxMemic = this->attrs.max_dm_size;
  }

  {
    struct ibv_exp_qp_init_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.pd = this->pd;
    attr.send_cq = this->umr_cq;
    attr.recv_cq = this->umr_cq;
    attr.qp_type = IBV_QPT_RC;
    attr.comp_mask = IBV_EXP_QP_INIT_ATTR_PD |
                     IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS |
                     IBV_EXP_QP_INIT_ATTR_MAX_INL_KLMS;
    attr.exp_create_flags = IBV_EXP_QP_CREATE_UMR;
    attr.cap.max_send_wr = 1;
    attr.cap.max_recv_wr = 0;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 0;
    attr.max_inl_send_klms = MAX_LOCAL_VECTOR_SIZE_TO_REDUCE;

    this->umr_qp = ibv_exp_create_qp(this->context, &attr);
    if (!this->umr_qp) {
      PERR(CouldNotCreateUmrQP); // throw("Couldn't create UMR QP");
    }
  }

  {
    struct ibv_qp_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.pkey_index = 0;
    qp_attr.port_num = 1;
    qp_attr.qp_access_flags = 0;

    if (ibv_modify_qp(this->umr_qp, &qp_attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                                                  IBV_QP_PORT |
                                                  IBV_QP_ACCESS_FLAGS)) {
      PERR(CouldNotInitUmrQp); // throw("Failed to INIT the UMR QP");
    }

    peer_addr_t my_addr;
    rc_qp_get_addr(this->umr_qp, &my_addr);
    rc_qp_connect(&my_addr, this->umr_qp);
  }

  return; // SUCCESS!

clean_qp: // TODO: Check if never used. If not used - delete!
  ibv_destroy_qp(this->umr_qp);

clean_cq: // TODO: Check if never used. If not used - delete!
  ibv_destroy_cq(this->umr_cq);

clean_mr: // TODO: Check if never used. If not used - delete!
  ibv_dealloc_pd(this->pd);

clean_comp_channel: // TODO: Check if never used. If not used - delete!
  ibv_close_device(this->context);

  PERR(CouldNotCreateQP); // throw "Failed to create QP";
}

int rc_qp_get_addr(struct ibv_qp *qp, peer_addr_t *addr) {
  struct ibv_port_attr attr;
  if (ibv_query_port(qp->context, 1, &attr)) {
    fprintf(stderr, "Couldn't get port info\n");
    return 1; // TODO: indicate error?
  }

  addr->lid = attr.lid;
  addr->qpn = qp->qp_num;
  addr->psn = 0x1234;

  if (ibv_query_gid(qp->context, 1, GID_INDEX, &addr->gid)) {
    fprintf(stderr, "can't read sgid of index %d\n", GID_INDEX);
    // PERR(CantReadsGid);
  }
}

int rc_qp_connect(peer_addr_t *addr, struct ibv_qp *qp) {
  struct ibv_qp_attr attr;
  memset((void *)&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTR;
  attr.path_mtu = IBV_MTU_1024;
  attr.dest_qp_num = addr->qpn;
  attr.rq_psn = addr->psn;
  attr.min_rnr_timer = 20;
  attr.max_dest_rd_atomic = 1;
  attr.ah_attr.is_global = 1;
  attr.ah_attr.dlid = addr->lid;
  attr.ah_attr.sl = 0;
  attr.ah_attr.src_path_bits = 0;
  attr.ah_attr.port_num = 1;
  attr.ah_attr.grh.hop_limit = 1;
  attr.ah_attr.grh.dgid = addr->gid;
  attr.ah_attr.grh.sgid_index = GID_INDEX;
  int res;
  res = ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                                     IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                                     IBV_QP_MAX_DEST_RD_ATOMIC |
                                     IBV_QP_MIN_RNR_TIMER);
  if (res) {
    fprintf(stderr, "Failed to modify QP to RTR. reason: %d\n", res);
    PERR(CouldNotModifyQpToRTR); // throw "a";
    // PERR(QpFailedRTR);
  }

  attr.qp_state = IBV_QPS_RTS;
  attr.timeout = 10;
  attr.retry_cnt = 7;
  attr.rnr_retry = 7;
  attr.sq_psn = addr->psn;
  attr.max_rd_atomic = 1;
  if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT |
                                   IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                                   IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC)) {
    // PERR(QpFailedRTS);
    PERR(CouldNotModifyQpToRTS); // throw 3;
  }

  return 0;
}

void print_values(volatile float *buf, int count) { // TODO: Move to utils.cc file
  int i = 0;
  for (i = 0; i < count; ++i) {
    if (i % 8 == 0) {
      fprintf(stderr, "\n");
    }
    fprintf(stderr, "%.1f\t", buf[i]);
  }
  fprintf(stderr, "\n");
}

// count is the number of *bytes* within the buffer
void print_buffer(volatile void *buf, int count) { // TODO: Move to utils.cc file
  int i = 0;
  int line_width = 16;
  int line_count = (count / sizeof(int)) / line_width;
  int line_seperator = 16;
  for (int line = 0; line < line_count; ++line) {
    // After every 'line_seperator' lines, print \n
    if ((line > 0) && (line >= line_seperator) && (line % line_seperator == 0)) {
      fprintf(stderr, "\n");
    }
    fprintf(stderr, "#%02d: ", line);
    for (int column = 0; column < 16; ++column) {
      fprintf(stderr, "%08X  ", ntohl(((int *)buf)[line*line_width + column]));
    }
    fprintf(stderr, "\n");
  }
  fprintf(stderr, "\n");
}
