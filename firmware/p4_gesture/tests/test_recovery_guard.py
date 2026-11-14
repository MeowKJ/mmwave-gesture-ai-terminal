"""Host-only failure injection for the PPA patch and USB recovery code.

Compiles the actual generated factory/production recovery functions with
allocation and RTOS doubles. No USB, ESP-IDF build or hardware reset.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]
UPSTREAM = PROJECT / "managed_components/espressif__esp_lvgl_port/src/common/ppa/lcd_ppa.c"
PATCH = PROJECT / "cmake/PpaGuard.cmake"
CC = os.environ.get("CC") or shutil.which("gcc") or "C:/Apps/Env/mingw64/bin/gcc.exe"
CMAKE = shutil.which("cmake") or "C:/Apps/Env/cmake/bin/cmake.exe"


def generate(source, output, folder, check=True):
    script = folder / "patch.cmake"
    script.write_text(f'include("{PATCH.as_posix()}")\n'
                      f'meoave_patch_ppa("{source.as_posix()}" "{output.as_posix()}")\n', encoding="utf-8")
    return subprocess.run([CMAKE, "-P", str(script)], check=check, capture_output=True, text=True)


def compile_run(source, folder):
    path, exe = folder / "test.c", folder / "test.exe"
    path.write_text(source, encoding="utf-8")
    subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)


PPA_PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
typedef void *ppa_client_handle_t;
typedef int ppa_srm_color_mode_t;
typedef struct {} ppa_event_data_t;
typedef struct { int oper_type; } ppa_client_config_t;
typedef struct {
    bool (*on_trans_done)(ppa_client_handle_t, ppa_event_data_t *, void *);
} ppa_event_callbacks_t;
typedef struct lvgl_port_ppa_t lvgl_port_ppa_t;
typedef lvgl_port_ppa_t *lvgl_port_ppa_handle_t;
typedef struct {
    uint32_t buffer_size; int color_mode;
    struct { unsigned buff_dma:1, buff_spiram:1; } flags;
} lvgl_port_ppa_cfg_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_DEFAULT 4
#define CONFIG_CACHE_L2_CACHE_LINE_SIZE 64
#define PPA_OPERATION_SRM 1
#define PPA_LCD_ENABLE_CB 1
#define ALIGN_UP(n,a) (((n)+(a)-1)&~((a)-1))
#define ESP_GOTO_ON_FALSE(c,e,l,t,m) do { (void)(t); if (!(c)) { ret=(e); goto l; } } while(0)
#define ESP_GOTO_ON_ERROR(c,l,t,m) do { (void)(t); ret=(c); if(ret) goto l; } while(0)
static int allocs, fail_alloc, live, fail_register, fail_callback, unregisters;
static void *tracked_malloc(size_t n) {
    if (++allocs == fail_alloc) return NULL;
    void *p=malloc(n); assert(p); live++; return p;
}
static void tracked_free(void *p) { if(p) { live--; free(p); } }
static void *heap_caps_aligned_calloc(size_t a,size_t n,size_t s,unsigned caps) {
    assert(a==64 && caps==MALLOC_CAP_SPIRAM); return tracked_malloc(n*s);
}
static esp_err_t ppa_register_client(const ppa_client_config_t *c, ppa_client_handle_t *out) {
    assert(c->oper_type==1); if(fail_register) return 1; *out=(void*)0x1234; return 0;
}
static esp_err_t ppa_unregister_client(ppa_client_handle_t h) {
    assert(h==(void*)0x1234); unregisters++; return 0;
}
static esp_err_t ppa_client_register_event_callbacks(ppa_client_handle_t h,const ppa_event_callbacks_t *cb) {
    assert(h==(void*)0x1234 && cb->on_trans_done); return fail_callback;
}
#define malloc tracked_malloc
#define free tracked_free
'''
PPA_MAIN = r'''
#undef malloc
#undef free
static bool _lvgl_port_ppa_callback(ppa_client_handle_t h, ppa_event_data_t *e, void *u) {
    (void)h; (void)e; (void)u; return false;
}
int main(void) {
    lvgl_port_ppa_cfg_t cfg={.buffer_size=128,.color_mode=3,.flags.buff_spiram=1};
    for(int scenario=1; scenario<=5; scenario++) {
        allocs=live=unregisters=fail_register=fail_callback=fail_alloc=0;
        if(scenario<=2) fail_alloc=scenario;
        if(scenario==3) fail_register=1;
        if(scenario==4) fail_callback=1;
        lvgl_port_ppa_handle_t p=lvgl_port_ppa_create(&cfg);
        if(scenario<5) {
            assert(p==NULL && live==0);
            assert(unregisters==(scenario==4));
        } else {
            assert(p && p->color_mode==3 && live==2 && !unregisters);
            ppa_unregister_client(p->srm_handle);
            tracked_free(p->buffer); tracked_free(p); assert(live==0);
        }
    }
    return 0;
}
'''

USB_PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
typedef int esp_err_t;
typedef uint32_t StackType_t;
typedef struct { int unused; } StaticTask_t;
typedef void *TaskHandle_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 2
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
static int notified, takes, fail_power, prepared, quiesced, disconnected, forced, cleared;
static jmp_buf stop;
static void xTaskNotifyGive(TaskHandle_t h) { assert(h==(void*)1); notified++; }
static unsigned ulTaskNotifyTake(int clear, unsigned timeout) {
    assert(clear==1 && timeout==UINT32_MAX);
    if(takes++) longjmp(stop,2);
    return 1;
}
static void vTaskDelay(unsigned ms) { assert(ms==120 || ms==250); }
static int bsp_power_prepare_reset(void) { return fail_power; }
static void bsp_platform_prepare_download(void) { prepared++; }
static void meo_link_quiesce(void) { quiesced++; }
static void tud_disconnect(void) { disconnected++; }
static void set_force_download_boot(void) { forced++; }
static void clear_force_download_boot(void) { cleared++; }
static void esp_restart(void) { longjmp(stop,1); }
'''
USB_MAIN = r'''
static TaskHandle_t xTaskCreateStatic(void (*fn)(void *),const char *name,unsigned size,void *arg,
                                      unsigned priority,StackType_t *stack,StaticTask_t *tcb) {
    assert(fn==reboot_to_download_task && name && size==4096 && !arg && priority==5);
    assert(stack==s_reset_stack && tcb==&s_reset_tcb); return (void*)1;
}
int main(void) {
    s_reset_task=xTaskCreateStatic(reboot_to_download_task,"usb_reset",sizeof(s_reset_stack),NULL,5,s_reset_stack,&s_reset_tcb);
    assert(request_reboot("test",true)==ESP_OK && notified==1 && s_rebooting);
    assert(request_reboot("duplicate",false)==ESP_ERR_INVALID_STATE && notified==1);
    if(!setjmp(stop)) reboot_to_download_task(NULL);
    assert(forced==1 && !cleared && prepared==1 && quiesced==1 && disconnected==1);
    s_rebooting=false; takes=0;
    assert(request_reboot("restart",false)==ESP_OK);
    if(!setjmp(stop)) reboot_to_download_task(NULL);
    assert(cleared==1 && forced==1);
    s_rebooting=false; takes=0; fail_power=1;
    assert(request_reboot("refuse",true)==ESP_OK);
    int result=setjmp(stop); if(!result) reboot_to_download_task(NULL);
    assert(result==2 && !s_rebooting && !s_download_requested);
    assert(prepared==2 && quiesced==2 && disconnected==2);
    s_reset_task=NULL;
    assert(request_reboot("not initialized",true)==ESP_ERR_INVALID_STATE);
    return 0;
}
'''


class RecoveryGuardTest(unittest.TestCase):
    def test_idempotent_pinned_build_copy(self):
        before = UPSTREAM.read_bytes()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            output = root / "lcd_ppa.c"
            generate(UPSTREAM, output, root)
            stamp = output.stat().st_mtime_ns
            generate(UPSTREAM, output, root)
            self.assertEqual(stamp, output.stat().st_mtime_ns)
            self.assertEqual(before, UPSTREAM.read_bytes())
            self.assertIn("return NULL;", output.read_text())
            # Factory error paths only; the proven rotation code is identical.
            marker = "void lvgl_port_ppa_delete"
            self.assertEqual(UPSTREAM.read_text().split(marker)[1], output.read_text().split(marker)[1])

    def test_unreviewed_upstream_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            changed = root / "changed.c"
            changed.write_text(UPSTREAM.read_text() + "\n// upstream upgrade\n")
            result = generate(changed, root / "lcd_ppa.c", root, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unreviewed", result.stderr)

    def test_actual_ppa_factory_failure_paths(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            output = root / "lcd_ppa.c"
            generate(UPSTREAM, output, root)
            code = output.read_text()
            factory = code[code.index("struct lvgl_port_ppa_t"):code.index("void lvgl_port_ppa_delete")]
            compile_run(PPA_PRELUDE + factory + PPA_MAIN, root)

    def test_actual_reserved_usb_recovery(self):
        code = (PROJECT / "main/bsp_usb.c").read_text(encoding="utf-8")
        storage = code[code.index("static volatile bool s_rebooting;"):code.index("static uint32_t      s_boot_id;")]
        functions = code[code.index("static void reboot_to_download_task"):code.index("static void request_download")]
        # Production request path may not regain a dependency on heap allocation.
        self.assertNotIn("xTaskCreate(", functions)
        self.assertNotIn("malloc(", functions)
        functions = functions.replace("    if (s_rebooting ||", "    (void)why;\n    if (s_rebooting ||")
        with tempfile.TemporaryDirectory() as td:
            compile_run(USB_PRELUDE + storage + functions + USB_MAIN, Path(td))


if __name__ == "__main__":
    unittest.main()
