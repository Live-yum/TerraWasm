#ifndef TERRA_RENDER_TASK_H
#define TERRA_RENDER_TASK_H

#include "terra_types.h"

typedef struct TxOpenPreviewTask {
    uint8_t* rgba;
    uint32_t preview_width;
    uint32_t preview_height;
    uint32_t stride;

    uint32_t source_width;
    uint32_t source_height;
    double ground;
    double rock;

    uint32_t tile_start;
    uint32_t tile_end;
    uint32_t offset;
    uint32_t x;
    uint32_t y;

    uint8_t initialized;
    uint8_t finished;
} TxOpenPreviewTask;

int tx_open_preview_begin(TxWorld* world, TxOpenPreviewTask* task);
int tx_open_preview_step(TxWorld* world, TxOpenPreviewTask* task, uint32_t record_budget);
uint32_t tx_open_preview_progress(const TxOpenPreviewTask* task);
int32_t tx_open_preview_finish_png(TxOpenPreviewTask* task);
void tx_open_preview_discard(TxOpenPreviewTask* task);

#endif
