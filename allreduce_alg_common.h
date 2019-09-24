// TODO: Add license
#pragma once

#include "third-party/pcx/pcx_mem.h" // TODO: Fix path
#include "third-party/pcx/qps.h" // TODO: Fix path
#include "third-party/pcx/verbs_ctx.h" // TODO: Fix path

// TODO: copy from Gloo/algorithm.h all the code regarding ReductionType etc.

typedef std::vector<PipeMem *> Iop;
typedef Iop::iterator Iopit;

typedef std::vector<NetMem *> Iov;
typedef Iov::iterator Iovit;

void freeIov(Iov &iov);
void freeIop(Iop &iop);


