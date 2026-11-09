#include "fake_dependencies.h"
#include "meoave_app.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static meo_settings_t settings(uint8_t brightness, uint8_t bpm, bool reduced, bool list)
{
    return (meo_settings_t){brightness, bpm, reduced, list};
}

static void check_settings(meo_settings_t actual, meo_settings_t expected)
{
    CHECK(actual.brightness == expected.brightness);
    CHECK(actual.guide_bpm == expected.guide_bpm);
    CHECK(actual.reduced_motion == expected.reduced_motion);
    CHECK(actual.app_list_view == expected.app_list_view);
}

static void seed_settings(meo_settings_t s)
{
    fake_seed(KEY_BRIGHTNESS, s.brightness);
    fake_seed(KEY_BPM, s.guide_bpm);
    fake_seed(KEY_REDUCED, s.reduced_motion);
    fake_seed(KEY_APP_LIST, s.app_list_view);
}

static void check_durable(meo_settings_t s)
{
    for (int i = 0; i < KEY_COUNT; ++i) CHECK(fake.durable.present[i]);
    CHECK(fake.durable.values[KEY_BRIGHTNESS] == s.brightness);
    CHECK(fake.durable.values[KEY_BPM] == s.guide_bpm);
    CHECK(fake.durable.values[KEY_REDUCED] == (uint8_t)s.reduced_motion);
    CHECK(fake.durable.values[KEY_APP_LIST] == (uint8_t)s.app_list_view);
}

static void boot_pending(void)
{
    fake_boot();
    fake_service_at(0); /* Consume the adapter's initial settings queue item. */
    CHECK(fake.brightness_calls == 1 && fake.write_open_calls == 0);
    fake_clear_observations();
}

static void check_success(meo_settings_t expected)
{
    CHECK(fake.write_open_calls == 1 && fake.set_calls == KEY_COUNT);
    CHECK(fake.commit_calls == 1 && fake.close_calls == 1);
    CHECK(strcmp(fake.trace, "OSSSSCX") == 0);
    for (int i = 0; i < KEY_COUNT; ++i) CHECK(fake.set_keys[i] == i);
    CHECK(fake.set_values[KEY_BRIGHTNESS] == expected.brightness);
    CHECK(fake.set_values[KEY_BPM] == expected.guide_bpm);
    CHECK(fake.set_values[KEY_REDUCED] == (uint8_t)expected.reduced_motion);
    CHECK(fake.set_values[KEY_APP_LIST] == (uint8_t)expected.app_list_view);
    check_durable(expected);
}

static void check_one_notice(bool success)
{
    CHECK(fake.notice_count == 1 && fake.notices[0] == success);
}

static void check_read_only(const fake_image_t *before)
{
    CHECK(fake.write_open_calls == 0 && fake.set_calls == 0 && fake.commit_calls == 0);
    CHECK(fake.erase_calls == 0 && fake.trace[0] == '\0');
    for (int i = 0; i < KEY_COUNT; ++i) {
        CHECK(fake.durable.present[i] == before->present[i]);
        CHECK(fake.durable.values[i] == before->values[i]);
    }
    for (unsigned i = 0; i < fake.notice_count; ++i) CHECK(!fake.notices[i]);
}

/* Common assertions, not a replacement implementation of loading or saving.
 * Faults and the expected loaded settings are supplied by each scenario. */
