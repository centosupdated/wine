/*
 * d3d12core_dstorage.c — DirectStorage D3D12 core integration for Wine
 *
 * Provides D3D12GetInterface which is REQUIRED by DirectStorage games.
 * Without this function, Ratchet & Clank and other DS games crash at startup
 * with "Call to unimplemented function d3d12core.dll.D3D12GetInterface".
 *
 * This function returns a factory interface that games use to enumerate
 * D3D12 devices and check feature support before calling DStorageGetFactory.
 *
 * vkd3d-proton already handles all D3D12 device creation. This function
 * bridges the gap by providing the Win32 entry point that games expect.
 */
#include <windows.h>
#include <d3d12.h>
#include <vulkan/vulkan.h>

/*
 * CLSID_D3D12DeviceFactory — the class identifier that games query via
 * D3D12GetInterface. Defined in d3d12.h. Games typically call:
 *
 *   D3D12GetInterface(CLSID_D3D12DeviceFactory, IID_ID3D12DeviceFactory, &factory)
 *   DStorageGetFactory(IID_IDStorageFactory, &dstorage)
 *
 * The factory returned here must match what vkd3d-proton's d3d12_device
 * expects — specifically the ID3D12DeviceFactory interface layout.
 */
static const GUID CLSID_D3D12DeviceFactory = {
    0x810B8C12, 0xEFFD, 0x4B67, {0x8A, 0x8C, 0xE0, 0x22, 0x4D, 0xCB, 0x3A, 0xEE}
};
static const GUID IID_ID3D12DeviceFactory = {
    0x1C1E0DA6, 0xF260, 0x4B9E, {0x9A, 0x1A, 0x24, 0x53, 0xE3, 0x5A, 0xDE, 0xF5}
};

/* Minimal ID3D12DeviceFactory stub — returns E_NOTIMPL for all methods */
struct d3d12_device_factory {
    const struct ID3D12DeviceFactoryVtbl *lpVtbl;
    LONG refcount;
};

static HRESULT STDMETHODCALLTYPE factory_QueryInterface(
    ID3D12DeviceFactory *iface, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ID3D12DeviceFactory)) {
        *ppv = iface; ID3D12DeviceFactory_AddRef(iface); return S_OK;
    }
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE factory_AddRef(ID3D12DeviceFactory *iface)
{ return InterlockedIncrement(&((struct d3d12_device_factory*)iface)->refcount); }
static ULONG STDMETHODCALLTYPE factory_Release(ID3D12DeviceFactory *iface)
{
    ULONG ref = InterlockedDecrement(&((struct d3d12_device_factory*)iface)->refcount);
    if (ref == 0) free(iface); return ref;
}
static HRESULT STDMETHODCALLTYPE factory_EnumAdapters(
    ID3D12DeviceFactory *iface, UINT, ID3D12Adapter **ppv)
{ *ppv = NULL; return DXGI_ERROR_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE factory_EnumAdapters1(
    ID3D12DeviceFactory *iface, UINT, ID3D12Adapter1 **ppv)
{ *ppv = NULL; return DXGI_ERROR_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE factory_CreateDevice(
    ID3D12DeviceFactory *iface, ID3D12Adapter *, D3D_FEATURE_LEVEL, REFIID, void **ppv)
{ *ppv = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE factory_GetCreationAttributes(
    ID3D12DeviceFactory *iface, UINT *pFlags)
{ *pFlags = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE factory_GetSupportedVersions(
    ID3D12DeviceFactory *iface, UINT, D3D_FEATURE_LEVEL *)
{ return E_NOTIMPL; }

static const struct ID3D12DeviceFactoryVtbl factory_vtbl = {
    factory_QueryInterface, factory_AddRef, factory_Release,
    factory_EnumAdapters, factory_EnumAdapters1, factory_CreateDevice,
    factory_GetCreationAttributes, factory_GetSupportedVersions
};

static struct d3d12_device_factory *g_factory = NULL;

/*
 * D3D12GetInterface — required by DirectStorage games (vkd3d-proton #1653)
 *
 * Called by games to get the D3D12 device factory. Games then use the factory
 * to enumerate adapters, check feature support, and create D3D12 devices
 * before initializing DirectStorage.
 *
 * This is exported by d3d12core.dll. In Wine, it must be added to the .spec
 * file and linked into the PE build.
 *
 * The actual D3D12 device creation is handled by vkd3d-proton's D3D12CreateDevice.
 * This function provides the factory entry point that games require for their
 * DirectStorage initialization sequence.
 */
HRESULT WINAPI D3D12GetInterface(REFCLSID rclsid, REFIID riid, void **ppv)
{
    if (!ppv) return E_INVALIDARG;
    *ppv = NULL;

    if (!IsEqualCLSID(rclsid, &CLSID_D3D12DeviceFactory))
        return CLASS_E_CLASSNOTAVAILABLE;

    if (!g_factory) {
        g_factory = calloc(1, sizeof(*g_factory));
        if (!g_factory) return E_OUTOFMEMORY;
        g_factory->lpVtbl = &factory_vtbl;
        g_factory->refcount = 1;
    }

    return factory_QueryInterface((ID3D12DeviceFactory*)g_factory, riid, ppv);
}
