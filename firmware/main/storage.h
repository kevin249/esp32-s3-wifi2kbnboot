#pragma once
#include <stdbool.h>
void storage_init(void);
/* 必须从唯一 USB 任务调用。 */
void storage_tick(void);
bool storage_ready(void);
void w2b_msc_cancel(void);
