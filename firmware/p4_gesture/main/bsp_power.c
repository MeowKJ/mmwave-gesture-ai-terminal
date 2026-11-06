#include "bsp_power.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc.h"
#include "soc/lp_system_reg.h"

#include "bsp_common.h"
#include "board_pins.h"

static const char *TAG = "BSP_POWER";

/* RTC_NOINIT 在 LP SRAM，软件复位不清、掉电才清。用它做「复位期间是否掉电」
   的体检标记：下载复位前写，应用启动读；读到就说明 PWR_HOLD 撑住了。 */
#define DL_MARKER 0x4D454F31u            /* "MEO1" */
RTC_NOINIT_ATTR static uint32_t s_dl_marker;
static bool s_download_survived;
static bool s_pad_hold_active;
static bool s_key_at_boot;      /* 开机锁存瞬间电源键是否仍被按住 */

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
    s_dl_marker = DL_MARKER;

    /* 关键：把 PWR_HOLD 当前的高电平锁存在 IO pad 上。P4 rev>=3 的
       gpio_hold_en() 走 LP_SYS.pad_rtc_hold_ctrl0（always-on），TOP 域
       esp_restart() 时管脚不会退回高阻，Q2/Q1 保持导通，3V3 不掉电。
       复位前后的整个 ROM 下载过程都靠这个锁存撑着。 */
    esp_err_t err = gpio_hold_en(PIN_PWR_HOLD);
    ESP_LOGW(TAG, "download reset: PWR_HOLD pad hold %s (reg=%d), marker set",
             err == ESP_OK ? "ENABLED" : esp_err_to_name(err),
             bsp_power_pad_hold_active());
}

void bsp_power_init(void)
{
    /* 按键开机的最后一步，也是 app_main 的第一件事：
       SW1 按下时 D1/Q1 已把 VSYS 接到 SYS_PWR_EN（硬件上电），固件一上来
       把 PWR_HOLD 拉高，让 Q2 接管 Q1 栅极 —— 此后松手也不掉电。
       必须尽早：晚一毫秒，用户就得多按住一毫秒。 */
    bsp_gpio_out(PIN_PWR_HOLD, 1);

    /* 先读诊断，再释放上一次复位遗留的 pad hold。顺序不能反：释放 hold
       的瞬间管脚由 GPIO 输出寄存器接管，必须保证它已经是输出高。 */
    s_download_survived = (s_dl_marker == DL_MARKER);
    s_dl_marker = 0;
    s_pad_hold_active = bsp_power_pad_hold_active();
    esp_err_t hold_err = gpio_hold_dis(PIN_PWR_HOLD);
    ESP_LOGI(TAG, "PWR_HOLD=1, download-reboot survived=%d, pad_hold_was=%d, release=%s",
             s_download_survived, s_pad_hold_active,
             hold_err == ESP_OK ? "OK" : esp_err_to_name(hold_err));
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
