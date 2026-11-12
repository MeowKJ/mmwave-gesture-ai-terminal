#include "bsp_power.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc.h"
#include "soc/lp_system_reg.h"
#include "soc/gpio_reg.h"
#include <stdio.h>
#include "esp_private/panic_internal.h"
#include "riscv/rvruntime-frames.h"

#include "bsp_common.h"
#include "board_pins.h"

static const char *TAG = "BSP_POWER";

/* A retained marker is diagnostic evidence only, not a voltage measurement.
   Reset domains also determine whether RTC_NOINIT survives. */
#define DL_MARKER 0x4D454F31u            /* "MEO1" */
RTC_NOINIT_ATTR static uint32_t s_dl_marker;
static bool s_download_survived;
static bool s_pad_hold_active;
static bool s_key_at_boot;      /* 开机锁存瞬间电源键是否仍被按住 */
static volatile bool s_poweroff; /* 用户已请求关机：复位钩子不得再锁存 */

/* Preserve a minimal panic frame for native-USB diagnostics after reboot.
   No USB, allocation, logging, or reset-hint modification in panic context. */
#define PANIC_MARKER 0x50414E31u
typedef struct {
    uint32_t valid, core, pc, ra, sp, cause, badaddr;
} reset_panic_t;
RTC_NOINIT_ATTR static volatile reset_panic_t s_reset_panic;
void __real_esp_panic_handler(panic_info_t *info);
void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    s_reset_panic.valid = 0;
    s_reset_panic.core = info->core;
    s_reset_panic.pc = (uint32_t)info->addr;
    if (info->frame) {
        const RvExcFrame *frame = info->frame;
        s_reset_panic.pc = frame->mepc;
        s_reset_panic.ra = frame->ra;
        s_reset_panic.sp = frame->sp;
        s_reset_panic.cause = frame->mcause;
        s_reset_panic.badaddr = frame->mtval;
    }
    s_reset_panic.valid = PANIC_MARKER;
    __real_esp_panic_handler(info);
}

void bsp_power_reset_diagnostic(char *buffer, unsigned size)
{
    snprintf(buffer, size,
             "valid=%d core=%lu pc=%08lx ra=%08lx sp=%08lx cause=%08lx bad=%08lx",
             s_reset_panic.valid == PANIC_MARKER, (unsigned long)s_reset_panic.core,
             (unsigned long)s_reset_panic.pc, (unsigned long)s_reset_panic.ra,
             (unsigned long)s_reset_panic.sp, (unsigned long)s_reset_panic.cause,
             (unsigned long)s_reset_panic.badaddr);
}

static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_adc_chan;
static adc_cali_handle_t s_cali;
static bool s_adc_ready;

bool bsp_power_pad_hold_active(void)
{
#ifdef LP_SYSTEM_REG_PAD_RTC_HOLD_CTRL0_REG
    /* P4 rev>=3: LP_SYS 常开域，bit n = GPIO n */
    return (REG_READ(LP_SYSTEM_REG_PAD_RTC_HOLD_CTRL0_REG) & BIT(29)) != 0;
#else
    return false;
#endif
}

bool bsp_power_download_survived(void) { return s_download_survived; }

void bsp_power_prepare_download_reset(void)
{
    esp_err_t err = bsp_power_prepare_reset();
    ESP_LOGW(TAG, "download reset: PWR_HOLD pad hold %s (reg=%d), marker set",
             err == ESP_OK ? "ENABLED" : esp_err_to_name(err),
             bsp_power_pad_hold_active());
}

bool bsp_power_hold_ready(void)
{
    const uint32_t mask = BIT(PIN_PWR_HOLD);
    return bsp_power_pad_hold_active() && (REG_READ(GPIO_OUT_REG) & mask) &&
           (REG_READ(GPIO_ENABLE_REG) & mask);
}

