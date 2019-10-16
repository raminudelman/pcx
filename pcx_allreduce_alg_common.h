// TODO: Add license
#pragma once

#include "pcx_mem.h"

typedef std::vector<PipeMem *> Iop;
typedef Iop::iterator Iopit;

typedef std::vector<NetMem *> Iov;
typedef Iov::iterator Iovit;

void freeIov(Iov &iov);
void freeIop(Iop &iop);
