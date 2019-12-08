/*
 * Copyright (c) 2019-present, Mellanox Technologies Ltd. 
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "pcx_allreduce_alg_common.h"
#include "pcx_comm_graph.h"
#include "pcx_mem.h"
#include "pcx_qps.h"
#include "pcx_verbs_ctx.h"

#ifdef PCX_DEBUG
#define PCX_KING_PRINT(args...)                                                \
    fprintf(stderr, "(%s: %d) in function %s [%d]: ", __FILE__, __LINE__,      \
            __func__, contextRank_);                                           \
    fprintf(stderr, args)
#else
#define PCX_KING_PRINT(args...)
#endif

#define KING_PIPELINE_DEPTH 1

// Performs data exchange between peers in the algorithm.
// Sends data of size 'size' to 'peer' from 'send_buf' and
// receives data of size 'size' from 'peer' to 'recv_buf'.
// This function is used for connecting the QPs of two ranks.
// After the execution of this function, the ranks will be able to communicate
// via the two QPs (each rank through it's own QP on it's own end)
//
// Args:
//    comm : Communicator that holds the rank ID of the current rank
//    peer : Rank ID of the rank that will take part in the data exchange
//           with the current rank
//    send_buf: The buffer that wil be sent to the rank with
//              rank id equals to 'peer'.
//    recv_buf: The buffer that receive data from the rank with
//              rank id equals to 'peer'.
//
int p2p_exchange(void *comm, volatile void *send_buf, volatile void *recv_buf,
                 size_t size, uint32_t peer, uint32_t tag);

template <typename T> class PcxAllreduceChunkedKingAlg {
  public:
    PcxAllreduceChunkedKingAlg(const int contextSize, const int contextRank,
                               const std::vector<T *> &ptrs, const int count,
                               uint32_t tag,
                               void *comm) // TODO: Need to add as argument also
                                           // ReductionFunction/Type.
        : contextSize_(contextSize), contextRank_(contextRank), ptrs_(ptrs),
          count_(count), bytes_(count_ * sizeof(T)), tag_(tag), comm_(comm) {

        PCX_KING_PRINT("Initializing PcxAllreduceChunkedKingAlg \n");

        // In case the communicator is of size 1,
        // No need to reduce the ptrs vector, because
        // it's already reduced. The reduced result is
        // the first element in the ptrs vector (ptrs[0])
        if (this->contextSize_ == 1) {
            return;
        }

        // PCX performs the elements reduction on the NIC using Vector-CALC.
        // The reduction is on the number of elements in ptrs and another
        // element that is the result from a peer rank
        if ((ptrs.size() + 1) > MAX_LOCAL_VECTOR_SIZE_TO_REDUCE) {
            fprintf(
                stderr,
                "PCX does not support more than %d to be reduced on the NIC",
                MAX_LOCAL_VECTOR_SIZE_TO_REDUCE);
        }

        // Step #1:
        // Initialize verbs context (choose IB device, open PD, etc.)
        ibv_ctx_ = VerbCtx::getInstance();
        PCX_KING_PRINT("Verbs initiated \n");

        // Step #2 & #3:  // TODO: Improve the comment/documentation
        // Connect to the (recursive-doubling) peers and pre-post operations
        connect_and_prepare();
        mone_ = 0;
    }

    virtual ~PcxAllreduceChunkedKingAlg() {
        delete (rd_.mqp);
        delete (rd_.lqp);
        delete (rd_.graph);
        delete (rd_.result);
        delete[](rd_.peers);

        // Deregister memory
        delete mem_.tmpMem;
        PCX_KING_PRINT("Freeing UMR and freeing user memory \n");
        delete (mem_.umr_mem);
        freeIov(mem_.usr_vec);

        VerbCtx::remInstance();
    }

    void run() {
        PCX_KING_PRINT("King allreduce run started \n");
        debug_write_input();

        rd_.graph->mqp->qp->db(); // TODO: graph has db() API function. Use it!
                                  // mpq should not be accessed!
        PCX_KING_PRINT("Sent Doorbell to Management QP \n");

        // Calling the rearm after the collective operation started (using the
        // DoorBell) makes the rearm process to run in parallel with the
        // collective algorithm.
        rd_.graph->mqp->qp->rearm();
        PCX_KING_PRINT("Sent Rearm command to Management QP \n");

        int res = 0;
        uint64_t count = 0;
        while (res == 0) {
            res = rd_.lqp->qp->poll();
            ++count;
            if (contextRank_ == 0) {
                debug_hang_report(count);
            }
        }
        debug_check_output();
        ++mone_;
        PCX_KING_PRINT("[%d] Done running PcxRingAllReduce \n", contextRank_);
    }

    void register_memory() {
        unsigned step_idx, step_count = 0;
        while ((1 << ++step_count) < contextSize_)
            ;

        pipeline_ = KING_PIPELINE_DEPTH;
        while (step_count % pipeline_) {
            --pipeline_;
        }

        PCX_KING_PRINT("Registering user memory \n");
        /* Register the user's buffers */
        for (int buf_idx = 0; buf_idx < ptrs_.size(); buf_idx++) {
            mem_.usr_vec.push_back(
                new UsrMem(ptrs_[buf_idx], bytes_, ibv_ctx_));
        }
        PCX_KING_PRINT("UMR started \n");
        mem_.umr_mem = new UmrMem(mem_.usr_vec, ibv_ctx_);
        PCX_KING_PRINT("UMR finished \n");

        int mem_type = PCX_MEMORY_TYPE_MEMIC;
        mem_type = PCX_MEMORY_TYPE_HOST;

        mem_.tmpMem = new PipeMem(bytes_, pipeline_, ibv_ctx_, mem_type);
    }

    void connect_and_prepare() {
        int inputs = ptrs_.size();
        unsigned step_idx, step_count = 0;
        while ((1 << ++step_count) < contextSize_)
            ;

        PCX_KING_PRINT("step_count=%d \n", step_count);

        VerbCtx *ctx = (this->ibv_ctx_);

        PCX_KING_PRINT("Locking the IB verbs context mtx \n");
        this->ibv_ctx_->mtx.lock();

        rd_.graph = new CommGraph();
        CommGraph *sess = rd_.graph;

        // Create a single management QP
        rd_.mqp = new ManagementQp(this->ibv_ctx_);
        sess->regQp(rd_.mqp);

        PCX_KING_PRINT("Created Management QP \n");

        // Step #2: Register existing memory buffers with UMR
        register_memory();

        // Create a loopback QP
        rd_.lqp = new LoopbackQp(this->ibv_ctx_);
        sess->regQp(rd_.lqp);
        LoopbackQp *lqp = rd_.lqp;
        PCX_KING_PRINT("Created Loopback QP \n");

        rd_.result = new HostMem(bytes_, ibv_ctx_);

        rd_.peers_cnt = step_count;
        rd_.peers = new rd_peer_t[step_count];
        if (!rd_.peers) {
            throw "malloc failed";
        }
        /* Establish a connection with each peer */

        for (step_idx = 0; step_idx < step_count; step_idx++) {
            /* calculate the rank of each peer */
            int leap = 1 << step_idx;
            if ((contextRank_ % (leap << 1)) >= leap) {
                leap *= -1;
            }
            uint32_t mypeer = contextRank_ + leap;

            rd_.peers[step_idx].incoming_buf = new RefMem(mem_.tmpMem->next());

            rd_.peers[step_idx].qp =
                new DoublingQp(this->ibv_ctx_, &p2p_exchange, comm_, mypeer,
                               tag_, rd_.peers[step_idx].incoming_buf);
            sess->regQp(rd_.peers[step_idx].qp);
            PCX_KING_PRINT("Creating RC QP - Done \n");
            Iov umr_iov{rd_.result, rd_.peers[step_idx].incoming_buf};
            rd_.peers[step_idx].outgoing_buf = new UmrMem(umr_iov, ibv_ctx_);
        }
        sess->reduce_write(lqp, mem_.umr_mem, rd_.result, inputs,
                           MLX5DV_VECTOR_CALC_OP_ADD,
                           MLX5DV_VECTOR_CALC_DATA_TYPE_FLOAT32, false);
        sess->wait(lqp, false);
        for (step_idx = 0; step_idx < step_count; step_idx++) {
            if (step_idx >= pipeline_) {
                sess->wait(rd_.peers[step_idx].qp,
                           false); // Wait to receive credits.
            }
            // Send the data from "result" buffer to the peer and wait for the
            // peer to also send his data to this rank's buffer.
            sess->write(rd_.peers[step_idx].qp, rd_.result,
                        true); // Send (write) the data to peer's buffer and
                               // requsting a completion for the sending side.
            sess->wait(
                rd_.peers[step_idx].qp,
                false); // Wait for the peer to send his data to this rank //
                        // TODO: First need to wait for the send and then need
                        // to wait for the receive. Or maybe even better,
                        // because of symmetry, in case this ranks sends data to
                        // the peer, the peer also sends it data to this rank,
                        // so no need to use "write with completion", and no
                        // need to wait for the "send" to complete.
            sess->wait(rd_.peers[step_idx].qp,
                       true); // Wait for this rank to finish sending it's data
                              // to the peer.

            sess->reduce_write(lqp, rd_.peers[step_idx].outgoing_buf,
                               rd_.result, 2, MLX5DV_VECTOR_CALC_OP_ADD,
                               MLX5DV_VECTOR_CALC_DATA_TYPE_FLOAT32, false);
            sess->wait(lqp, false);
            sess->send_credit(
                rd_.peers[(step_idx + pipeline_) % step_count].qp);
        }
        for (uint32_t buf_idx = 0; buf_idx < inputs; buf_idx++) {
            sess->write(lqp, rd_.result, mem_.usr_vec[buf_idx], false);
        }
        sess->wait(lqp, false);
        for (step_idx = 0; step_idx < pipeline_; step_idx++) {
            sess->wait(rd_.peers[step_idx].qp, false);
        }
        PCX_KING_PRINT("Graph building - Done \n");
        sess->finish();

        this->ibv_ctx_->mtx.unlock();

        PCX_KING_PRINT("connect_and_prepare DONE \n");
    }

    void debug_write_input() {
#ifdef VALIDITY_CHECK
        for (int i = 0; i < ptrs_.size(); ++i) {
            // fprintf(stderr, "Input %d:\n",i);
            float *buf = (float *)ptrs_[i];
            for (int k = 0; k < count_; ++k) {
                buf[k] = ((float)k + i) + contextRank_ + mone_;
            }
            // print_values(buf, count_);
        }
#endif
    }

    void debug_hang_report(uint64_t &count) {
#ifdef HANG_REPORT

        unsigned step_count = 0;
        while ((1 << ++step_count) < contextSize_)
            ;

        if (count == 1000000000) {
            fprintf(stderr, "iteration: %d\n", mone_);
            fprintf(stderr, "poll cnt: %d\n", rd_.lqp->qp->get_poll_cnt());
            fprintf(stderr, "management qp: \n");
            rd_.graph->mqp->print();
            fprintf(stderr, "loopback qp: \n");
            rd_.lqp->print();
            for (int k = 0; k < step_count; ++k) {
                fprintf(stderr, "rc qp %d: \n", k);
                rd_.peers[k].qp->print();
            }
            fprintf(stderr, "\n\n\n");
        }

#endif
    }

    void debug_check_output() {
#ifdef VALIDITY_CHECK

        unsigned step_count = 0;
        while ((1 << ++step_count) < contextSize_)
            ;

        for (int i = 0; i < ptrs_.size(); ++i) {
            // fprintf(stderr, "Output %d:\n",i);
            int err = 0;
            float *buf = (float *)ptrs_[i];
            // print_values(buf, count_);
            for (int k = 0; k < count_; ++k) {
                int expected_base =
                    ((k + mone_) * 2 + ptrs_.size() - 1) * ptrs_.size() / 2;
                int expected_max =
                    ((k + mone_ + contextSize_ - 1) * 2 + ptrs_.size() - 1) *
                    ptrs_.size() / 2;
                float expected_result =
                    (float)(expected_base + expected_max) * contextSize_ / 2;
                float result = buf[k];
                if (result != expected_result) {
                    fprintf(
                        stderr,
                        "ERROR: In Iteration %d\n expected: %.2f, got: %.2f\n",
                        mone_, expected_result, result);
                    for (int i = 0; i < ptrs_.size(); ++i) {
                        fprintf(stderr, "Input %d:\n", i);
                        float buf[count_];
                        for (int k = 0; k < count_; ++k) {
                            buf[k] = ((float)k + i) + contextRank_ + mone_;
                        }
                        print_values(buf, count_);
                    }
                    for (int i = 0; i < step_count; ++i) {
                        fprintf(stderr, "Incoming %d:\n", i);
                        float *buf = (float *)((void *)rd_.peers[i]
                                                   .incoming_buf->sg()
                                                   ->addr);
                        print_values(buf, count_);
                    }
                    fprintf(stderr, "Output %d:\n", i);
                    print_values(buf, count_);
                    // err = 1;
                    break;
                }
            }
            if (err) {
                break;
            }
        }
#endif
    }

  protected:
    // Vector of required elements to reduce.
    // Assume ptrs_ vector is of size N.
    // Each element in the ptrs_ vector is of type T* (pointer to an array).
    // Every T* element is a pointer to a vector/array with count_ elements.
    // and every element is of type T.
    // The ptrs_ vector is initialized in the constructor.
    //
    // The ptrs_ vector can be visualized as follows:
    //
    //    ptrs[0] , ptrs[1] , ptrs[2] , ... , ptrs[N-1]
    //
    // The ptrs_[i] can be visualized as follows (assume I = count_):
    //
    //    ptrs[i]:   Ti[0] , Ti[1] , Ti[2] , ... , Ti[count_-1]
    //
    // Where every Ti[j] element is an element of type T.
    // Finally, ptrs_ vector can be visualized as follows:
    //
    //   {     ptrs_[0]      },{         ptrs_[1]  },...,{      ptrs_[N-1] }
    //    [T0[0],...,T0[I-1]] , [T1[0],...,T1[I-1]] ,...,
    // [TN_1[0],...,TN_1[I-1]]
    //
    // The ptrs_ vector can be seen as a matrix with dimentions of ptrs_.size()
    // raws and count_ columns. Each cell [i][j] in the matrix is of type T.
    // Each raw in the matrix contains a single element in the ptrs_ vector and
    // every column represents the element of type T
    //
    // The reduce result can be presented easly via the matrix view.
    // The reduce operation is performed on the column, meaning for every column
    // j
    // all the raws 0 to N-1 are reduced and the same result stored in all the
    // raws of column j.
    std::vector<T *> ptrs_;

    // Number of elements in each element of the ptrs_ vector.
    // Notice that every element in the ptrs_ vector may be also a vector/array)
    // that will be reduced.
    // The count_ variable is initialized in the constructor.
    const int count_;

    // Total amount of bytes of all elements in a single element in ptrs_
    // vector. Initialized in the constructor. For example if a single ptrs_
    // element is a vector of 5 elements each of size T, then bytes_ will be
    // equal to 5*size_in_bytes(T).
    const int bytes_;

    // The reduction function to use when performing the reduce operation.
    // Initialized in the constructor.
    // const ReductionFunction<T> *fn_; // TODO: Currently not in use. Need to
    // start using it...

    VerbCtx *ibv_ctx_;

    typedef struct mem_registration {
        Iov usr_vec;
        UmrMem *umr_mem;
        PipeMem *tmpMem;
    } mem_registration_t;

    mem_registration_t mem_;

    class rd_peer_t {
      public:
        rd_peer_t() : outgoing_buf(NULL), incoming_buf(NULL){};
        ~rd_peer_t() {
            delete (qp);
            delete (this->incoming_buf);
            delete (this->outgoing_buf);
        };
        DoublingQp *qp;
        NetMem *outgoing_buf;
        NetMem *incoming_buf;
    };

    typedef struct rd_connections {
        NetMem *result;

        CommGraph *graph;
        ManagementQp *mqp; // mqp stands for "Management Queue Pair"
        LoopbackQp *lqp;

        unsigned peers_cnt;

        rd_peer_t *peers; // Pointer to array of rd_peer_t
    } rd_connections_t;

    rd_connections_t rd_;
    int mone_;
    int pipeline_;

  private:
    // The rank ID of the process
    const int contextRank_;

    // Total number of ranks within the communicators group.
    const int contextSize_;

    // The struct that hold the communicator structure.
    void *comm_;

    // Used for out of band QPs exchange
    uint32_t tag_;
};
