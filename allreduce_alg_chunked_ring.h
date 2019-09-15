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


template <typename T>
class PcxAllreduceChunkedRing {
  public:

  PcxAllreduceChunkedRing(
    const int contextSize,
    const int contextRank,
    const std::vector<T *> &ptrs,
    const int count) // TODO: Need to add as argument also ReductionFunction/Type.
    : contextSize_(contextSize),
      contextRank_(contextRank),
      ptrs_(ptrs),
      count_(count),
      bytes_(count_ * sizeof(T)),
      pieceSize_(bytes_ / contextSize) {
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
    delete (rd_.pqp);
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
    debug_check_output();
    ++mone_;
    PCX_RING_PRINT("[%d] Done running PcxRingAllReduce \n", contextRank_);
  }

  void connect_and_prepare() {
    // TODO: Copy the connect_and_prepare() function after getting rid of nextSlot() or context_
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
    rd_.pqp->right->print();
    fprintf(stderr, "=======================================\n");
    fprintf(stderr, "Left QP: Run #%d: %s\n", mone_, msg);
    fprintf(stderr, "=======================================\n");
    rd_.pqp->left->print();
#endif // HANG_REPORT
  }

  // Debug function // TODO: Make this function private!
  void debug_check_output()
  {
#ifdef VALIDITY_CHECK
    unsigned step_count = 0;
    while ((1 << ++step_count) < contextSize_)
      ;

    for (int i = 0; i < ptrs_.size(); ++i)
    {
      // fprintf(stderr, "Output %d:\n",i);
      int err = 0;
      float *buf = (float *)ptrs_[i];
      // print_values(buf, count_);
      for (int k = 0; k < count_; ++k)
      {
        int expected_base =
            ((k + mone_) * 2 + ptrs_.size() - 1) * ptrs_.size() / 2;
        int expected_max =
            ((k + mone_ + contextSize_ - 1) * 2 + ptrs_.size() - 1) *
            ptrs_.size() / 2;
        float expected_result =
            (float)(expected_base + expected_max) * contextSize_ / 2;
        float result = buf[k];
        if (result != expected_result)
        {
          fprintf(stderr,
                  "ERROR: In Iteration %d\n expected: %.2f, got: %.2f\n", mone_,
                  expected_result, result);
          for (int i = 0; i < ptrs_.size(); ++i)
          {
            fprintf(stderr, "Input %d:\n", i);
            float buf[count_];
            for (int k = 0; k < count_; ++k)
            {
              buf[k] = ((float)k + i) + contextRank_ + mone_;
            }
            print_values(buf, count_);
          }
          mem_.tmpMem->print();
          fprintf(stderr, "Output %d:\n", i);
          print_values(buf, count_);
          // err = 1;
          break;
        }
      }
      if (err)
      {
        break;
      }
    }
#endif // VALIDITY_CHECK
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

  class RingPair { // TODO: Move to new file pcx_ring.h
  public:
    RingPair(CommGraph *cgraph, p2p_exchange_func func, void *comm, // TODO: Move to some "ring algorithms qps" file
             uint32_t myRank, uint32_t commSize, uint32_t tag1,
             uint32_t tag2, PipeMem *incoming, VerbCtx *ctx) {
      uint32_t rightRank = (myRank + 1) % commSize;
      uint32_t leftRank = (myRank - 1 + commSize) % commSize;
  
      if (myRank % 2) { // Odd rank
        this->right = new RingQp(ctx, func, comm, rightRank, tag1, incoming);
        cgraph->regQp(this->right);
        this->left = new RingQp(ctx, func, comm, leftRank, tag2, incoming);
        cgraph->regQp(this->left);
      } else { // Even rank
        this->left = new RingQp(ctx, func, comm, leftRank, tag1, incoming);
        cgraph->regQp(this->left);
        this->right = new RingQp(ctx, func, comm, rightRank, tag2, incoming);
        cgraph->regQp(this->right);
      }
      right->set_pair(left);
      left->set_pair(right);
    }
  
    ~RingPair() {
      delete (right);
      delete (left);
    }
  
    RingQp *right;
    RingQp *left;
  };

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
    RingPair *pqp;     // pqp stands for "Pair Queue Pair"

    // Holds the number of iterations that will be executed during the All-Reduce
    // algorithm
    unsigned iters_cnt;

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

};
