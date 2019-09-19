#include "comm_graph.h"

CommGraph::CommGraph() : mqp(NULL), iq() {
}

CommGraph::~CommGraph() { 
}

void CommGraph::regQp(ManagementQp *qp) {
  this->mqp = qp;
  regQpCommon(qp);
  QP_PRINT("Registered ManagementQp with ID='%d' \n", qp->id);
}

void CommGraph::regQp(LoopbackQp *qp) {
  LambdaInstruction lambda = this->mqp->cd_recv_enable(qp);
  this->enqueue(lambda);
  regQpCommon(qp);
  QP_PRINT("Registered LoopbackQp with ID='%d' \n", qp->id);
}

void CommGraph::regQp(DoublingQp *qp) {
  LambdaInstruction lambda = this->mqp->cd_recv_enable(qp);
  this->enqueue(lambda);
  regQpCommon(qp);
  QP_PRINT("Registered DoublingQp with ID='%d' \n", qp->id);
}

void CommGraph::regQp(RingQp *qp) {
  LambdaInstruction lambda = this->mqp->cd_recv_enable(qp);
  this->enqueue(lambda);
  regQpCommon(qp);
  QP_PRINT("Registered RingQp with ID='%d' \n", qp->id);
}

void CommGraph::regQpCommon(PcxQp *qp) {
  this->objects.push_back(qp);
  qp->id = this->objects.size();
}

void CommGraph::enqueue(LambdaInstruction &ins) { 
  iq.push(std::move(ins)); 
}

void CommGraph::wait(PcxQp *slave_qp, bool wait_scq) { 
  LambdaInstruction lambda = mqp->cd_wait(slave_qp, wait_scq);
  QP_PRINT("Add graph operation: ManagementQP        will 'wait'             on QP   ID='%d' on SQ? %d. \n", slave_qp->id, wait_scq);
  this->enqueue(lambda);
}

