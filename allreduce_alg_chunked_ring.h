// TODO: Add liscense 

#pragma once

#include "allreduce_alg_common.h"

#ifdef DEBUG
#define PCX_RING_PRINT(args...)                                                \
  if (contextRank_ == 0) {                                                     \
    fprintf(stderr, "(%s: %d) in function %s: ", __FILE__, __LINE__, __func__);\
    fprintf(stderr, args);                                                     \
  };
#else
#define PCX_RING_PRINT(args...)
#endif

#define RING_PIPELINE_DEPTH 1

// Performs data exchange between peers in ring.
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
//    recv_buf: The buffer that recieve data from the rank with
//              rank id equals to 'peer'.
//
int ring_exchange(void *comm, volatile void *send_buf_left, volatile void *send_buf_right, volatile void *recv_buf_left,
                  volatile void *recv_buf_right, size_t size, uint32_t peer_left, uint32_t peer_right, uint32_t tag1, uint32_t tag2);

template <typename T>
class PcxAllreduceChunkedRing {
  public:

  PcxAllreduceChunkedRing(
    const int contextSize,
    const int contextRank,
    const std::vector<T *> &ptrs,
    const int count,
    uint32_t tag1,
    uint32_t tag2,
    void* comm) // TODO: Need to add as argument also ReductionFunction/Type.
    : contextSize_(contextSize),
      contextRank_(contextRank),
      ptrs_(ptrs),
      count_(count),
      bytes_(count_ * sizeof(T)),
      pieceSize_(bytes_ / contextSize),
      tag1(tag1),
      tag2(tag2),
      comm(comm) {
    PCX_RING_PRINT("Initializing PcxAllreduceRing \n");
    // In case the communicator is of size 1,
    // No need to reduce the ptrs vector, because
    // it's already reduced. The reduced result is
    // the first element in the ptrs vector (ptrs[0])
    if (this->contextSize_ == 1) {
      return;
    }

    // PCX performs the elements reduction on the NIC using Vector-CALC.
    // The reduction is on the number of elements in ptrs and another element
    // that is the result from a peer rank
    if ((ptrs.size() + 1) > MAX_LOCAL_VECTOR_SIZE_TO_REDUCE) {
      fprintf(stderr, "PCX does not support more than %d to be reduced on the NIC", MAX_LOCAL_VECTOR_SIZE_TO_REDUCE);
    }
    // Step #1:
    // Initialize verbs context (choose IB device, open PD, etc.)
    ibv_ctx_ = VerbCtx::getInstance();
    PCX_RING_PRINT("Verbs context initiated \n");
    
    // Step #2 & #3:  // TODO: Improve the comment/documentation
    // Connect to the (recursive-doubling)
    // iters and pre-post operations
    connect_and_prepare();
    mone_ = 0;
  }

  ~PcxAllreduceChunkedRing() {
    PCX_RING_PRINT("Freeing UMR and freeing user memory \n");
    delete (rd_.mqp);
    delete (rd_.lqp);
    delete (rd_.graph);
    delete(rd_.ring_qps);
    delete[](rd_.iters);
    // Deregister memory
    delete (mem_.tmpMem);
    freeIop(mem_.usr_vec);
    VerbCtx::remInstance();
  }

  void run() {
    debug_write_input();
    debug_hang_report("Start");
    rd_.graph->mqp->qp->db(); // TODO: graph has db() API function. Use it! mpq should not be accessed!
    debug_hang_report("After Doorbell");
    // Calling the rearm after the collective operation started (using the
    // DoorBell) makes the rearm process to run in parallel with the
    // collective algorithm.
    rd_.graph->mqp->qp->rearm();
    debug_hang_report("After ReArm");
    int res = 0;
    uint64_t count = 0;
    while (res == 0)
    {
      res = rd_.lqp->qp->poll();
      ++count;
      debug_hang_report("Stuck", count);
    }
    ++mone_;
    PCX_RING_PRINT("[%d] Done running PcxRingAllReduce \n", contextRank_);
  }