static void read_failure_lifecycle(meo_settings_t expected, bool open_error,
                                   unsigned failed_keys)
{
    fake_image_t before = fake.durable;
    fake.poison_read_error_output = true;
    fake_boot();
    check_settings(fake.ui_config.settings, expected);
    check_settings(fake.ui_model.settings, expected);
    CHECK(fake.init_calls == 1 && fake.read_open_calls == 1);
    CHECK(fake.read_calls == (open_error ? 0U : KEY_COUNT));
    CHECK(fake.close_calls == (open_error ? 0U : 1U));
    CHECK(fake.log_calls == (open_error ? 1U : failed_keys));
    for (int i = 0; i < KEY_COUNT; ++i)
        CHECK(fake.read_key_calls[i] == (open_error ? 0U : 1U));
    check_one_notice(false); /* Feedback is required during initialization. */
    check_read_only(&before);

    fake_service_at(0);
    CHECK(fake.brightness_calls == 1 && fake.last_brightness == expected.brightness);
    fake_service_at(599999);
    CHECK(fake.notice_count == 1);
    fake_service_at(600000);
    CHECK(fake.notice_count == 2 && !fake.notices[1]);
    check_read_only(&before);

    /* The dependency is healthy now. This boot must still remain read-only. */
    fake.read_error_mask = 0;
    memset(fake.read_error_codes, 0, sizeof(fake.read_error_codes));
    fake.read_open_error = ESP_OK;
    meo_settings_t first = settings(64, 12, false, true);
    fake_ui_change(first);
    CHECK(fake.brightness_calls == 1 && fake.notice_count == 2);
    check_read_only(&before);
    fake_service_at(700000);
    CHECK(fake.brightness_calls == 2 && fake.last_brightness == 64);
    fake_service_at(1299999);
    CHECK(fake.notice_count == 2);
    fake.lock_failures = 2;
    fake_service_at(1300000);
    fake_service_at(1300001);
    CHECK(fake.notice_count == 2);
    check_read_only(&before);
    fake_service_at(1300002);
    CHECK(fake.notice_count == 3 && !fake.notices[2]);
    check_settings(fake.notice_settings[2], first);

    meo_settings_t second = settings(29, 4, true, false);
    fake_ui_change(second);
    fake_service_at(1400000);
    CHECK(fake.brightness_calls == 3 && fake.last_brightness == 29);
    fake_service_at(2000000);
    CHECK(fake.notice_count == 4 && !fake.notices[3]);
    check_settings(fake.notice_settings[3], second);
    fake_service_at(9000000);
    CHECK(fake.notice_count == 4 && fake.brightness_calls == 3);
    CHECK(fake.init_calls == 1 && fake.read_open_calls == 1);
    CHECK(fake.read_calls == (open_error ? 0U : KEY_COUNT));
    CHECK(fake.close_calls == (open_error ? 0U : 1U));
    check_read_only(&before);
}

static void before_ready(void)
{
    fake_service_at(0);
    fake_service_at(10000000);
    CHECK(fake.init_calls == 0 && fake.receive_calls == 0);
    CHECK(fake.write_open_calls == 0 && fake.lock_calls == 0 && fake.usb_calls == 0);
}

static void defaults(void)
{
    fake_boot();
    check_settings(fake.ui_config.settings, settings(80, 6, false, false));
    check_settings(fake.ui_model.settings, fake.ui_config.settings);
    CHECK(fake.init_calls == 1 && fake.read_open_calls == 1);
    CHECK(fake.read_calls == 4 && fake.close_calls == 1 && fake.notice_count == 0);
    CHECK(fake.write_open_calls == 0 && fake.brightness_calls == 0);
    fake_service_at(0);
    CHECK(fake.last_brightness == 80);
    fake_clear_observations();
    fake_service_at(599999);
    CHECK(fake.write_open_calls == 0 && fake.notice_count == 0);
    fake_service_at(600000);
    check_success(settings(80, 6, false, false));
    check_one_notice(true);
}

static void legacy_missing_app_list(void)
{
    fake_seed(KEY_BRIGHTNESS, 37);
    fake_seed(KEY_BPM, 9);
    fake_seed(KEY_REDUCED, 1);
    fake_boot();
    check_settings(fake.ui_config.settings, settings(37, 9, true, false));
    fake_service_at(0);
    CHECK(fake.last_brightness == 37);
    fake_clear_observations();
    fake_service_at(600000);
    check_success(settings(37, 9, true, false));
}

