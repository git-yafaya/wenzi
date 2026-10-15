#include "punctuation.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sherpa-onnx/c-api/c-api.h"

struct wenzi_punctuation {
    const SherpaOnnxOfflinePunctuation *processor;
};

wenzi_punctuation *wenzi_punctuation_create(const char *model, int num_threads,
                                          char *error, size_t size) {
    struct stat info;
    int code = 0;
    if (error && size) error[0] = '\0';
    if (!model || !*model) code = EINVAL;
    else if (stat(model, &info) < 0 || access(model, R_OK) < 0) code = errno;
    else if (!S_ISREG(info.st_mode) || info.st_size == 0) code = EINVAL;
    if (code) {
        if (error && size)
            snprintf(error, size, "无法读取标点模型 %s: %s",
                     model ? model : "", strerror(code));
        errno = code;
        return NULL;
    }

    wenzi_punctuation *punctuation = calloc(1, sizeof(*punctuation));
    if (!punctuation) {
        if (error && size) snprintf(error, size, "标点模块内存不足");
        errno = ENOMEM;
        return NULL;
    }
    SherpaOnnxOfflinePunctuationConfig config = {0};
    config.model.ct_transformer = model;
    config.model.num_threads = num_threads > 0 ? num_threads : 1;
    config.model.provider = "cpu";
    punctuation->processor = SherpaOnnxCreateOfflinePunctuation(&config);
    if (!punctuation->processor) {
        if (error && size) snprintf(error, size, "无法初始化 CT-Transformer 标点模型");
        free(punctuation);
        errno = EINVAL;
        return NULL;
    }
    return punctuation;
}

const char *wenzi_punctuation_apply(wenzi_punctuation *punctuation,
                                  const char *text) {
    if (!punctuation || !text) {
        errno = EINVAL;
        return NULL;
    }
    return SherpaOfflinePunctuationAddPunct(punctuation->processor, text);
}

void wenzi_punctuation_free_text(const char *text) {
    if (text) SherpaOfflinePunctuationFreeText(text);
}

void wenzi_punctuation_destroy(wenzi_punctuation *punctuation) {
    if (!punctuation) return;
    SherpaOnnxDestroyOfflinePunctuation(punctuation->processor);
    free(punctuation);
}
