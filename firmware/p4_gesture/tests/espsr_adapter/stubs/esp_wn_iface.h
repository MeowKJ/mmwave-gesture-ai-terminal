#pragma once
#include <stdint.h>
typedef struct { int unused; } model_iface_data_t;
#define DET_MODE_90 0
typedef struct {
    model_iface_data_t *(*create)(const char *, int);
    int (*get_samp_chunksize)(model_iface_data_t *);
    int (*get_samp_rate)(model_iface_data_t *);
    int (*get_channel_num)(model_iface_data_t *);
    int (*detect)(model_iface_data_t *, int16_t *);
    void (*clean)(model_iface_data_t *);
    void (*destroy)(model_iface_data_t *);
} esp_wn_iface_t;
