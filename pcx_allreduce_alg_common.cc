// TODO: Add license
#include "pcx_allreduce_alg_common.h"

void freeIov(Iov &iov) {
    for (Iovit it = iov.begin(); it != iov.end(); ++it) {
        delete (*it);
    }
    iov.clear();
}

void freeIop(Iop &iop) {
    for (Iopit it = iop.begin(); it != iop.end(); ++it) {
        delete (*it);
    }
    iop.clear();
}