  void connect_and_prepare() {
    // The number of vectors to reduce.
    // Each vector has count_ elements.
    int vectors_to_reduce = ptrs_.size();

    // step_count holds the number of iterations that the ring algorithm should
    // perform in reduce-scatter stage, and in all-gather stage.
    // During reduce-scatter it will perform step_count iterations,
    // and in all-gather stage it will perform additional step_count iterations.
    unsigned step_count = contextSize_;

    // First we lock the verbs context so not other thread will be able to
    // access it until we finish the whole "graph building" process.
    // The lock should be released after the whole graph was built and
    // "finalized".
    ibv_ctx_->mtx.lock();

    rd_.graph = new CommGraph();
    CommGraph *sess = rd_.graph;

    // Create a single management QP
    rd_.mqp = new ManagementQp(ibv_ctx_);
    sess->regQp(rd_.mqp);

    PCX_RING_PRINT("Created management QP \n");

    // Step #2: Register existing memory buffers with UMR

    // Register (ibv_reg_mr) the users data buffers.
    for (int i = 0; i < vectors_to_reduce; i++)
    {
      mem_.usr_vec.push_back(new PipeMem((void *)ptrs_[i], pieceSize_,
                                         (size_t)contextSize_, ibv_ctx_));
    }

    int temp_type = PCX_MEMORY_TYPE_MEMIC;
    temp_type = PCX_MEMORY_TYPE_HOST; // CHECK: Why is this patch needed? Why MEMIC cannot be used? MEMIC worked but during debug this code was added to prevent other bugs or compilcations. MEMIC is not that important and did not help performance too much because after some point the MEMIC region is not enought and it wil fall back to host mem anyway.

    pipeline_ = RING_PIPELINE_DEPTH;

    // Find the maximal pipeline which devides the communicator
    // size without a reminder
    while (contextSize_ % pipeline_) {
      --pipeline_;
    }
    pipeline_ = contextSize_ * 2; // TODO: What is this? it overrides the loop that was before!!

    // The tmpMem will be used for "incoming" messages from the qps, this buffer is of size pipeline and each element in the buffer is with size peicesize.
    mem_.tmpMem = new PipeMem(pieceSize_, pipeline_, ibv_ctx_, temp_type);

    // Create a loopback QP - used for DMA inside the container itself.
    // Instead of using MemCpy and CudaMemCopy, the memory is copied via the NIC.
    rd_.lqp = new LoopbackQp(ibv_ctx_);
    sess->regQp(rd_.lqp);
    LoopbackQp *lqp = rd_.lqp;
    PCX_RING_PRINT("Loopback created \n");

    // Establish a connection with each peer
    uint32_t myRank = contextRank_;

    rd_.ring_qps = new RingQps(&ring_exchange, comm, myRank, contextSize_, tag1, tag2, mem_.tmpMem, ibv_ctx_);
    RingQp *right = rd_.ring_qps->getRightQp();
    RingQp *left = rd_.ring_qps->getLeftQp();

    sess->regQp(rd_.ring_qps);

    PCX_RING_PRINT("RC ring QPs created \n");

    // Allocating a data structure for every step in the algorithm.
    // The data structure will hold all the required data buffers for the
    // step in the algorithm
    rd_.iters = new StepCtx[contextSize_];
    if (!rd_.iters) {
      throw "malloc failed";
    }

    // For every step in the ring algorithm, we create a single umr vector
    // that combines all the corresponding pieces from the usr_vec.
    for (unsigned step_idx = 0; step_idx < step_count; step_idx++) {
      size_t piece = (contextSize_ + myRank - step_idx) % contextSize_;
      for (int k = 0; k < vectors_to_reduce; ++k) {
        rd_.iters[step_idx].umr_iov.push_back(new RefMem((*mem_.usr_vec[k])[piece]));
      }
      if (step_idx > 0) {
        rd_.iters[step_idx].umr_iov.push_back(new RefMem(mem_.tmpMem->next())); // The next() operation is cyclic.
      }
      rd_.iters[step_idx].outgoing_buf = new UmrMem(rd_.iters[step_idx].umr_iov, ibv_ctx_);
    }
    PCX_RING_PRINT("UMR registration done \n");

    // For convenience, we will define local variables
    

    PCX_RING_PRINT("Starting All-Reduce \n");
    PCX_RING_PRINT("Starting Scatter-Reduce stage \n");

    int credits = pipeline_;

    PCX_RING_PRINT("Performed first reduce in the Reduce-Scatter stage \n");

    // The first reduce (first step in the ring algorithm)
    for (unsigned step_idx = 0; step_idx < step_count; step_idx++) {
      int num_vectors = rd_.iters[step_idx].umr_iov.size();
      if (credits == 1) {
        // reduce_write with 'require_cmpl==true' means that we perform reduce and perform RDMA write to the next rank and require a completion for the RDMA write.
        sess->reduce_write(right, rd_.iters[step_idx].outgoing_buf, step_idx, num_vectors, MLX5DV_VECTOR_CALC_OP_ADD, MLX5DV_VECTOR_CALC_DATA_TYPE_FLOAT32, true);
        sess->wait(right, true);
        sess->send_credit(left);  // Notifying the left rank that it can continue sending new data
        sess->wait(right, false); // Waiting for the rank from the right to realse a credit that mean that our rank can continue sending the data to the right.
        // Initialize number of credits
        credits = pipeline_;
      } else {
      // reduce_write with 'require_cmpl==false' means that we perform reduce and perform RDMA write to the
      // next rank. The RDMA write will send the outgoing_buf to the incoming
      // buffer (wihch is the tmpMem) of the destination rank.
        sess->reduce_write(right, rd_.iters[step_idx].outgoing_buf, step_idx, num_vectors, MLX5DV_VECTOR_CALC_OP_ADD, MLX5DV_VECTOR_CALC_DATA_TYPE_FLOAT32, false);
        --credits;
      }
      // Once a rank sent a message from the sending QP (to the rank to the right),
      // the rank knows it should wait for the message from the left QP  the
      sess->wait(left, false);
    }

    PCX_RING_PRINT("Reduce-Scatter stage done \n");

    // Done with the AllReduce-Scatter. Every rank has a peice of the final
    // result stored in it's tmpMem (in one of the slots).

    // Start the All-Gather step!!
    size_t last_frag = (step_count - 1);

    for (unsigned step_idx = 0; step_idx < step_count; step_idx++) {
      // Ref mem does not alocate new memory...
      RefMem newVal((*mem_.tmpMem)[last_frag]); // Cyclic pipe...there is wraparound

      size_t piece = (step_idx + myRank) % step_count;
      if (credits == 1) {
        sess->write(right, &newVal, step_count + step_idx, true);
        // Copying "the reduce result from ptrs[0] to all ptrs[i]"
        for (uint32_t buf_idx = 0; buf_idx < vectors_to_reduce; buf_idx++) {
          sess->write(lqp, &newVal, rd_.iters[step_idx].umr_iov[buf_idx], false);
        }
        sess->wait(right, true);
        sess->wait(lqp); //Waiting for the receive to finish in the loopback QP
        sess->send_credit(left);
        sess->wait(right); //for credit
        credits = pipeline_;
      } else {
        sess->write(right, &newVal, step_count + step_idx, false);
        for (uint32_t buf_idx = 0; buf_idx < vectors_to_reduce; buf_idx++) {
          sess->write(lqp, &newVal, rd_.iters[step_idx].umr_iov[buf_idx], false);
        }
      }
      sess->wait(left, false); //for data
      ++last_frag;
    }

    PCX_RING_PRINT("All-Gather stage done \n");

    // Making the NIC wait for the last credit although the user already got the reduce result from the lpq because in the run, we poll only the lpq.
    if (credits != pipeline_) {
      sess->send_credit(left);
      sess->wait(right);
      PCX_RING_PRINT("Returned all credits to peer \n");
    }

    PCX_RING_PRINT("Finalizing the graph\n");
    sess->finish();
    PCX_RING_PRINT("Finalized the graph\n");

    ibv_ctx_->mtx.unlock();

    PCX_RING_PRINT("Graph building stage done \n");

    PCX_RING_PRINT("connect_and_prepare DONE \n");
  }