static void missing_namespace(void)
{
    fake.read_open_error = ESP_ERR_NVS_NOT_FOUND;
    fake_boot();
    check_settings(fake.ui_config.settings, settings(80, 6, false, false));
    CHECK(fake.read_calls == 0 && fake.close_calls == 0);
    fake_service_at(0);
    fake_service_at(600000);
    check_success(settings(80, 6, false, false));
    check_one_notice(true);
}

static void validation(uint8_t brightness, uint8_t bpm, meo_settings_t expected)
{
    seed_settings(settings(brightness, bpm, true, true));
    fake_boot();
    check_settings(fake.ui_config.settings, expected);
    fake_service_at(0);
    CHECK(fake.last_brightness == expected.brightness);
    fake_clear_observations();
    fake_service_at(600000);
    check_success(expected);
}

static void read_errors(void)
{
    seed_settings(settings(37, 9, true, true));
    fake.read_error_mask = (1U << KEY_BRIGHTNESS) | (1U << KEY_REDUCED)
                           | (1U << KEY_APP_LIST);
    read_failure_lifecycle(settings(80, 9, false, false), false, 3);
}

static void missing_key_migration(unsigned key, meo_settings_t expected)
{
    seed_settings(settings(37, 9, true, true));
    CHECK(key < KEY_COUNT);
    fake.durable.present[key] = false;
    fake_boot();
    check_settings(fake.ui_config.settings, expected);
    check_settings(fake.ui_model.settings, expected);
    CHECK(fake.read_calls == KEY_COUNT && fake.close_calls == 1);
    for (int i = 0; i < KEY_COUNT; ++i) CHECK(fake.read_key_calls[i] == 1);
    CHECK(fake.notice_count == 0 && fake.log_calls == 0);
    CHECK(!fake.durable.present[key]);
    fake_service_at(0);
    CHECK(fake.last_brightness == expected.brightness);
    fake_clear_observations();
    fake_service_at(599999);
    CHECK(fake.write_open_calls == 0 && fake.notice_count == 0);
    CHECK(!fake.durable.present[key]);
    fake_service_at(600000);
    check_success(expected);
    check_one_notice(true);
    check_settings(fake.notice_settings[0], expected);
}

static bool run_key_case(const char *name)
{
    /* Explicit oracles: only the unreadable/missing key falls back. */
    static const struct {
        const char *name;
        unsigned key;
        meo_settings_t expected;
    } cases[] = {
        {"brightness", KEY_BRIGHTNESS, {80, 9, true, true}},
        {"guide_bpm", KEY_BPM, {37, 6, true, true}},
        {"reduced", KEY_REDUCED, {37, 9, false, true}},
        {"app_list", KEY_APP_LIST, {37, 9, true, false}}
    };
    static const struct {
        const char *suffix;
        esp_err_t error;
    } errors[] = {{"io_error", ESP_FAIL}, {"type_mismatch", ESP_ERR_NVS_TYPE_MISMATCH}};
    char candidate[96];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        for (size_t j = 0; j < sizeof(errors) / sizeof(errors[0]); ++j) {
            (void)snprintf(candidate, sizeof(candidate), "read_%s_%s",
                           cases[i].name, errors[j].suffix);
            if (strcmp(name, candidate) == 0) {
                seed_settings(settings(37, 9, true, true));
                fake.read_error_codes[cases[i].key] = errors[j].error;
                read_failure_lifecycle(cases[i].expected, false, 1);
                return true;
            }
        }
        (void)snprintf(candidate, sizeof(candidate), "missing_%s_migration", cases[i].name);
        if (strcmp(name, candidate) == 0) {
            missing_key_migration(cases[i].key, cases[i].expected);
            return true;
        }
    }
    return false;
}

static void boolean_normalization(void)
{
    fake_seed(KEY_REDUCED, 255);
    fake_seed(KEY_APP_LIST, 2);
    boot_pending();
    check_settings(fake.ui_config.settings, settings(80, 6, true, true));
    fake_service_at(600000);
    check_success(settings(80, 6, true, true));
}

