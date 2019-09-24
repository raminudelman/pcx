// TODO: Add license
#pragma once

#include "pcx_mem.h"
#include "qps.h"
#include "verbs_ctx.h"
#include "comm_graph.h"

// TODO: copy from Gloo/algorithm.h all the code regarding ReductionType etc.

typedef std::vector<PipeMem *> Iop;
typedef Iop::iterator Iopit;

typedef std::vector<NetMem *> Iov;
typedef Iov::iterator Iovit;

void freeIov(Iov &iov);
void freeIop(Iop &iop);