  // Debug function // TODO: Make this function private!
  void debug_write_input()
  {
#ifdef VALIDITY_CHECK
    for (int i = 0; i < ptrs_.size(); ++i)
    {
      // fprintf(stderr, "Input %d:\n",i);
      float *buf = (float *)ptrs_[i];
      for (int k = 0; k < count_; ++k)
      {
        buf[k] = ((float)k + i) + contextRank_ + mone_;
      }
      // print_values(buf, count_);
    }
#endif // VALIDITY_CHECK
  }

  // Debug function // TODO: Make this function private!
  void debug_hang_report(std::string str, int count = 0)
  {
#ifdef HANG_REPORT
    if ((count != 0) and (count != 1000000)) {
      return;
    }
    if (contextRank_ != 0) {
      return;
    }
    char msg[str.length() + 1];
    strcpy(msg, str.c_str());

    fprintf(stderr, "======================================================\n");
    fprintf(stderr, "Run #%d: %s \n", mone_, msg);
    fprintf(stderr, "======================================================\n");
    fprintf(stderr, "Waiting for Loopback QP CQE with index %d to be completed\n", rd_.lqp->qp->get_poll_cnt());
    fprintf(stderr, "=======================================\n");
    fprintf(stderr, "Management QP: Run #%d: %s\n", mone_, msg);
    fprintf(stderr, "=======================================\n");
    rd_.mqp->print();
    fprintf(stderr, "=======================================\n");
    fprintf(stderr, "Loopback QP: Run #%d: %s\n", mone_, msg);
    fprintf(stderr, "=======================================\n");
    rd_.lqp->print();
    fprintf(stderr, "=======================================\n");
    fprintf(stderr, "Right QP: Run #%d: %s\n", mone_, msg);
    fprintf(stderr, "=======================================\n");
    //TODO: replace pqp with ring_qps
    // rd_.pqp->right->print();
    // fprintf(stderr, "=======================================\n");
    // fprintf(stderr, "Left QP: Run #%d: %s\n", mone_, msg);
    // fprintf(stderr, "=======================================\n");
    // rd_.pqp->left->print();
#endif // HANG_REPORT
  }