static void debounce_last_value(void)
{
    boot_pending();
    fake_ui_change(settings(20, 4, false, false));
    fake_ui_change(settings(30, 5, true, false));
    fake_ui_change(settings(40, 7, false, true));
    CHECK(fake.overwrite_calls == 3 && fake.brightness_calls == 0);
    CHECK(fake.write_open_calls == 0); /* UI callback does no display/NVS IO. */
    fake_service_at(100000);
    CHECK(fake.brightness_calls == 1 && fake.last_brightness == 40);
    fake_service_at(699999);
    CHECK(fake.write_open_calls == 0);
    fake_ui_change(settings(57, 11, true, true));
    fake_service_at(699999);
    CHECK(fake.brightness_calls == 2 && fake.last_brightness == 57);
    fake_service_at(700000); /* Previous deadline was displaced. */
    fake_service_at(1299998);
    CHECK(fake.write_open_calls == 0 && fake.notice_count == 0);
    fake_service_at(1299999);
    check_success(settings(57, 11, true, true));
    check_one_notice(true);
    fake_service_at(2000000);
    CHECK(fake.commit_calls == 1 && fake.notice_count == 1);
}

static void init_failure(esp_err_t error)
{
    fake.init_error = error;
    fake_boot();
    CHECK(fake.init_calls == 1 && fake.read_open_calls == 0 && fake.erase_calls == 0);
    check_settings(fake.ui_config.settings, settings(80, 6, false, false));
    check_one_notice(false);
    fake_service_at(0);
    fake_service_at(600000);
    CHECK(fake.notice_count == 2 && !fake.notices[1]);
    fake_ui_change(settings(41, 10, true, true));
    fake_service_at(700000);
    fake_service_at(1300000);
    CHECK(fake.notice_count == 3 && !fake.notices[2]);
    CHECK(fake.last_brightness == 41 && fake.write_open_calls == 0);
    CHECK(fake.set_calls == 0 && fake.commit_calls == 0 && fake.erase_calls == 0);
    fake_service_at(9000000);
    CHECK(fake.notice_count == 3 && fake.init_calls == 1);
}

static void write_failure(int fail_set, bool fail_open, bool fail_commit)
{
    seed_settings(settings(32, 5, false, false));
    boot_pending();
    fake_ui_change(settings(61, 8, true, true));
    fake_service_at(1000);
    fake.fail_set_at = fail_set;
    fake.write_open_error = fail_open ? ESP_FAIL : ESP_OK;
    fake.commit_error = fail_commit ? ESP_FAIL : ESP_OK;
    fake_service_at(601000);
    CHECK(fake.write_open_calls == 1);
    CHECK(fake.set_calls == (unsigned)(fail_open ? 0 : fail_set ? fail_set : 4));
    CHECK(fake.commit_calls == (unsigned)fail_commit);
    CHECK(fake.close_calls == (unsigned)!fail_open);
    if (fail_open) CHECK(strcmp(fake.trace, "O") == 0);
    else if (fail_commit) CHECK(strcmp(fake.trace, "OSSSSCX") == 0);
    else {
        char expected[8] = "O";
        for (int i = 0; i < fail_set; ++i) strcat(expected, "S");
        strcat(expected, "X");
        CHECK(strcmp(fake.trace, expected) == 0);
    }
    for (unsigned i = 0; i < fake.set_calls; ++i) CHECK(fake.set_keys[i] == (int)i);
    check_one_notice(false);
    fake_service_at(9000000);
    CHECK(fake.write_open_calls == 1 && fake.notice_count == 1);
    /* A subsequent user change must be able to save after the fault clears. */
    fake_clear_observations();
    fake.fail_set_at = 0;
    fake.write_open_error = fake.commit_error = ESP_OK;
    fake_ui_change(settings(74, 12, false, true));
    fake_service_at(9000001);
    fake_service_at(9600001);
    check_success(settings(74, 12, false, true));
    check_one_notice(true);
}