esp_err_t bsp_power_prepare_reset(void)
{
    esp_err_t err = gpio_set_level(PIN_PWR_HOLD, 1);
    if (err != ESP_OK) return err;
    err = gpio_hold_en(PIN_PWR_HOLD);
    if (err != ESP_OK) return err;
    if (!bsp_power_hold_ready()) return ESP_ERR_INVALID_STATE;
    s_reset_panic.valid = 0;
    s_dl_marker = DL_MARKER;
    return ESP_OK;
}

static void power_shutdown_handler(void)
{
    /* Cover every normal esp_restart(), not only explicit download requests.
       Keep an already-high pad latched; never release it during startup.
       An explicit user power-off must stay off: do not re-latch. */
    if (s_poweroff) return;
    (void)bsp_power_prepare_reset();
}

void bsp_power_init(void)
{
    /* 按键开机的最后一步，也是 app_main 的第一件事：
       SW1 按下时 D1/Q1 已把 VSYS 接到 SYS_PWR_EN（U10 的 EN，不是 VIN），固件一上来
       把 PWR_HOLD 拉高，让 Q2 接管 Q1 栅极 —— 此后松手也不掉电。
       必须尽早：晚一毫秒，用户就得多按住一毫秒。 */
    /* Preload the output latch before enabling output: no initial low pulse. */
    ESP_ERROR_CHECK(gpio_set_level(PIN_PWR_HOLD, 1));
    bsp_gpio_out(PIN_PWR_HOLD, 1);

    /* Preserve any inherited high pad hold across the entire boot process. */
    s_download_survived = (s_dl_marker == DL_MARKER);
    s_dl_marker = 0;
    s_pad_hold_active = bsp_power_pad_hold_active();
    ESP_ERROR_CHECK(gpio_hold_en(PIN_PWR_HOLD));
    ESP_ERROR_CHECK(bsp_power_hold_ready() ? ESP_OK : ESP_ERR_INVALID_STATE);
    ESP_ERROR_CHECK(esp_register_shutdown_handler(power_shutdown_handler));
    ESP_LOGI(TAG, "PWR_HOLD=1, reset-marker retained=%d, pad_hold_was=%d, hold=CONTINUOUS",
             s_download_survived, s_pad_hold_active);
    if (s_dl_marker == DL_MARKER) ESP_LOGE(TAG, "marker clear failed");

    /* 电池分压使能：VBAT_AD_EN=1 -> Q5 导通 -> Q4(P-MOS) 导通 -> VBAT 进入
       R36 100K / R37 100K 分压。R38/R39 是 470K 偏置，默认关断省电。 */
    bsp_gpio_out(PIN_BAT_AD_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(15));       /* 等 Q4 栅极与分压节点稳定 */

    /* VBUS_ON 是 R34/R35 = 100K/100K 分压检测脚（VBUS/2 ≈ 2.5V），
       是输入，不是输出；配成输出会与 VBUS 打架。 */
    bsp_gpio_in(PIN_VBUS_ON, false);
    bsp_gpio_in(PIN_PWR_DET, true);
    bsp_gpio_in(PIN_BUCK_PG, true);

    /* 记录开机瞬间 SW1 是否还按着（1 = 正常的「按住电源键开机」）。
       仅用于日志 / 开机报告，不参与供电判定：锁存已经在函数第一行完成，
       此后无论按键状态如何都保持开机，本版本也不做任何关机动作。 */
    s_key_at_boot = bsp_power_key_pressed();
    /* 放在 ADC 初始化之前：即使电池采样不可用，开机事实也一定要有日志。 */
    ESP_LOGI(TAG, "power ON latched (PWR_HOLD=1, no software power-off), key_at_boot=%d",
             s_key_at_boot);

    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(PIN_BAT_AD, &unit, &s_adc_chan) != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d is not an ADC pin, battery monitor disabled", (int)PIN_BAT_AD);
        return;
    }
    adc_oneshot_unit_init_cfg_t init = {.unit_id = unit};
    if (adc_oneshot_new_unit(&init, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "adc new unit failed");
        return;
    }
    adc_oneshot_chan_cfg_t ch = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(s_adc, s_adc_chan, &ch) != ESP_OK) {
        ESP_LOGW(TAG, "adc config channel failed");
        return;
    }

