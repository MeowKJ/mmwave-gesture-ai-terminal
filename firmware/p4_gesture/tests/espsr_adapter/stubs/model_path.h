#pragma once
typedef struct { int num; char **model_info; } srmodel_list_t;
srmodel_list_t *esp_srmodel_init(const char *name);
int esp_srmodel_exists(srmodel_list_t *models, char *name);
char *esp_srmodel_get_wake_words(srmodel_list_t *models, char *name);
void esp_srmodel_deinit(srmodel_list_t *models);