static void notice_retry(bool success)
{
    boot_pending();
    fake.commit_error = success ? ESP_OK : ESP_FAIL;
    fake.lock_failures = 3;
    fake_service_at(600000);
    CHECK(fake.write_open_calls == 1 && fake.set_calls == 4 && fake.commit_calls == 1);
    CHECK(fake.close_calls == 1 && fake.notice_count == 0);
    fake_service_at(600001);
    fake_service_at(600002);
    CHECK(fake.lock_calls == 3 && fake.notice_count == 0);
    fake_service_at(600003);
    check_one_notice(success);
    CHECK(fake.lock_calls == 4 && fake.unlock_calls == 1);
    fake_service_at(9999999);
    CHECK(fake.write_open_calls == 1 && fake.set_calls == 4 && fake.commit_calls == 1);
    CHECK(fake.notice_count == 1 && fake.lock_calls == 4);
}

static void update_during(int hook, bool old_success)
{
    boot_pending();
    fake.hook = hook;
    fake.hook_settings = settings(49, 10, true, true);
    fake.commit_error = old_success ? ESP_OK : ESP_FAIL;
    fake_service_at(600000);
    CHECK(fake.hook == HOOK_NONE && fake.notice_count == 0);
    CHECK(fake.write_open_calls == 1 && fake.set_calls == 4 && fake.commit_calls == 1);
    CHECK(fake.set_values[KEY_BRIGHTNESS] == 80 && fake.set_values[KEY_BPM] == 6);
    CHECK(fake.set_values[KEY_REDUCED] == 0 && fake.set_values[KEY_APP_LIST] == 0);
    check_settings(fake.ui_model.settings, settings(49, 10, true, true));
    fake_clear_observations();
    fake.commit_error = ESP_OK;
    fake_service_at(600001);
    CHECK(fake.last_brightness == 49 && fake.notice_count == 0);
    fake_service_at(1200000);
    CHECK(fake.write_open_calls == 0 && fake.notice_count == 0);
    fake_service_at(1200001);
    check_success(settings(49, 10, true, true));
    check_one_notice(true);
    check_settings(fake.notice_settings[0], settings(49, 10, true, true));
}

static void update_while_notice_waits(void)
{
    boot_pending();
    fake.lock_failures = 1;
    fake_service_at(600000);
    CHECK(fake.commit_calls == 1 && fake.notice_count == 0);
    fake_ui_change(settings(53, 7, false, true));
    fake_clear_observations();
    fake_service_at(600001);
    CHECK(fake.lock_calls == 0 && fake.notice_count == 0 && fake.write_open_calls == 0);
    fake_service_at(1200000);
    CHECK(fake.notice_count == 0 && fake.write_open_calls == 0);
    fake_service_at(1200001);
    check_success(settings(53, 7, false, true));
    check_one_notice(true);
}

static void update_at_deadline(void)
{
    boot_pending();
    fake_ui_change(settings(71, 8, true, false));
    fake_service_at(600000);
    CHECK(fake.write_open_calls == 0 && fake.last_brightness == 71);
    fake_service_at(1199999);
    CHECK(fake.write_open_calls == 0);
    fake_service_at(1200000);
    check_success(settings(71, 8, true, false));
}

static void check_status(void)
{
    CHECK(strcmp(fake.usb_text,
        "MEO:UI page=HOME frames=123 rotation=180 brightness=80 reduced=0 "
        "running=0 mode=GUIDED elapsed_ms=0 wifi=UNAVAILABLE radar=0 app_view=GRID eye=0 eye_auto=1\n") == 0);
}

static void status_retry(void)
{
    boot_pending();
    fake.lock_failures = 2;
    meoave_app_request_status();
    meoave_app_request_status(); /* Coalesced request, no output until serviced. */
    CHECK(fake.usb_calls == 0);
    fake_service_at(1);
    fake_service_at(2);
    CHECK(fake.usb_calls == 0 && fake.lock_calls == 2);
    fake_service_at(3);
    CHECK(fake.usb_calls == 1 && fake.lock_calls == 3 && fake.unlock_calls == 1);
    check_status();
    fake_service_at(4);
    CHECK(fake.usb_calls == 1 && fake.lock_calls == 3 && fake.write_open_calls == 0);
}