  void print_values(volatile float *buf, int count) { // TODO: Move to utils.cc file?
    int i = 0;
    for (i = 0; i < count; ++i) {
      if (i % 8 == 0) {
        fprintf(stderr, "\n");
      }
      fprintf(stderr, "%.1f\t", buf[i]);
    }
    fprintf(stderr, "\n");
  }

  private: 

  // The rank ID of the process
  const int contextRank_;

  // Total number of ranks within the communicators group.
  const int contextSize_;
  
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
  //   {     ptrs_[0]      },{         ptrs_[1]  },...,{      ptrs_[N-1]       }
  //    [T0[0],...,T0[I-1]] , [T1[0],...,T1[I-1]] ,..., [TN_1[0],...,TN_1[I-1]]
  //
  // The ptrs_ vector can be seen as a matrix with dimentions of ptrs_.size()
  // raws and count_ columns. Each cell [i][j] in the matrix is of type T.
  // Each raw in the matrix contains a single element in the ptrs_ vector and
  // every column represents the element of type T
  //
  // The reduce result can be presented easly via the matrix view.
  // The reduce operation is performed on the column, meaning for every column j
  // all the raws 0 to N-1 are reduced and the same result stored in all the
  // raws of column j.
  std::vector<T *> ptrs_;

  // Number of elements in each element of the ptrs_ vector.
  // Notice that every element in the ptrs_ vector may be also a vector/array)
  // that will be reduced.
  // The count_ variable is initialized in the constructor.
  const int count_;

  // Total amount of bytes of all elements in a single element in ptrs_ vector.
  // Initialized in the constructor.
  // For example if a single ptrs_ element is a vector of 5 elements each of
  // size T, then bytes_ will be equal to 5*size_in_bytes(T).
  const int bytes_;

  // The reduction function to use when performing the reduce operation.
  // Initialized in the constructor.
  //const ReductionFunction<T> *fn_; // TODO: Currently not in use. Need to start using it...

  VerbCtx *ibv_ctx_;

  typedef struct mem_registration_ring { // TODO: Convert into a class and delete from pcx_mem.h all the Iop* functions and typdefs
    // TODO: Add documentation
    Iop usr_vec;

    PipeMem *tmpMem;
  } mem_registration_ring_t;

  mem_registration_ring_t mem_;

  class StepCtx {
  public:
    StepCtx() : outgoing_buf(NULL), umr_iov() {};
    ~StepCtx() {
      delete (this->outgoing_buf);
      freeIov(umr_iov);
    };
    Iov umr_iov;          // Iov == Input/Output Vector, UMR is because the user's buffer is not contigious and we convert it to a UMR.
    NetMem *outgoing_buf; // The buffer which contains the result of the reduce and which will be sent to the peer rank
  };

  typedef struct rd_connections_ring {
    CommGraph *graph;

    ManagementQp *mqp; // mqp stands for "Management Queue Pair"
    LoopbackQp *lqp;   // lqp stands for "Loopback Queue Pair"
    RingQps *ring_qps; // pair of QP

    // Each element in the array holds all the data structure that the algorithm
    // operates on during each step of the algorithm.
    StepCtx *iters;

  } rd_connections_ring_t;

  rd_connections_ring_t rd_;

  // Counts how many times the algorithm ran (for debug reasons).
  int mone_;

  // Holds the number of data pieces (each peace is of size pieceSize_)
  // that a peer rank (in the ring) can send to this rank without waiting for
  // the rank to notify that the buffers used to recieve the data can be reused.
  int pipeline_ = RING_PIPELINE_DEPTH; // TODO: Consider converting into 'static constexpr int'

  // The size of each chunk that will be moved through
  // ring throughout the run of the algorithm
  size_t pieceSize_;

  // Used for out of band QPs exchange
  uint32_t tag1;
  uint32_t tag2;

  // The struct that hold the communicator structure.
  void* comm;

};