#if defined(ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED) && ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .chan = s_adc_chan,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "adc curve-fitting calibration unavailable, using linear estimate");
        s_cali = NULL;
    }
#elif defined(ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED) && ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    const adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "adc line-fitting calibration unavailable, using linear estimate");
        s_cali = NULL;
    }
#endif

    s_adc_ready = true;
    ESP_LOGI(TAG, "VBUS=%d, PWR_DET=%d, BUCK_PG=%d, cali=%s",
             bsp_power_usb_attached(), bsp_gpio_read(PIN_PWR_DET),
             bsp_gpio_read(PIN_BUCK_PG), s_cali ? "yes" : "linear");
}

int bsp_power_battery_mv(void)
{
    if (!s_adc_ready) return -1;
    int raw = 0;
    if (adc_oneshot_read(s_adc, s_adc_chan, &raw) != ESP_OK) return -1;

    int mv;
    if (s_cali) {
        if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return -1;
    } else {
        /* 12dB 衰减满量程约 3100mV（无 eFuse 校准时的保守线性估算） */
        mv = raw * 3100 / 4095;
    }
    /* R36 100K / R37 100K：ADC 节点 = VBAT / 2 */
    return mv * BAT_ADC_DIV_RATIO;
}

bool bsp_power_usb_attached(void) { return bsp_gpio_read(PIN_VBUS_ON) != 0; }
bool bsp_power_key_pressed(void)  { return bsp_gpio_read(PIN_PWR_DET) == 0; }
bool bsp_power_key_at_boot(void)  { return s_key_at_boot; }
bool bsp_power_buck_ok(void)      { return bsp_gpio_read(PIN_BUCK_PG) != 0; }
bool bsp_power_charging(void)     { return bsp_power_usb_attached(); }

int bsp_power_battery_percent(void)
{
    /* 单体锂电开路电压近似值，不是库仑计：只在无大电流负载时可信。
       Q4 打开分压后有几十微秒的建立过程，取 5 次平均，避免单次抖动。 */
    int sum = 0, count = 0;
    for (unsigned i = 0; i < 5; ++i) {
        int mv = bsp_power_battery_mv();
        if (mv > 0) { sum += mv; ++count; }
        if (i < 4) vTaskDelay(pdMS_TO_TICKS(4));
    }
    if (!count) return -1;
    int mv = sum / count;
    static const struct { int mv, pct; } curve[] = {
        {4200,100}, {4080,88}, {3980,74}, {3900,62}, {3820,48},
        {3760,34}, {3700,20}, {3600,8}, {3450,3}, {3300,0},
    };
    if (mv >= curve[0].mv) return 100;
    for (unsigned i = 1; i < sizeof(curve)/sizeof(curve[0]); ++i) {
        if (mv >= curve[i].mv) {
            int hi_v = curve[i-1].mv, lo_v = curve[i].mv;
            int hi_c = curve[i-1].pct, lo_c = curve[i].pct;
            return lo_c + (mv - lo_v) * (hi_c - lo_c) / (hi_v - lo_v);
        }
    }
    return 0;
}

esp_err_t bsp_power_off(void)
{
    /* 唯一的关机入口：长按电源键倒计时结束。没有自动恢复、没有重试：
       先把 pad hold 放开，再把 PWR_HOLD 拉低，Q2 关断后 Q1 栅极被 R10 拉高，
       VSYS 与 SYS_PWR_EN 断开，3V3 掉电。 */
    s_poweroff = true;
    esp_err_t hold = gpio_hold_dis(PIN_PWR_HOLD);
    esp_err_t level = gpio_set_level(PIN_PWR_HOLD, 0);
    ESP_LOGW(TAG, "POWER OFF: PWR_HOLD=0 (hold_dis=%s, level=%s)",
             esp_err_to_name(hold), esp_err_to_name(level));
    return level;
}

bool bsp_power_off_requested(void) { return s_poweroff; }