void CommGraph::reduce_write(RingQp *slave_qp, NetMem *local, size_t pos, uint16_t num_vectors, uint8_t op, uint8_t type, bool require_cmpl) {
  LambdaInstruction lambda = slave_qp->reduce_write(local, pos, num_vectors, op, type, require_cmpl);
  QP_PRINT("Add graph operation: RingQP      ID='%d' will 'reduce and write' to peer ID=%d to position = %lu \n", slave_qp->id, slave_qp->get_pair()->id, pos);
  this->enqueue(lambda);
  lambda = this->mqp->cd_send_enable(slave_qp);              
  this->enqueue(lambda);
}
void CommGraph::reduce_write(DoublingQp *slave_qp, NetMem *local, NetMem *remote, uint16_t num_vectors, uint8_t op, uint8_t type, bool require_cmpl) {
  LambdaInstruction lambda = slave_qp->reduce_write(local, remote, num_vectors, op, type, require_cmpl);
  this->enqueue(lambda);
  QP_PRINT("Add graph operation: DoublingQp  ID='%d' will 'reduce and write' to peer ID=%d\n", slave_qp->id, slave_qp->get_pair()->id);
  lambda = this->mqp->cd_send_enable(slave_qp);
  this->enqueue(lambda);
}
void CommGraph::reduce_write(LoopbackQp *slave_qp, UmrMem *local, NetMem *remote, uint16_t num_vectors, uint8_t op, uint8_t type, bool require_cmpl) {
  LambdaInstruction lambda = slave_qp->reduce_write(local, remote, num_vectors, op, type, require_cmpl);
  this->enqueue(lambda);
  QP_PRINT("Add graph operation: LoopbackQp  ID='%d' will 'reduce and write' to peer ID=%d\n", slave_qp->id, slave_qp->get_pair()->id);
  lambda = this->mqp->cd_send_enable(slave_qp);
  this->enqueue(lambda);
}
void CommGraph::reduce_write(LoopbackQp *slave_qp, NetMem *local, NetMem *remote, uint16_t num_vectors, uint8_t op, uint8_t type, bool require_cmpl) {
  LambdaInstruction lambda = slave_qp->reduce_write(local, remote, num_vectors, op, type, require_cmpl);
  this->enqueue(lambda);
  QP_PRINT("Add graph operation: LoopbackQp  ID='%d' will 'reduce and write' to peer ID=%d\n", slave_qp->id, slave_qp->get_pair()->id);
  lambda = this->mqp->cd_send_enable(slave_qp);
  this->enqueue(lambda);
}
void CommGraph::write(RingQp *slave_qp, NetMem *local, size_t pos, bool require_cmpl) {
  LambdaInstruction lambda = slave_qp->write(local, pos, require_cmpl);
  this->enqueue(lambda);
  QP_PRINT("Add graph operation: RingQp      ID='%d' will 'reduce and write' to peer ID=%d to position = %lu \n", slave_qp->id, slave_qp->get_pair()->id, pos);

  lambda = this->mqp->cd_send_enable(slave_qp);
  this->enqueue(lambda);
}
void CommGraph::write(LoopbackQp *slave_qp, NetMem *local, RefMem *remote, bool require_cmpl) {
  LambdaInstruction lambda = slave_qp->write(local, remote, require_cmpl);
  this->enqueue(lambda);
  QP_PRINT("Add graph operation: LoopbackQp  ID='%d' will 'write'            to peer ID=%d \n", slave_qp->id, slave_qp->get_pair()->id);
  lambda = this->mqp->cd_send_enable(slave_qp);
  this->enqueue(lambda);
}
void CommGraph::write(LoopbackQp *slave_qp, NetMem *local, NetMem *remote, bool require_cmpl) {
  LambdaInstruction lambda = slave_qp->write(local, remote, require_cmpl);
  this->enqueue(lambda);
  QP_PRINT("Add graph operation: LoopbackQp  ID='%d' will 'write'            to peer ID=%d \n", slave_qp->id, slave_qp->get_pair()->id);
  lambda = this->mqp->cd_send_enable(slave_qp);
  this->enqueue(lambda);
}
void CommGraph::write(DoublingQp *slave_qp, NetMem *local, NetMem *remote, bool require_cmpl) {
  LambdaInstruction lambda = slave_qp->write(local, remote, require_cmpl);
  this->enqueue(lambda);
  QP_PRINT("Add graph operation: DoublingQp  ID='%d' will 'write'            to peer ID=%d \n", slave_qp->id, slave_qp->get_pair()->id);
  lambda = this->mqp->cd_send_enable(slave_qp);
  this->enqueue(lambda);
}
void CommGraph::write(DoublingQp *slave_qp, NetMem *local, bool require_cmpl) {
  LambdaInstruction lambda = slave_qp->write(local, require_cmpl);
  this->enqueue(lambda);
  QP_PRINT("Add graph operation: DoublingQp  ID='%d' will 'write'            to peer ID=%d \n", slave_qp->id, slave_qp->get_pair()->id);
  lambda = this->mqp->cd_send_enable(slave_qp);
  this->enqueue(lambda);
}

void CommGraph::send_credit(TransportQp *slave_qp) {
  LambdaInstruction lambda = slave_qp->send_credit();
  this->enqueue(lambda);
  QP_PRINT("Add graph operation: TransportQP ID='%d' will 'send_credit'      to peer ID=%d \n", slave_qp->id, slave_qp->get_pair()->id);
  lambda = this->mqp->cd_send_enable(slave_qp);
  this->enqueue(lambda);
}

void CommGraph::db() { 
  mqp->qp->db(); 
}

void CommGraph::finish() {
  for (auto it = objects.begin(); it != objects.end(); ++it) {
    (*it)->init();
  }
  QP_PRINT("Starting to write WQEs \n");
  while (!iq.empty()) {
    LambdaInstruction &instruction = iq.front();
    instruction();
    iq.pop();
  }
  QP_PRINT("Finalizing \n");
  for (auto it = objects.begin(); it != objects.end(); ++it) {
    (*it)->fin();
  }
  QP_PRINT("READY! \n");
}