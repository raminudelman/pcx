/*
 * Copyright (c) 2019-present, Mellanox Technologies Ltd. 
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

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