static void status_before_ready(void)
{
    meoave_app_request_status();
    fake_service_at(0);
    CHECK(fake.usb_calls == 0 && fake.lock_calls == 0);
    fake_boot();
    fake_service_at(1);
    CHECK(fake.usb_calls == 1);
    check_status();
    fake_service_at(2);
    CHECK(fake.usb_calls == 1);
}

static void status_during_notice_retry(void)
{
    boot_pending();
    fake.lock_failures = 2; /* Save notice and status each fail once. */
    meoave_app_request_status();
    fake_service_at(600000);
    CHECK(fake.lock_calls == 2 && fake.notice_count == 0 && fake.usb_calls == 0);
    CHECK(fake.commit_calls == 1);
    fake_service_at(600001);
    CHECK(fake.lock_calls == 4 && fake.unlock_calls == 2 && fake.usb_calls == 1);
    check_one_notice(true);
    check_status();
    fake_service_at(600002);
    CHECK(fake.commit_calls == 1 && fake.lock_calls == 4 && fake.usb_calls == 1);
}

static void status_reentrant(void)
{
    boot_pending();
    fake.request_status_on_usb = true;
    meoave_app_request_status();
    fake_service_at(1);
    CHECK(fake.usb_calls == 1);
    fake_service_at(2);
    CHECK(fake.usb_calls == 2);
    fake_service_at(3);
    CHECK(fake.usb_calls == 2);
}

static void brightness_failure(void)
{
    boot_pending();
    fake.brightness_error = ESP_FAIL;
    fake_ui_change(settings(46, 11, true, true));
    fake_service_at(1);
    CHECK(fake.brightness_calls == 1 && fake.last_brightness == 46 && fake.log_calls == 1);
    CHECK(fake.write_open_calls == 0);
    fake_service_at(600001);
    check_success(settings(46, 11, true, true));
    check_one_notice(true);
}

static void roundtrip_write(const char *path)
{
    CHECK(path != NULL);
    /* Remove any prior fixture so an old run can never satisfy the reader. */
    FILE *old = fopen(path, "rb");
    if (old) { CHECK(fclose(old) == 0); CHECK(remove(path) == 0); }
    fake.image_path = path;
    boot_pending();
    fake_ui_change(settings(43, 11, true, true));
    fake_service_at(12345);
    fake_service_at(612344);
    CHECK(fake.commit_calls == 0);
    fake_service_at(612345);
    check_success(settings(43, 11, true, true));
    check_one_notice(true);
}

static void roundtrip_load(const char *path)
{
    CHECK(path != NULL);
    fake_read_image(path);
    fake_boot(); /* Fresh OS process: all production statics initialized by C. */
    check_settings(fake.ui_config.settings, settings(43, 11, true, true));
    check_settings(fake.ui_model.settings, settings(43, 11, true, true));
    CHECK(fake.init_calls == 1 && fake.read_calls == 4 && fake.notice_count == 0);
    fake_service_at(0);
    CHECK(fake.last_brightness == 43 && fake.write_open_calls == 0);
    /* A clean process following the read-fault process can persist again. */
    fake_clear_observations();
    fake.image_path = path;
    fake_ui_change(settings(72, 7, false, false));
    fake_service_at(1);
    fake_service_at(600000);
    CHECK(fake.write_open_calls == 0);
    fake_service_at(600001);
    check_success(settings(72, 7, false, false));
    check_one_notice(true);
    fake_read_image(path);
    check_durable(settings(72, 7, false, false));
}

static void roundtrip_read_failure(const char *path)
{
    CHECK(path != NULL);
    fake_read_image(path);
    check_durable(settings(43, 11, true, true));
    fake.image_path = path;
    fake.read_error_codes[KEY_BPM] = ESP_ERR_NVS_TYPE_MISMATCH;
    read_failure_lifecycle(settings(43, 6, true, true), false, 1);
    /* Also inspect the shared file, not just the fake's in-memory durable data. */
    fake_read_image(path);
    check_durable(settings(43, 11, true, true));
}

