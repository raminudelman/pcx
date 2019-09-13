// TODO: Add license
#pragma once

#include "third-party/pcx/pcx_mem.h"
#include "third-party/pcx/qps.h"

typedef std::vector<PipeMem *> Iop;
typedef Iop::iterator Iopit;

typedef std::vector<NetMem *> Iov;
typedef Iov::iterator Iovit;

void freeIov(Iov &iov);
void freeIop(Iop &iop);


