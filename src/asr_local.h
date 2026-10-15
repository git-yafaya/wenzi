#ifndef WENZI_ASR_LOCAL_H
#define WENZI_ASR_LOCAL_H

#include "wenzi.h"

typedef struct wenzi_local wenzi_local;

/* 创建不回调；feed 和 finish 同步回调，事件错误码是正 errno。 */
wenzi_local *wenzi_local_create(const wenzi_config *config,
                              wenzi_asr_callback callback, void *user,
                              char *error, size_t size);
/* 每块最多 WENZI_FRAME_SAMPLES 个采样。 */
int wenzi_local_feed(wenzi_local *local, const int16_t *samples, size_t count);
/* 真正排空后发 DONE；重复调用不重复发送。 */
int wenzi_local_finish(wenzi_local *local);
void wenzi_local_destroy(wenzi_local *local);

#endif