static void read_error_feedback(bool open_error)
{
    seed_settings(settings(37, 9, true, true));
    if (open_error) fake.read_open_error = ESP_FAIL;
    else fake.read_error_mask = 1U << KEY_BRIGHTNESS;
    read_failure_lifecycle(open_error ? settings(80, 6, false, false)
                                     : settings(80, 9, true, true), open_error, 1);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2 || argc == 3);
    fake_init();
    const char *name = argv[1];
    const char *path = argc == 3 ? argv[2] : NULL;
#define CASE(id) if (strcmp(name, #id) == 0) id()
    CASE(before_ready);
    else CASE(defaults);
    else CASE(legacy_missing_app_list);
    else CASE(missing_namespace);
    else if (strcmp(name, "validation_low") == 0)
        validation(14, 3, settings(80, 6, true, true));
    else if (strcmp(name, "validation_high") == 0)
        validation(101, 13, settings(80, 6, true, true));
    else if (strcmp(name, "validation_min") == 0)
        validation(15, 4, settings(15, 4, true, true));
    else if (strcmp(name, "validation_max") == 0)
        validation(100, 12, settings(100, 12, true, true));
    else CASE(read_errors);
    else CASE(boolean_normalization);
    else CASE(debounce_last_value);
    else if (strcmp(name, "init_no_free_pages") == 0) init_failure(ESP_ERR_NVS_NO_FREE_PAGES);
    else if (strcmp(name, "init_new_version") == 0) init_failure(ESP_ERR_NVS_NEW_VERSION_FOUND);
    else if (strcmp(name, "init_generic_error") == 0) init_failure(ESP_FAIL);
    else if (strcmp(name, "open_failure") == 0) write_failure(0, true, false);
    else if (strcmp(name, "set_brightness_failure") == 0) write_failure(1, false, false);
    else if (strcmp(name, "set_bpm_failure") == 0) write_failure(2, false, false);
    else if (strcmp(name, "set_reduced_failure") == 0) write_failure(3, false, false);
    else if (strcmp(name, "set_app_list_failure") == 0) write_failure(4, false, false);
    else if (strcmp(name, "commit_failure") == 0) write_failure(0, false, true);
    else if (strcmp(name, "notice_retry_success") == 0) notice_retry(true);
    else if (strcmp(name, "notice_retry_failure") == 0) notice_retry(false);
    else if (strcmp(name, "update_during_commit_success") == 0) update_during(HOOK_COMMIT, true);
    else if (strcmp(name, "update_during_commit_failure") == 0) update_during(HOOK_COMMIT, false);
    else if (strcmp(name, "update_during_set") == 0) update_during(HOOK_SET, true);
    else if (strcmp(name, "update_during_lock") == 0) update_during(HOOK_LOCK, true);
    else CASE(update_while_notice_waits);
    else CASE(update_at_deadline);
    else CASE(status_retry);
    else CASE(status_before_ready);
    else CASE(status_during_notice_retry);
    else CASE(status_reentrant);
    else CASE(brightness_failure);
    else if (strcmp(name, "roundtrip_write") == 0) roundtrip_write(path);
    else if (strcmp(name, "roundtrip_load") == 0) roundtrip_load(path);
    else if (strcmp(name, "roundtrip_read_failure") == 0) roundtrip_read_failure(path);
    else if (strcmp(name, "read_open_error_feedback") == 0) read_error_feedback(true);
    else if (strcmp(name, "read_value_error_feedback") == 0) read_error_feedback(false);
    else if (!run_key_case(name)) {
        fprintf(stderr, "Unknown test case: %s\n", name);
        return EXIT_FAILURE;
    }
#undef CASE
    CHECK(fake.erase_calls == 0 && fake.critical_depth == 0 && !fake.gui_locked);
    printf("PASS %s\n", name);
    return EXIT_SUCCESS;
}
