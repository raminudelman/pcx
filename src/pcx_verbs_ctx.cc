/*
 * Copyright (c) 2019-present, Mellanox Technologies Ltd. 
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "pcx_verbs_ctx.h"

VerbCtx *VerbCtx::instance = NULL; // TODO: Consider moving this variable into
                                   // the class and make it static and private.
bool VerbCtx::safeFlag = false; // TODO: Consider moving this variable into the
                                // class and make it static and private.
std::mutex VerbCtx::iniMtx;     // TODO: Consider moving this variable into the
                                // class and make it static and private.

int VerbCtx::ref = 0; // TODO: Consider moving this variable into the class and
                      // make it static and private.

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
    if (ref == 0) { // TODO: Is this "if" should be check only in "debug mode"?
        PERR(CouldNotRemoveVerbsInstance);
    }
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
    #ifdef PCX_DEBUG
        fprintf(stderr,"%s\n", "PCX_DEBUG is defined");
    #else
        fprintf(stderr,"%s\n", "PCX_DEBUG is NOT defined");
    #endif
    #ifdef DEBUG
        fprintf(stderr,"%s\n", "DEBUG is defined");
    #else
        fprintf(stderr,"%s\n", "DEBUG is NOT defined");
    #endif

    if (safeFlag) {
        PRINTF("ERROR - verb context initiated twice!");
        PERR(VerbsCtxInitiatedTwice); // throw "ERROR - verb context initiated
                                      // twice!";
    }

    PRINTF("VerbCtx C'tor: Current Time: %s \n", __TIME__);

    safeFlag = true;

    const char *ib_devname = std::getenv("PCX_DEVICE");

    int number_of_ib_devices;
    struct ibv_device **dev_list = ibv_get_device_list(&number_of_ib_devices);
    struct ibv_device *ib_dev;
    if (!dev_list) {
        PERR(
            FailedToGetIbDeviceList); // throw("Failed to get IB devices list");
    }

    for (int i = 0; i < number_of_ib_devices; i++) {
        PRINTF("Available device #%d: %s.\n", i,
               ibv_get_device_name(dev_list[i]));
    }

    if (!ib_devname) {
        ib_dev = dev_list[0]; // Get the first available device // TODO: Need to
                              // fix this?
        if (!ib_dev) {
            PERR(NoIbDevicesFound); // throw("No IB devices found");
        }
        PRINTF("Will use first available IB device: %s \n",
               ibv_get_device_name(ib_dev));
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

    if (!this->context) {
        PERR(FailedToOpenIbDevice); // throw "Couldn't get context (failed to
                                    // open an IB device)";
    }
    PRINTF("Opened IB device: %s \n", ibv_get_device_name(ib_dev));

    ibv_free_device_list(dev_list);

    this->pd = ibv_alloc_pd(this->context);
    if (!this->pd) {
        PRINT("Couldn't allocate PD");
        goto clean_comp_channel;
    }

    PRINTF("PD was allocated\n");

    this->channel = NULL; // TODO

    this->umr_cq = ibv_create_cq(this->context, CX_SIZE, NULL, NULL, 0);
    if (!this->umr_cq) {
        PERR(CouldNotCreateCQ); // throw "Couldn't create CQ";
    }

    PRINTF("UMR CQ was created\n");

    memset(&this->attrs, 0, sizeof(this->attrs));
    this->attrs.comp_mask = IBV_EXP_DEVICE_ATTR_UMR;
    this->attrs.comp_mask |= IBV_EXP_DEVICE_ATTR_MAX_DM_SIZE;

    if (ibv_exp_query_device(this->context, &this->attrs)) {
        PERR(CouldNotQueryDevice); // throw "Couldn't query device attributes";
    }
    PRINTF("Query device finished \n");

    if (!(this->attrs.comp_mask & IBV_EXP_DEVICE_ATTR_MAX_DM_SIZE) ||
        !(this->attrs.max_dm_size)) {
        PRINTF("Not using MEMIC as device does not support it\n");
        this->maxMemic = 0;
    } else {
        PRINTF("Max DM size: %d \n", this->maxMemic);
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
        PRINTF("UMR QP start creation\n");
        this->umr_qp = ibv_exp_create_qp(this->context, &attr);
        if (!this->umr_qp) {
            PERR(CouldNotCreateUmrQP); // throw("Couldn't create UMR QP");
        }
        PRINTF("UMR QP was created\n");
    }

    {
        struct ibv_qp_attr qp_attr;
        memset(&qp_attr, 0, sizeof(qp_attr));
        qp_attr.qp_state = IBV_QPS_INIT;
        qp_attr.pkey_index = 0;
        qp_attr.port_num = 1;
        qp_attr.qp_access_flags = 0;

        if (ibv_modify_qp(this->umr_qp, &qp_attr,
                          IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                              IBV_QP_ACCESS_FLAGS)) {
            PERR(CouldNotInitUmrQp); // throw("Failed to INIT the UMR QP");
        }

        peer_addr_t my_addr;
        rc_qp_get_addr(this->umr_qp, &my_addr);
        rc_qp_connect(&my_addr, this->umr_qp);
    }

    PRINTF("Created context\n");

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

int VerbCtx::register_dm(size_t length, uint64_t access_permissions,
                         PcxDeviceMemory *pcx_device_memory, ibv_mr **mr) {

    if (length > this->maxMemic) {
        PERR(AllocateDeviceMemoryFailed);
    };

    struct ibv_exp_alloc_dm_attr dm_attr = {0};
    dm_attr.length = length;
    struct ibv_exp_dm *dm = ibv_exp_alloc_dm(this->context, &dm_attr);
    if (!dm) {
        PERR(AllocateDeviceMemoryFailed);
    }

    pcx_device_memory->SetDeviceMemory(&dm);
    if (!pcx_device_memory->IsAllocated()) {
        PERR(AllocateDeviceMemoryFailed);
    }

    struct ibv_exp_reg_mr_in mr_in;
    mr_in.pd = this->pd;
    mr_in.addr = 0;
    mr_in.length = length;
    mr_in.exp_access = IB_ACCESS_FLAGS;
    mr_in.create_flags = 0;
    mr_in.dm = dm;
    mr_in.comp_mask = IBV_EXP_REG_MR_DM;

    *mr = ibv_exp_reg_mr(&mr_in);
    if (!(*mr)) {
        PERR(ExpRegMrFailed)
    }
    PRINTF("DM was allocated with size %d\n", length);
    return 0;
}

int VerbCtx::register_umr(std::vector<PcxMemRegion *> &mem_vec,
                          struct ibv_mr **res_mr) {
    unsigned mem_reg_cnt = mem_vec.size();

    if (mem_reg_cnt > this->attrs.umr_caps.max_klm_list_size) {
        PERR(NotEnoughKLMs);
    }

    if (mem_reg_cnt == 0) {
        PERR(EmptyUMR);
    }

    struct ibv_exp_mkey_list_container *umr_mkey = nullptr;
    if (mem_reg_cnt > this->attrs.umr_caps.max_send_wqe_inline_klms) {
        struct ibv_exp_mkey_list_container_attr list_container_attr;
        list_container_attr.pd = this->pd;
        list_container_attr.mkey_list_type = IBV_EXP_MKEY_LIST_TYPE_INDIRECT_MR;
        list_container_attr.max_klm_list_size = mem_reg_cnt;
        list_container_attr.comp_mask = 0;
        umr_mkey = ibv_exp_alloc_mkey_list_memory(&list_container_attr);
        if (!umr_mkey) {
            PERR(NoUMRKey);
        }
    } else {
        umr_mkey = NULL;
    }

    struct ibv_exp_create_mr_in mrin;
    memset(&mrin, 0, sizeof(mrin));
    mrin.pd = this->pd;
    mrin.attr.create_flags = IBV_EXP_MR_INDIRECT_KLMS;
    mrin.attr.exp_access_flags = IB_ACCESS_FLAGS;
    mrin.attr.max_klm_list_size = mem_reg_cnt;
    *res_mr = ibv_exp_create_mr(&mrin);
    if (!(*res_mr)) {
        PERR(CreateMRFailed);
    }

    int buf_idx = 0;
    struct ibv_exp_mem_region *mem_reg = (struct ibv_exp_mem_region *)malloc(
        mem_reg_cnt * sizeof(struct ibv_exp_mem_region));
    for (buf_idx = 0; buf_idx < mem_reg_cnt; ++buf_idx) {
        struct ibv_exp_mem_region *mem = mem_vec[buf_idx]->GetPcxMemRegion();
        mem_reg[buf_idx].base_addr = mem->base_addr;
        mem_reg[buf_idx].length = mem->length;
        mem_reg[buf_idx].mr = mem->mr;
    }

    // Create the UMR WR (work request)
    struct ibv_exp_send_wr wr, *bad_wr;
    memset(&wr, 0, sizeof(wr));
    wr.exp_opcode = IBV_EXP_WR_UMR_FILL;
    wr.exp_send_flags = IBV_EXP_SEND_SIGNALED;
    wr.ext_op.umr.umr_type = IBV_EXP_UMR_MR_LIST;
    wr.ext_op.umr.memory_objects = umr_mkey;
    wr.ext_op.umr.modified_mr = *res_mr;
    wr.ext_op.umr.base_addr = mem_reg[0].base_addr;
    wr.ext_op.umr.num_mrs = mem_reg_cnt;
    wr.ext_op.umr.mem_list.mem_reg_list = mem_reg;
    if (!umr_mkey) {
        wr.exp_send_flags |= IBV_EXP_SEND_INLINE;
    }

    // Post the UMR WR and wait for it to complete
    if (int res = ibv_exp_post_send(this->umr_qp, &wr, &bad_wr)) {
        RES_ERR(UMR_PostFailed, res);
    }
    struct ibv_wc wc;
    for (;;) { // Wait for the UMR WR to complete
        int ret = ibv_poll_cq(this->umr_cq, 1, &wc);
        if (ret < 0) {
            PERR(UMR_PollFailed);
        }
        if (ret == 1) {
            if (wc.status != IBV_WC_SUCCESS) {
                PERR(UMR_CompletionInError);
            }
            break;
        }
    }

    if (umr_mkey) {
        ibv_exp_dealloc_mkey_list_memory(umr_mkey);
    }

    free(mem_reg);

    return 0;
}

struct ibv_qp *VerbCtx::create_coredirect_master_qp(struct ibv_cq *cq,
                                                    uint16_t send_wq_size) {

    int rc = PCOLL_SUCCESS;
    struct ibv_exp_qp_init_attr init_attr;
    struct ibv_qp_attr attr;
    struct ibv_qp *_mq;

    memset(&init_attr, 0, sizeof(init_attr));
    init_attr.qp_context = NULL;
    init_attr.send_cq = cq;
    init_attr.recv_cq = cq;
    init_attr.srq = NULL;
    init_attr.cap.max_send_wr = send_wq_size;
    init_attr.cap.max_recv_wr = 0;
    init_attr.cap.max_send_sge = 1;
    init_attr.cap.max_recv_sge = 1;
    init_attr.cap.max_inline_data = 0;
    init_attr.qp_type = IBV_QPT_RC;
    init_attr.sq_sig_all = 0;
    init_attr.pd = this->pd;
    init_attr.comp_mask =
        IBV_QP_INIT_ATTR_PD | IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS;
    init_attr.exp_create_flags = IBV_EXP_QP_CREATE_CROSS_CHANNEL |
                                 IBV_EXP_QP_CREATE_IGNORE_SQ_OVERFLOW |
                                 IBV_EXP_QP_CREATE_IGNORE_RQ_OVERFLOW;
    ;
    _mq = ibv_exp_create_qp(this->context, &init_attr);

    if (NULL == _mq) {
        rc = PCOLL_ERROR;
    }

    if (rc == PCOLL_SUCCESS) {
        attr.qp_state = IBV_QPS_INIT;
        attr.pkey_index = 0;
        attr.port_num = 1;
        attr.qp_access_flags = 0;

        rc = ibv_modify_qp(_mq, &attr,
                           IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                               IBV_QP_ACCESS_FLAGS);
        if (rc) {
            rc = PCOLL_ERROR;
        }
    }

    if (rc == PCOLL_SUCCESS) {
        union ibv_gid gid;
        memset(&attr, 0, sizeof(attr));
        attr.qp_state = IBV_QPS_RTR;
        attr.path_mtu = IBV_MTU_1024;
        attr.dest_qp_num = _mq->qp_num;
        attr.rq_psn = 0;
        attr.max_dest_rd_atomic = 1;
        attr.min_rnr_timer = 12;
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.grh.sgid_index = GID_INDEX;
        attr.ah_attr.dlid = 0;
        attr.ah_attr.sl = 0;
        attr.ah_attr.src_path_bits = 0;
        attr.ah_attr.port_num = 1;

        if (ibv_query_gid(this->context, 1, GID_INDEX, &gid)) {
            PRINTF("can't read sgid of index %d\n", GID_INDEX);
            // PERR(CantReadsGid);
        }

        attr.ah_attr.grh.dgid = gid;

        rc =
            ibv_modify_qp(_mq, &attr,
                          IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                              IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                              IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);

        if (rc) {
            PERR(QpFailedRTR);
        }
    }

    if (rc == PCOLL_SUCCESS) {
        attr.qp_state = IBV_QPS_RTS;
        attr.timeout = 14;
        attr.retry_cnt = 7;
        attr.rnr_retry = 7;
        attr.sq_psn = 0;
        attr.max_rd_atomic = 1;
        rc = ibv_modify_qp(_mq, &attr,
                           IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                               IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                               IBV_QP_MAX_QP_RD_ATOMIC);
        if (rc) {
            rc = PCOLL_ERROR;
        }
    }
    return _mq;
}

struct ibv_qp *VerbCtx::create_coredirect_slave_rc_qp(
    struct ibv_cq *cq, uint16_t send_wq_size, uint16_t recv_rq_size,
    struct ibv_cq *s_cq, int slaveRecv, int slaveSend) {
    struct ibv_exp_qp_init_attr init_attr;
    struct ibv_qp_attr attr;
    memset(&init_attr, 0, sizeof(init_attr));
    init_attr.qp_context = NULL;
    init_attr.send_cq = (s_cq == NULL) ? cq : s_cq;
    init_attr.recv_cq = cq;
    init_attr.cap.max_send_wr = send_wq_size;
    init_attr.cap.max_recv_wr = recv_rq_size;
    init_attr.cap.max_send_sge = 1;
    init_attr.cap.max_recv_sge = 1;
    init_attr.qp_type = IBV_QPT_RC;
    init_attr.pd = this->pd;
    init_attr.comp_mask =
        IBV_QP_INIT_ATTR_PD | IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS;
    init_attr.exp_create_flags = IBV_EXP_QP_CREATE_CROSS_CHANNEL |
                                 IBV_EXP_QP_CREATE_IGNORE_SQ_OVERFLOW |
                                 IBV_EXP_QP_CREATE_IGNORE_RQ_OVERFLOW;

    if (slaveSend) {
        init_attr.exp_create_flags |= IBV_EXP_QP_CREATE_MANAGED_SEND;
    }

    if (slaveRecv) {
        init_attr.exp_create_flags |= IBV_EXP_QP_CREATE_MANAGED_RECV;
    }

    struct ibv_qp *qp = ibv_exp_create_qp(this->context, &init_attr);

    struct ibv_qp_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.pkey_index = 0;
    qp_attr.port_num = 1;
    qp_attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;

    if (ibv_modify_qp(qp, &qp_attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                          IBV_QP_ACCESS_FLAGS)) {
        PERR(QPInitFailed);
    }

    return qp;
}

struct ibv_cq *VerbCtx::create_coredirect_cq(int cqe, void *cq_context,
                                             struct ibv_comp_channel *channel,
                                             int comp_vector) {
    if (cqe == 0) {
        ++cqe;
    }

    struct ibv_cq *cq =
        ibv_create_cq(this->context, cqe, cq_context, channel, comp_vector);

    if (!cq) {
        PERR(CQCreateFailed);
    }

    struct ibv_exp_cq_attr attr;
    attr.cq_cap_flags = IBV_EXP_CQ_IGNORE_OVERRUN;
    attr.comp_mask = IBV_EXP_CQ_ATTR_CQ_CAP_FLAGS;

    int res = ibv_exp_modify_cq(cq, &attr, IBV_EXP_CQ_CAP_FLAGS);
    if (res) {
        PERR(CQModifyFailed);
    }

    return cq;
}

int rc_qp_get_addr(struct ibv_qp *qp, peer_addr_t *addr) {
    struct ibv_port_attr attr;
    if (ibv_query_port(qp->context, 1, &attr)) {
        PRINTF("Couldn't get port info\n");
        return 1; // TODO: indicate error?
    }

    addr->lid = attr.lid;
    addr->qpn = qp->qp_num;
    addr->psn = 0x1234;

    if (ibv_query_gid(qp->context, 1, GID_INDEX, &addr->gid)) {
        PRINTF("can't read sgid of index %d\n", GID_INDEX);
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
    res = ibv_modify_qp(qp, &attr,
                        IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                            IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                            IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    if (res) {
        PRINTF("Failed to modify QP to RTR. reason: %d\n", res);
        PERR(CouldNotModifyQpToRTR); // throw "a";
                                     // PERR(QpFailedRTR);
    }

    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 10;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = addr->psn;
    attr.max_rd_atomic = 1;
    if (ibv_modify_qp(qp, &attr,
                      IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                          IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                          IBV_QP_MAX_QP_RD_ATOMIC)) {
        // PERR(QpFailedRTS);
        PERR(CouldNotModifyQpToRTS); // throw 3;
    }

    return 0;
}
