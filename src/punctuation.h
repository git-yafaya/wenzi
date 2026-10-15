#ifndef WENZI_PUNCTUATION_H
#define WENZI_PUNCTUATION_H

#include <stddef.h>

typedef struct wenzi_punctuation wenzi_punctuation;

wenzi_punctuation *wenzi_punctuation_create(const char *model, int num_threads,
                                          char *error, size_t size);
/* 返回文本由库分配，用 free_text 释放。 */
const char *wenzi_punctuation_apply(wenzi_punctuation *punctuation,
                                  const char *text);
void wenzi_punctuation_free_text(const char *text);
void wenzi_punctuation_destroy(wenzi_punctuation *punctuation);

#endif
