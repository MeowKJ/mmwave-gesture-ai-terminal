"""Compile the actual P4 SR allocator with capability-aware heap doubles.

This is ABI/allocation regression coverage, not neural-network or acoustic QA.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]
CC = os.environ.get("CC") or shutil.which("gcc") or "C:/Apps/Env/mingw64/bin/gcc.exe"
SOURCE = PROJECT / "main/meoave_sr_alloc.c"

HEAP_HEADER = """
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1u
#define MALLOC_CAP_SPIRAM 2u
#define MALLOC_CAP_8BIT 4u
#define MALLOC_CAP_SIMD 8u
void *heap_caps_calloc(size_t, size_t, unsigned);
"""

HARNESS = r'''
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
void *__wrap_dl_lib_calloc(int,int,int);
void *__wrap_dl_lib_calloc_psram(int,int,int);
static unsigned calls, caps_seen[2], failures;
void *heap_caps_calloc(size_t count, size_t size, unsigned caps) {
    assert(count==1 && (caps&MALLOC_CAP_SIMD) && (caps&MALLOC_CAP_8BIT));
    assert(calls<2); caps_seen[calls++]=caps;
    if(failures) { failures--; return NULL; }
    if(size>8192) return NULL;
    return calloc(count,size);
}
/* Same prefix/free ABI as the hash-pinned 32-bit vendor archive. */
static void vendor_free(void *p) { if(p) free(((void **)p)[-1]); }
static void begin(unsigned fail) { calls=0; failures=fail; }
static void check(void *p, size_t bytes, unsigned alignment) {
    assert(p && !((uintptr_t)p % alignment));
    for(size_t i=0;i<bytes;i++) assert(((unsigned char*)p)[i]==0);
    void *raw=((void**)p)[-1];
    assert(raw && (uintptr_t)p>=(uintptr_t)raw+sizeof(void*));
    assert((uintptr_t)p<(uintptr_t)raw+sizeof(void*)+alignment);
    vendor_free(p);
}
int main(void) {
    unsigned internal=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT|MALLOC_CAP_SIMD;
    unsigned external=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT|MALLOC_CAP_SIMD;
    int aligns[]={0,1,2,4,8,16,32,64,256};
    for(unsigned i=0;i<sizeof(aligns)/sizeof(*aligns);i++) {
        unsigned a=aligns[i]<16?16:(unsigned)aligns[i];
        begin(0); check(__wrap_dl_lib_calloc(17,2,aligns[i]),34,a);
        assert(calls==1 && caps_seen[0]==internal);
        begin(0); check(__wrap_dl_lib_calloc_psram(17,2,aligns[i]),34,a);
        assert(calls==1 && caps_seen[0]==external);
    }
    begin(0); check(__wrap_dl_lib_calloc(2048,2,16),4096,16);
    assert(calls==1 && caps_seen[0]==external);
    begin(1); check(__wrap_dl_lib_calloc(32,2,16),64,16);
    assert(calls==2 && caps_seen[0]==internal && caps_seen[1]==external);
    begin(1); check(__wrap_dl_lib_calloc_psram(32,2,16),64,16);
    assert(calls==2 && caps_seen[0]==external && caps_seen[1]==internal);
    begin(2); assert(!__wrap_dl_lib_calloc(32,2,16) && calls==2);
    begin(2); assert(!__wrap_dl_lib_calloc_psram(32,2,16) && calls==2);
    begin(0);
    assert(!__wrap_dl_lib_calloc(-1,2,16));
    assert(!__wrap_dl_lib_calloc(1,-2,16));
    assert(!__wrap_dl_lib_calloc(0,2,16));
    assert(!__wrap_dl_lib_calloc(1,0,16));
    assert(!__wrap_dl_lib_calloc(1,2,-1));
    assert(!__wrap_dl_lib_calloc(1,2,3));
    assert(!__wrap_dl_lib_calloc_psram(1,2,24));
    assert(calls==0);
    /* Never wrap a giant product into a small accepted allocation. */
    begin(0); assert(!__wrap_dl_lib_calloc(INT_MAX,INT_MAX,16));
    return 0;
}
'''


class SrAllocTest(unittest.TestCase):
    def test_capabilities_alignment_zeroing_failure_and_free_abi(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "sdkconfig.h").write_text("#define CONFIG_IDF_TARGET_ESP32P4 1\n"
                                               "#define CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL 1024\n")
            (root / "esp_heap_caps.h").write_text(HEAP_HEADER)
            (root / "dl_lib.h").write_text("")
            (root / "test.c").write_text(HARNESS)
            exe = root / "test.exe"
            subprocess.run([CC, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-DMEOAVE_ESPSR_LINKED=1", "-I", str(root), str(SOURCE),
                            str(root / "test.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)

    def test_no_system_allocator_or_global_heap_policy_override(self):
        source = SOURCE.read_text()
        cmake = (PROJECT / "cmake/SrSimdGuard.cmake").read_text()
        self.assertIn("--wrap=dl_lib_calloc", cmake)
        self.assertNotIn("--wrap=calloc", cmake)
        self.assertNotIn("--wrap=malloc", cmake)
        self.assertNotIn("__wrap_dl_lib_free", source)
        self.assertIn("07174a92fae60884998186a843e06e9955b78c2dc4d491fe2b6de2068f6eedd2", cmake)
        self.assertIn("unreviewed dl_lib", cmake)


if __name__ == "__main__":
    unittest.main()
