#pragma once
void fake_log(const char *level, const char *tag, const char *format, ...);
#define ESP_LOGE(tag, ...) fake_log("E", (tag), __VA_ARGS__)
#define ESP_LOGW(tag, ...) fake_log("W", (tag), __VA_ARGS__)
