#ifndef TERRA_WLD_GUARD_TASK_H
#define TERRA_WLD_GUARD_TASK_H

#include "terra_types.h"

typedef struct TxWldGuardTask {
    const uint8_t* data;
    uint32_t length;
    uint32_t offset;
    uint32_t index;
    uint32_t count;
    uint32_t sub_index;
    uint32_t sub_count;
    uint16_t legacy_slots;
    uint8_t stage;
    uint8_t initialized;
    uint8_t finished;
} TxWldGuardTask;

int tx_wld_guard_task_begin(TxWorld* world, TxWldGuardTask* task);
int tx_wld_guard_task_step(TxWorld* world, TxWldGuardTask* task, uint32_t record_budget);
uint32_t tx_wld_guard_task_progress(const TxWldGuardTask* task);

#endif
