/*
 * dstorage_main.c — Wine dstorage.dll PE shim
 *
 * When compiled as a PE DLL with winegcc for Wine/Proton, this file
 * uses LoadLibrary/GetProcAddress to resolve libdstorage.so functions.
 * Wine maps these to the native Linux ELF loading layer automatically.
 *
 * Build for Wine:
 *   winegcc -m64 -shared -o dstorage.dll dstorage_main.c
 *
 * Build for Linux (testing):
 *   gcc -shared -o libdstorage_shim.so dstorage_main.c -ldl
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

/* Type forward declarations matching dstorage_api.h but returning HRESULT (Windows ABI) */
typedef struct DSTORAGE_CONFIGURATION { uint32_t a; int32_t b; int32_t c; int32_t d; int32_t e; int32_t f; int32_t g; } DSTORAGE_CONFIGURATION;
typedef struct DSTORAGE_CONFIGURATION1 { uint32_t a; int32_t b; int32_t c; int32_t d; int32_t e; int32_t f; int32_t g; int32_t h; } DSTORAGE_CONFIGURATION1;

/* Function pointer types for libdstorage.so APIs */
typedef int32_t (*DStorageSetConfiguration_t)(const void*);
typedef int32_t (*DStorageSetConfiguration1_t)(const void*);
typedef int32_t (*DStorageGetFactory_t)(const void*, void**);
typedef int32_t (*DStorageCreateCompressionCodec_t)(uint8_t, uint32_t, const void*, void**);

static HMODULE g_lib = NULL;
static int g_loaded = 0;

static void ensure_lib(void) {
    if (g_loaded) return;
    g_loaded = 1;
    /* In Wine: LoadLibrary maps to the native .so via Wine's ELF loader.
     * Wine translates LoadLibrary("libdstorage.so") to dlopen("libdstorage.so"). */
    const char* paths[] = {
        "libdstorage.so",
        "/usr/lib/libdstorage.so",
        "/usr/local/lib/libdstorage.so",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        g_lib = LoadLibraryA(paths[i]);
        if (g_lib) break;
    }
}

static void* get_sym(const char* name) {
    if (!g_lib) return NULL;
    return (void*)GetProcAddress(g_lib, name);
}

/* ---- Exported API ---- */

int32_t WINAPI DStorageSetConfiguration(const DSTORAGE_CONFIGURATION* cfg) {
    ensure_lib();
    DStorageSetConfiguration_t fn = (DStorageSetConfiguration_t)get_sym("DStorageSetConfiguration");
    return fn ? (HRESULT)fn(cfg) : E_FAIL;
}

int32_t WINAPI DStorageSetConfiguration1(const DSTORAGE_CONFIGURATION1* cfg) {
    ensure_lib();
    DStorageSetConfiguration1_t fn = (DStorageSetConfiguration1_t)get_sym("DStorageSetConfiguration1");
    return fn ? (int32_t)fn(cfg) : E_FAIL;
}

int32_t WINAPI DStorageGetFactory(REFIID riid, void** ppv) {
    ensure_lib();
    DStorageGetFactory_t fn = (DStorageGetFactory_t)get_sym("DStorageGetFactory");
    return fn ? (int32_t)fn(riid, ppv) : E_FAIL;
}

int32_t WINAPI DStorageCreateCompressionCodec(
    uint8_t format, uint32_t numThreads, REFIID riid, void** ppv) {
    ensure_lib();
    DStorageCreateCompressionCodec_t fn = (DStorageCreateCompressionCodec_t)get_sym("DStorageCreateCompressionCodec");
    return fn ? (int32_t)fn(format, numThreads, riid, ppv) : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH && g_lib) {
        FreeLibrary(g_lib);
        g_lib = NULL;
    }
    return TRUE;
}
