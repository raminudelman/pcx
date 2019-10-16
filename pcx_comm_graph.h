#pragma once

#include "pcx_qps.h"
#include "pcx_ring_qp.h"
#include "pcx_king_qp.h"

// Communication Graph class.
// Holds all the communication graph of a collective operation.
// The Communication Graph holds a management QP which acts as a
// management unit which in charge of executing all the operation that
// where defined in advance.
class CommGraph {
  public:
    CommGraph();
    ~CommGraph();

    void enqueue(LambdaInstruction &ins); // TODO: Move to private section of
                                          // the class

    // Register a QP to the graph.
    // Each graph should have a single Management QP and single/multiple
    // Transport QPs
    void regQp(ManagementQp *qp);
    void regQp(LoopbackQp *qp);
    void regQp(DoublingQp *qp);
    void regQp(RingQp *qp);
    void regQp(RingQps *qps);

    void wait(PcxQp *slave_qp, bool wait_scq = false);

    void reduce_write(RingQp *slave_qp, NetMem *local, size_t pos,
                      uint16_t num_vectors, uint8_t op, uint8_t type,
                      bool require_cmpl);
    void reduce_write(DoublingQp *slave_qp, NetMem *local, NetMem *remote,
                      uint16_t num_vectors, uint8_t op, uint8_t type,
                      bool require_cmpl);
    void reduce_write(LoopbackQp *slave_qp, UmrMem *local, NetMem *remote,
                      uint16_t num_vectors, uint8_t op, uint8_t type,
                      bool require_cmpl);
    void reduce_write(LoopbackQp *slave_qp, NetMem *local, NetMem *remote,
                      uint16_t num_vectors, uint8_t op, uint8_t type,
                      bool require_cmpl);

    void write(RingQp *slave_qp, NetMem *local, size_t pos, bool require_cmpl);
    void write(LoopbackQp *slave_qp, NetMem *local, RefMem *remote,
               bool require_cmpl);
    void write(LoopbackQp *slave_qp, NetMem *local, NetMem *remote,
               bool require_cmpl);
    void write(DoublingQp *slave_qp, NetMem *local, NetMem *remote,
               bool require_cmpl);
    void write(DoublingQp *slave_qp, NetMem *local, bool require_cmpl);

    void send_credit(TransportQp *slave_qp);

    void db();

    void finish();

    // mqp stands for "Management Queue Pair"
    ManagementQp *mqp;

    // Instructions queue
    InsQueue iq;

    std::vector<GraphObj *> objects;
    // TODO: Cannot change to vector of TransportQps because ManagementQp is
    // also registered... need to seprate it from the list?

  private:
    void regQpCommon(GraphObj *obj);
};
