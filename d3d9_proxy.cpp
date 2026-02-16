/**
 * Mirror's Edge Camera Proxy for RTX Remix
 *
 * This proxy DLL intercepts D3D9 calls, extracts camera matrices from
 * vertex shader constants, and provides them to RTX Remix via SetTransform().
 *
 * Mirror's Edge uses Unreal Engine 3 which typically uses:
 *   c0-c3:   LocalToWorld (World) matrix
 *   c4-c7:   ViewProjection or View matrix
 *   c8-c11:  WorldViewProjection matrix
 *
 * This version has LOGGING ENABLED to discover the actual register layout.
 *
 * Build with Visual Studio Developer Command Prompt (x86):
 *   build.bat
 *
 * Setup:
 * 1. Rename Remix's d3d9.dll to d3d9_remix.dll
 * 2. Place this compiled d3d9.dll in the game's Binaries folder
 * 3. Run game and check camera_proxy.log for register analysis
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cmath>

#pragma comment(lib, "user32.lib")

// Configuration
struct ProxyConfig {
    // Mirror's Edge confirmed layout:
    // c5-c8 = View matrix (rotation rows + translation)
    // c0-c3 = WorldViewProjection (not used directly)
    int viewMatrixRegister = 5;      // c5-c8 confirmed from log analysis
    int projMatrixRegister = -1;     // No separate projection - synthesize 90deg FOV
    int worldMatrixRegister = -1;    // Not needed
    bool enableLogging = true;
    float minFOV = 0.1f;
    float maxFOV = 2.5f;

    // Reduced logging now that registers are known
    bool logAllConstants = false;    // Disable verbose logging
    bool autoDetectMatrices = false; // Disable - we know the layout now
};

static ProxyConfig g_config;
static HMODULE g_hRemixD3D9 = nullptr;
static FILE* g_logFile = nullptr;
static int g_frameCount = 0;

// Function pointer types
typedef IDirect3D9* (WINAPI* Direct3DCreate9_t)(UINT SDKVersion);
typedef HRESULT (WINAPI* Direct3DCreate9Ex_t)(UINT SDKVersion, IDirect3D9Ex**);
typedef int (WINAPI* D3DPERF_BeginEvent_t)(D3DCOLOR, LPCWSTR);
typedef int (WINAPI* D3DPERF_EndEvent_t)(void);
typedef DWORD (WINAPI* D3DPERF_GetStatus_t)(void);
typedef BOOL (WINAPI* D3DPERF_QueryRepeatFrame_t)(void);
typedef void (WINAPI* D3DPERF_SetMarker_t)(D3DCOLOR, LPCWSTR);
typedef void (WINAPI* D3DPERF_SetOptions_t)(DWORD);
typedef void (WINAPI* D3DPERF_SetRegion_t)(D3DCOLOR, LPCWSTR);

static Direct3DCreate9_t g_origDirect3DCreate9 = nullptr;
static Direct3DCreate9Ex_t g_origDirect3DCreate9Ex = nullptr;
static D3DPERF_BeginEvent_t g_origD3DPERF_BeginEvent = nullptr;
static D3DPERF_EndEvent_t g_origD3DPERF_EndEvent = nullptr;
static D3DPERF_GetStatus_t g_origD3DPERF_GetStatus = nullptr;
static D3DPERF_QueryRepeatFrame_t g_origD3DPERF_QueryRepeatFrame = nullptr;
static D3DPERF_SetMarker_t g_origD3DPERF_SetMarker = nullptr;
static D3DPERF_SetOptions_t g_origD3DPERF_SetOptions = nullptr;
static D3DPERF_SetRegion_t g_origD3DPERF_SetRegion = nullptr;

// Logging helper
void LogMsg(const char* fmt, ...) {
    if (!g_config.enableLogging || !g_logFile) return;

    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    fprintf(g_logFile, "\n");
    fflush(g_logFile);
    va_end(args);
}

// Check if matrix values are valid
bool LooksLikeMatrix(const float* data) {
    float sum = 0;
    for (int i = 0; i < 16; i++) {
        if (!isfinite(data[i])) return false;
        sum += fabsf(data[i]);
    }
    if (sum < 0.001f || sum > 100000.0f) return false;
    return true;
}

// Extract FOV from projection matrix
float ExtractFOV(const D3DMATRIX& proj) {
    if (fabsf(proj._22) < 0.001f) return 0;
    return 2.0f * atanf(1.0f / proj._22);
}

// Check if matrix looks like projection
bool LooksLikeProjection(const D3DMATRIX& m) {
    // Check for typical projection structure
    if (fabsf(m._12) > 0.01f || fabsf(m._13) > 0.01f || fabsf(m._14) > 0.01f) return false;
    if (fabsf(m._21) > 0.01f || fabsf(m._23) > 0.01f || fabsf(m._24) > 0.01f) return false;
    if (fabsf(m._31) > 0.01f || fabsf(m._32) > 0.01f) return false;
    if (fabsf(m._11) < 0.01f || fabsf(m._22) < 0.01f) return false;

    float fov = ExtractFOV(m);
    if (fov < g_config.minFOV || fov > g_config.maxFOV) return false;

    return true;
}

// Create a standard perspective projection matrix
void CreateProjectionMatrix(D3DMATRIX* out, float fovY, float aspect, float zNear, float zFar) {
    float yScale = 1.0f / tanf(fovY / 2.0f);
    float xScale = yScale / aspect;
    memset(out, 0, sizeof(D3DMATRIX));
    out->_11 = xScale;
    out->_22 = yScale;
    out->_33 = zFar / (zFar - zNear);
    out->_34 = 1.0f;
    out->_43 = -zNear * zFar / (zFar - zNear);
}

// Create an identity matrix
void CreateIdentityMatrix(D3DMATRIX* out) {
    memset(out, 0, sizeof(D3DMATRIX));
    out->_11 = out->_22 = out->_33 = out->_44 = 1.0f;
}

// Check if matrix looks like view matrix (orthonormal rotation + translation)
bool LooksLikeView(const D3DMATRIX& m) {
    float row0len = sqrtf(m._11*m._11 + m._12*m._12 + m._13*m._13);
    float row1len = sqrtf(m._21*m._21 + m._22*m._22 + m._23*m._23);
    float row2len = sqrtf(m._31*m._31 + m._32*m._32 + m._33*m._33);

    if (fabsf(row0len - 1.0f) > 0.1f) return false;
    if (fabsf(row1len - 1.0f) > 0.1f) return false;
    if (fabsf(row2len - 1.0f) > 0.1f) return false;

    if (fabsf(m._14) > 0.01f || fabsf(m._24) > 0.01f || fabsf(m._34) > 0.01f) return false;
    if (fabsf(m._44 - 1.0f) > 0.01f) return false;

    return true;
}

// Check if matrix could be ViewProjection (has projection-like characteristics but rotation too)
bool LooksLikeViewProjection(const D3DMATRIX& m) {
    // ViewProj will have large values due to projection multiplication
    // Check for non-zero values in typical projection positions
    if (fabsf(m._34) < 0.5f) return false;  // Perspective divide indicator
    if (fabsf(m._44) > 0.1f) return false;  // Should be ~0 for perspective

    // Should have some rotation component
    float magnitude = sqrtf(m._11*m._11 + m._12*m._12 + m._13*m._13);
    if (magnitude < 0.1f) return false;

    return true;
}

// Try to extract View from ViewProjection by removing projection component
void ExtractViewFromViewProjection(const D3DMATRIX& vp, D3DMATRIX* viewOut) {
    CreateIdentityMatrix(viewOut);

    // The ViewProjection combines View and Projection
    // We need to approximately invert the projection influence
    // For a typical projection, _11 and _22 contain the FOV scaling

    float r0len = sqrtf(vp._11*vp._11 + vp._12*vp._12 + vp._13*vp._13);
    float r1len = sqrtf(vp._21*vp._21 + vp._22*vp._22 + vp._23*vp._23);
    float r2len = sqrtf(vp._31*vp._31 + vp._32*vp._32 + vp._33*vp._33);

    if (r0len > 0.001f && r1len > 0.001f && r2len > 0.001f) {
        // Normalize to approximate rotation
        viewOut->_11 = vp._11 / r0len; viewOut->_12 = vp._12 / r0len; viewOut->_13 = vp._13 / r0len;
        viewOut->_21 = vp._21 / r1len; viewOut->_22 = vp._22 / r1len; viewOut->_23 = vp._23 / r1len;
        viewOut->_31 = vp._31 / r2len; viewOut->_32 = vp._32 / r2len; viewOut->_33 = vp._33 / r2len;

        // Translation approximation
        viewOut->_41 = vp._14 / r0len;
        viewOut->_42 = vp._24 / r1len;
        viewOut->_43 = vp._34 / r2len;
    }
}

// Forward declarations
class WrappedD3D9Device;
class WrappedD3D9;

/**
 * Wrapped IDirect3DDevice9 - intercepts SetVertexShaderConstantF
 */
class WrappedD3D9Device : public IDirect3DDevice9 {
private:
    IDirect3DDevice9* m_real;
    D3DMATRIX m_lastViewMatrix;
    D3DMATRIX m_lastProjMatrix;
    D3DMATRIX m_pendingViewMatrix;  // Captured during frame
    bool m_hasView = false;
    bool m_hasProj = false;
    bool m_pendingViewUpdate = false;  // Flag for once-per-frame update
    bool m_capturedThisFrame = false;  // Only capture FIRST camera per frame
    int m_constantLogThrottle = 0;
    int m_loggedThisFrame = 0;

public:
    WrappedD3D9Device(IDirect3DDevice9* real) : m_real(real) {
        memset(&m_lastViewMatrix, 0, sizeof(D3DMATRIX));
        memset(&m_lastProjMatrix, 0, sizeof(D3DMATRIX));
        LogMsg("WrappedD3D9Device created, wrapping device at %p", real);
    }

    ~WrappedD3D9Device() {
        LogMsg("WrappedD3D9Device destroyed");
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override {
        HRESULT hr = m_real->QueryInterface(riid, ppvObj);
        return hr;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_real->AddRef();
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = m_real->Release();
        if (count == 0) {
            delete this;
        }
        return count;
    }

    // The key interception point
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(
        UINT StartRegister,
        const float* pConstantData,
        UINT Vector4fCount) override
    {
        // MIRROR'S EDGE: View matrix is at c5-c8
        // Format: c5=RotRow0, c6=RotRow1, c7=RotRow2, c8=[Tx,Ty,Tz,1]
        if (StartRegister <= 5 && StartRegister + Vector4fCount >= 9) {
            // c5-c8 is within this update
            int offset = (5 - StartRegister) * 4;
            const float* viewData = pConstantData + offset;

            // Validate it looks like a proper view matrix
            float row0len = sqrtf(viewData[0]*viewData[0] + viewData[1]*viewData[1] + viewData[2]*viewData[2]);
            float row1len = sqrtf(viewData[4]*viewData[4] + viewData[5]*viewData[5] + viewData[6]*viewData[6]);
            float row2len = sqrtf(viewData[8]*viewData[8] + viewData[9]*viewData[9] + viewData[10]*viewData[10]);

            // Check for orthonormal rotation (all row lengths ~1.0) and valid w components
            bool isValidView = (fabsf(row0len - 1.0f) < 0.15f) &&
                              (fabsf(row1len - 1.0f) < 0.15f) &&
                              (fabsf(row2len - 1.0f) < 0.15f) &&
                              (fabsf(viewData[3]) < 0.01f) &&   // c5.w = 0
                              (fabsf(viewData[7]) < 0.01f) &&   // c6.w = 0
                              (fabsf(viewData[11]) < 0.01f) &&  // c7.w = 0
                              (fabsf(viewData[15] - 1.0f) < 0.01f); // c8.w = 1

            if (isValidView) {
                // Copy the view matrix
                D3DMATRIX viewMat;
                memcpy(&viewMat, viewData, sizeof(D3DMATRIX));

                // Check if this is a real 3D camera (not UI - has non-trivial translation)
                float transMag = sqrtf(viewMat._41*viewMat._41 + viewMat._42*viewMat._42 + viewMat._43*viewMat._43);

                // Only use if translation magnitude suggests 3D world (> 100 units typically)
                // AND we haven't captured a camera this frame yet (avoid shadow/reflection cameras)
                if (transMag > 50.0f && !m_capturedThisFrame) {
                    // Store pending view - will apply once per frame in Present()
                    memcpy(&m_pendingViewMatrix, &viewMat, sizeof(D3DMATRIX));
                    m_pendingViewUpdate = true;
                    m_capturedThisFrame = true;  // Only capture FIRST camera per frame

                    // Create projection if we don't have one (90 degree FOV, matching ME's typical FOV)
                    if (!m_hasProj) {
                        CreateProjectionMatrix(&m_lastProjMatrix, 1.5708f, 16.0f/9.0f, 10.0f, 100000.0f);
                        m_real->SetTransform(D3DTS_PROJECTION, &m_lastProjMatrix);
                        m_hasProj = true;
                    }

                    if (!m_hasView) {
                        LogMsg("ME: Found VIEW at c5-c8, trans=[%.1f, %.1f, %.1f]",
                               viewMat._41, viewMat._42, viewMat._43);
                        m_hasView = true;
                        // Set initial view immediately
                        memcpy(&m_lastViewMatrix, &viewMat, sizeof(D3DMATRIX));
                        m_real->SetTransform(D3DTS_VIEW, &m_lastViewMatrix);
                        D3DMATRIX identity;
                        CreateIdentityMatrix(&identity);
                        m_real->SetTransform(D3DTS_WORLD, &identity);
                    }
                }
            }
        }

        // Optional: Log for debugging (throttled)
        if (g_config.logAllConstants && m_constantLogThrottle == 0 && Vector4fCount >= 4) {
            if (m_loggedThisFrame < 10) {
                m_loggedThisFrame++;
                LogMsg("Frame %d: c%d-%d (%d vec4s)", g_frameCount,
                       StartRegister, StartRegister + Vector4fCount - 1, Vector4fCount);
            }
        }

        return m_real->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
    }

    // Present - per-frame operations
    HRESULT STDMETHODCALLTYPE Present(const RECT* pSourceRect, const RECT* pDestRect,
                                       HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) override {
        // Apply pending view matrix ONCE per frame (prevents constant camera cut detection)
        if (m_pendingViewUpdate && m_hasView) {
            memcpy(&m_lastViewMatrix, &m_pendingViewMatrix, sizeof(D3DMATRIX));
            m_real->SetTransform(D3DTS_VIEW, &m_lastViewMatrix);
            m_pendingViewUpdate = false;
        }

        // Reset for next frame - allow capturing first camera again
        m_capturedThisFrame = false;

        g_frameCount++;
        m_loggedThisFrame = 0;

        // Throttle constant logging after first 10 frames (enough to see patterns)
        if (g_config.logAllConstants && g_frameCount > 10) {
            m_constantLogThrottle = (m_constantLogThrottle + 1) % 300; // Log every 300 frames
        }

        // Log periodic status
        if (g_frameCount % 300 == 0) {
            LogMsg("=== Frame %d Status: hasView=%d hasProj=%d ===",
                   g_frameCount, m_hasView, m_hasProj);
            if (m_hasView) {
                LogMsg("  View matrix translation: [%.1f, %.1f, %.1f]",
                       m_lastViewMatrix._41, m_lastViewMatrix._42, m_lastViewMatrix._43);
            }
        }

        return m_real->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    }

    // All other methods pass through
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return m_real->TestCooperativeLevel(); }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override { return m_real->GetAvailableTextureMem(); }
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override { return m_real->EvictManagedResources(); }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** ppD3D9) override { return m_real->GetDirect3D(ppD3D9); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* pCaps) override { return m_real->GetDeviceCaps(pCaps); }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) override { return m_real->GetDisplayMode(iSwapChain, pMode); }
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* pParameters) override { return m_real->GetCreationParameters(pParameters); }
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) override { return m_real->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap); }
    void STDMETHODCALLTYPE SetCursorPosition(int X, int Y, DWORD Flags) override { m_real->SetCursorPosition(X, Y, Flags); }
    BOOL STDMETHODCALLTYPE ShowCursor(BOOL bShow) override { return m_real->ShowCursor(bShow); }
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain) override { return m_real->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain); }
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) override { return m_real->GetSwapChain(iSwapChain, pSwapChain); }
    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override { return m_real->GetNumberOfSwapChains(); }
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) override { return m_real->Reset(pPresentationParameters); }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) override { return m_real->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer); }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) override { return m_real->GetRasterStatus(iSwapChain, pRasterStatus); }
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL bEnableDialogs) override { return m_real->SetDialogBoxMode(bEnableDialogs); }
    void STDMETHODCALLTYPE SetGammaRamp(UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP* pRamp) override { m_real->SetGammaRamp(iSwapChain, Flags, pRamp); }
    void STDMETHODCALLTYPE GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp) override { m_real->GetGammaRamp(iSwapChain, pRamp); }
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) override { return m_real->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle) override { return m_real->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle) override { return m_real->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) override { return m_real->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle) override { return m_real->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override { return m_real->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override { return m_real->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* pSourceSurface, const RECT* pSourceRect, IDirect3DSurface9* pDestinationSurface, const POINT* pDestPoint) override { return m_real->UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint); }
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) override { return m_real->UpdateTexture(pSourceTexture, pDestinationTexture); }
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) override { return m_real->GetRenderTargetData(pRenderTarget, pDestSurface); }
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface) override { return m_real->GetFrontBufferData(iSwapChain, pDestSurface); }
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* pSourceSurface, const RECT* pSourceRect, IDirect3DSurface9* pDestSurface, const RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter) override { return m_real->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter); }
    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* pSurface, const RECT* pRect, D3DCOLOR color) override { return m_real->ColorFill(pSurface, pRect, color); }
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override { return m_real->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) override { return m_real->SetRenderTarget(RenderTargetIndex, pRenderTarget); }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) override { return m_real->GetRenderTarget(RenderTargetIndex, ppRenderTarget); }
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) override { return m_real->SetDepthStencilSurface(pNewZStencil); }
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) override { return m_real->GetDepthStencilSurface(ppZStencilSurface); }
    HRESULT STDMETHODCALLTYPE BeginScene() override {
        // Set last known camera - Remix needs this during draw calls
        // Using m_lastViewMatrix (stable, from previous frame's Present) not pending
        if (m_hasView && m_hasProj) {
            D3DMATRIX identity;
            CreateIdentityMatrix(&identity);
            m_real->SetTransform(D3DTS_WORLD, &identity);
            m_real->SetTransform(D3DTS_VIEW, &m_lastViewMatrix);
            m_real->SetTransform(D3DTS_PROJECTION, &m_lastProjMatrix);
        }
        return m_real->BeginScene();
    }
    HRESULT STDMETHODCALLTYPE EndScene() override { return m_real->EndScene(); }
    HRESULT STDMETHODCALLTYPE Clear(DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override { return m_real->Clear(Count, pRects, Flags, Color, Z, Stencil); }
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) override { return m_real->SetTransform(State, pMatrix); }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) override { return m_real->GetTransform(State, pMatrix); }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) override { return m_real->MultiplyTransform(State, pMatrix); }
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* pViewport) override { return m_real->SetViewport(pViewport); }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* pViewport) override { return m_real->GetViewport(pViewport); }
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* pMaterial) override { return m_real->SetMaterial(pMaterial); }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* pMaterial) override { return m_real->GetMaterial(pMaterial); }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD Index, const D3DLIGHT9* pLight) override { return m_real->SetLight(Index, pLight); }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT9* pLight) override { return m_real->GetLight(Index, pLight); }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, BOOL Enable) override { return m_real->LightEnable(Index, Enable); }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, BOOL* pEnable) override { return m_real->GetLightEnable(Index, pEnable); }
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD Index, const float* pPlane) override { return m_real->SetClipPlane(Index, pPlane); }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float* pPlane) override { return m_real->GetClipPlane(Index, pPlane); }
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override { return m_real->SetRenderState(State, Value); }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) override { return m_real->GetRenderState(State, pValue); }
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) override { return m_real->CreateStateBlock(Type, ppSB); }
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override { return m_real->BeginStateBlock(); }
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** ppSB) override { return m_real->EndStateBlock(ppSB); }
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9* pClipStatus) override { return m_real->SetClipStatus(pClipStatus); }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* pClipStatus) override { return m_real->GetClipStatus(pClipStatus); }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) override { return m_real->GetTexture(Stage, ppTexture); }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) override { return m_real->SetTexture(Stage, pTexture); }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) override { return m_real->GetTextureStageState(Stage, Type, pValue); }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override { return m_real->SetTextureStageState(Stage, Type, Value); }
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) override { return m_real->GetSamplerState(Sampler, Type, pValue); }
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override { return m_real->SetSamplerState(Sampler, Type, Value); }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* pNumPasses) override { return m_real->ValidateDevice(pNumPasses); }
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY* pEntries) override { return m_real->SetPaletteEntries(PaletteNumber, pEntries); }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) override { return m_real->GetPaletteEntries(PaletteNumber, pEntries); }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) override { return m_real->SetCurrentTexturePalette(PaletteNumber); }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* PaletteNumber) override { return m_real->GetCurrentTexturePalette(PaletteNumber); }
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* pRect) override { return m_real->SetScissorRect(pRect); }
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* pRect) override { return m_real->GetScissorRect(pRect); }
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL bSoftware) override { return m_real->SetSoftwareVertexProcessing(bSoftware); }
    BOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override { return m_real->GetSoftwareVertexProcessing(); }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float nSegments) override { return m_real->SetNPatchMode(nSegments); }
    float STDMETHODCALLTYPE GetNPatchMode() override { return m_real->GetNPatchMode(); }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override {
        return m_real->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) override {
        return m_real->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override { return m_real->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride); }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void* pIndexData, D3DFORMAT IndexDataFormat, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override { return m_real->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride); }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags) override { return m_real->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags); }
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl) override { return m_real->CreateVertexDeclaration(pVertexElements, ppDecl); }
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) override { return m_real->SetVertexDeclaration(pDecl); }
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) override { return m_real->GetVertexDeclaration(ppDecl); }
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD FVF) override { return m_real->SetFVF(FVF); }
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* pFVF) override { return m_real->GetFVF(pFVF); }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* pFunction, IDirect3DVertexShader9** ppShader) override { return m_real->CreateVertexShader(pFunction, ppShader); }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pShader) override { return m_real->SetVertexShader(pShader); }
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** ppShader) override { return m_real->GetVertexShader(ppShader); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override { return m_real->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT StartRegister, const int* pConstantData, UINT Vector4iCount) override { return m_real->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override { return m_real->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT StartRegister, const BOOL* pConstantData, UINT BoolCount) override { return m_real->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) override { return m_real->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride) override { return m_real->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride); }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* pOffsetInBytes, UINT* pStride) override { return m_real->GetStreamSource(StreamNumber, ppStreamData, pOffsetInBytes, pStride); }
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT StreamNumber, UINT Setting) override { return m_real->SetStreamSourceFreq(StreamNumber, Setting); }
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT StreamNumber, UINT* pSetting) override { return m_real->GetStreamSourceFreq(StreamNumber, pSetting); }
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* pIndexData) override { return m_real->SetIndices(pIndexData); }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ppIndexData) override { return m_real->GetIndices(ppIndexData); }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* pFunction, IDirect3DPixelShader9** ppShader) override { return m_real->CreatePixelShader(pFunction, ppShader); }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* pShader) override { return m_real->SetPixelShader(pShader); }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppShader) override { return m_real->GetPixelShader(ppShader); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT StartRegister, const float* pConstantData, UINT Vector4fCount) override { return m_real->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override { return m_real->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT StartRegister, const int* pConstantData, UINT Vector4iCount) override { return m_real->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override { return m_real->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT StartRegister, const BOOL* pConstantData, UINT BoolCount) override { return m_real->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) override { return m_real->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT Handle, const float* pNumSegs, const D3DRECTPATCH_INFO* pRectPatchInfo) override { return m_real->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo); }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT Handle, const float* pNumSegs, const D3DTRIPATCH_INFO* pTriPatchInfo) override { return m_real->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo); }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override { return m_real->DeletePatch(Handle); }
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) override { return m_real->CreateQuery(Type, ppQuery); }
};

/**
 * Wrapped IDirect3D9 - intercepts CreateDevice to return wrapped devices
 */
class WrappedD3D9 : public IDirect3D9 {
private:
    IDirect3D9* m_real;

public:
    WrappedD3D9(IDirect3D9* real) : m_real(real) {
        LogMsg("WrappedD3D9 created, wrapping IDirect3D9 at %p", real);
    }

    ~WrappedD3D9() {
        LogMsg("WrappedD3D9 destroyed");
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override {
        return m_real->QueryInterface(riid, ppvObj);
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_real->AddRef();
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = m_real->Release();
        if (count == 0) {
            delete this;
        }
        return count;
    }

    // IDirect3D9 methods
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* pInitializeFunction) override {
        return m_real->RegisterSoftwareDevice(pInitializeFunction);
    }

    UINT STDMETHODCALLTYPE GetAdapterCount() override {
        return m_real->GetAdapterCount();
    }

    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) override {
        return m_real->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
    }

    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override {
        return m_real->GetAdapterModeCount(Adapter, Format);
    }

    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override {
        return m_real->EnumAdapterModes(Adapter, Format, Mode, pMode);
    }

    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) override {
        return m_real->GetAdapterDisplayMode(Adapter, pMode);
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed) override {
        return m_real->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed);
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override {
        return m_real->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels) override {
        return m_real->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels);
    }

    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override {
        return m_real->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) override {
        return m_real->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat);
    }

    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override {
        return m_real->GetDeviceCaps(Adapter, DeviceType, pCaps);
    }

    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override {
        return m_real->GetAdapterMonitor(Adapter);
    }

    // The key interception - wrap the device!
    HRESULT STDMETHODCALLTYPE CreateDevice(
        UINT Adapter,
        D3DDEVTYPE DeviceType,
        HWND hFocusWindow,
        DWORD BehaviorFlags,
        D3DPRESENT_PARAMETERS* pPresentationParameters,
        IDirect3DDevice9** ppReturnedDeviceInterface) override
    {
        LogMsg("CreateDevice called - Adapter: %d, DeviceType: %d", Adapter, DeviceType);

        IDirect3DDevice9* realDevice = nullptr;
        HRESULT hr = m_real->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                          pPresentationParameters, &realDevice);

        if (SUCCEEDED(hr) && realDevice) {
            LogMsg("CreateDevice succeeded, wrapping device");
            *ppReturnedDeviceInterface = new WrappedD3D9Device(realDevice);
        } else {
            LogMsg("CreateDevice failed with HRESULT: 0x%08X", hr);
            *ppReturnedDeviceInterface = nullptr;
        }

        return hr;
    }
};

/**
 * Wrapped IDirect3D9Ex - for games that use Direct3DCreate9Ex
 */
class WrappedD3D9Ex : public IDirect3D9Ex {
private:
    IDirect3D9Ex* m_real;

public:
    WrappedD3D9Ex(IDirect3D9Ex* real) : m_real(real) {
        LogMsg("WrappedD3D9Ex created, wrapping IDirect3D9Ex at %p", real);
    }

    ~WrappedD3D9Ex() {
        LogMsg("WrappedD3D9Ex destroyed");
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override {
        return m_real->QueryInterface(riid, ppvObj);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_real->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = m_real->Release();
        if (count == 0) delete this;
        return count;
    }

    // IDirect3D9 methods
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* pInitializeFunction) override { return m_real->RegisterSoftwareDevice(pInitializeFunction); }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return m_real->GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) override { return m_real->GetAdapterIdentifier(Adapter, Flags, pIdentifier); }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override { return m_real->GetAdapterModeCount(Adapter, Format); }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override { return m_real->EnumAdapterModes(Adapter, Format, Mode, pMode); }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) override { return m_real->GetAdapterDisplayMode(Adapter, pMode); }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed) override { return m_real->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override { return m_real->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat); }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels) override { return m_real->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels); }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override { return m_real->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) override { return m_real->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override { return m_real->GetDeviceCaps(Adapter, DeviceType, pCaps); }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override { return m_real->GetAdapterMonitor(Adapter); }

    HRESULT STDMETHODCALLTYPE CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
        D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface) override
    {
        LogMsg("CreateDevice (via Ex) called");
        IDirect3DDevice9* realDevice = nullptr;
        HRESULT hr = m_real->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, &realDevice);
        if (SUCCEEDED(hr) && realDevice) {
            *ppReturnedDeviceInterface = new WrappedD3D9Device(realDevice);
        } else {
            *ppReturnedDeviceInterface = nullptr;
        }
        return hr;
    }

    // IDirect3D9Ex methods
    UINT STDMETHODCALLTYPE GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter) override { return m_real->GetAdapterModeCountEx(Adapter, pFilter); }
    HRESULT STDMETHODCALLTYPE EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter, UINT Mode, D3DDISPLAYMODEEX* pMode) override { return m_real->EnumAdapterModesEx(Adapter, pFilter, Mode, pMode); }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation) override { return m_real->GetAdapterDisplayModeEx(Adapter, pMode, pRotation); }
    HRESULT STDMETHODCALLTYPE GetAdapterLUID(UINT Adapter, LUID* pLUID) override { return m_real->GetAdapterLUID(Adapter, pLUID); }

    HRESULT STDMETHODCALLTYPE CreateDeviceEx(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
        D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode,
        IDirect3DDevice9Ex** ppReturnedDeviceInterface) override
    {
        LogMsg("CreateDeviceEx called");
        IDirect3DDevice9Ex* realDevice = nullptr;
        HRESULT hr = m_real->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                            pPresentationParameters, pFullscreenDisplayMode, &realDevice);
        if (SUCCEEDED(hr) && realDevice) {
            LogMsg("CreateDeviceEx succeeded, wrapping device (as base Device9)");
            *ppReturnedDeviceInterface = (IDirect3DDevice9Ex*)new WrappedD3D9Device(realDevice);
        } else {
            LogMsg("CreateDeviceEx failed: 0x%08X", hr);
            *ppReturnedDeviceInterface = nullptr;
        }
        return hr;
    }
};

// Load configuration from ini file (optional - defaults are good for discovery)
void LoadConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);

    char* lastSlash = strrchr(path, '\\');
    if (lastSlash) {
        strcpy(lastSlash + 1, "camera_proxy.ini");
    }

    // Check if ini file exists
    DWORD attrib = GetFileAttributesA(path);
    if (attrib == INVALID_FILE_ATTRIBUTES) {
        // No ini file - use defaults (logging enabled)
        return;
    }

    g_config.viewMatrixRegister = GetPrivateProfileIntA("CameraProxy", "ViewMatrixRegister", 4, path);
    g_config.projMatrixRegister = GetPrivateProfileIntA("CameraProxy", "ProjMatrixRegister", -1, path);
    g_config.worldMatrixRegister = GetPrivateProfileIntA("CameraProxy", "WorldMatrixRegister", 0, path);
    g_config.enableLogging = GetPrivateProfileIntA("CameraProxy", "EnableLogging", 1, path) != 0;
    g_config.logAllConstants = GetPrivateProfileIntA("CameraProxy", "LogAllConstants", 1, path) != 0;
    g_config.autoDetectMatrices = GetPrivateProfileIntA("CameraProxy", "AutoDetectMatrices", 1, path) != 0;

    char buf[64];
    GetPrivateProfileStringA("CameraProxy", "MinFOV", "0.1", buf, sizeof(buf), path);
    g_config.minFOV = (float)atof(buf);
    GetPrivateProfileStringA("CameraProxy", "MaxFOV", "2.5", buf, sizeof(buf), path);
    g_config.maxFOV = (float)atof(buf);
}

// DLL entry point
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);

        LoadConfig();

        if (g_config.enableLogging) {
            g_logFile = fopen("camera_proxy.log", "w");
            LogMsg("=== Mirror's Edge Camera Proxy for RTX Remix ===");
            LogMsg("=== CAMERA EXTRACTION MODE ===");
            LogMsg("View matrix: c5-c8 (confirmed from log analysis)");
            LogMsg("Projection: Synthetic 90deg FOV");
        }

        // Load the real Remix d3d9.dll
        char path[MAX_PATH];
        GetModuleFileNameA(hinstDLL, path, MAX_PATH);
        char* lastSlash = strrchr(path, '\\');
        if (lastSlash) {
            strcpy(lastSlash + 1, "d3d9_remix.dll");
        }

        g_hRemixD3D9 = LoadLibraryA(path);
        if (!g_hRemixD3D9) {
            g_hRemixD3D9 = LoadLibraryA("d3d9_remix.dll");
        }

        if (g_hRemixD3D9) {
            g_origDirect3DCreate9 = (Direct3DCreate9_t)GetProcAddress(g_hRemixD3D9, "Direct3DCreate9");
            g_origDirect3DCreate9Ex = (Direct3DCreate9Ex_t)GetProcAddress(g_hRemixD3D9, "Direct3DCreate9Ex");
            g_origD3DPERF_BeginEvent = (D3DPERF_BeginEvent_t)GetProcAddress(g_hRemixD3D9, "D3DPERF_BeginEvent");
            g_origD3DPERF_EndEvent = (D3DPERF_EndEvent_t)GetProcAddress(g_hRemixD3D9, "D3DPERF_EndEvent");
            g_origD3DPERF_GetStatus = (D3DPERF_GetStatus_t)GetProcAddress(g_hRemixD3D9, "D3DPERF_GetStatus");
            g_origD3DPERF_QueryRepeatFrame = (D3DPERF_QueryRepeatFrame_t)GetProcAddress(g_hRemixD3D9, "D3DPERF_QueryRepeatFrame");
            g_origD3DPERF_SetMarker = (D3DPERF_SetMarker_t)GetProcAddress(g_hRemixD3D9, "D3DPERF_SetMarker");
            g_origD3DPERF_SetOptions = (D3DPERF_SetOptions_t)GetProcAddress(g_hRemixD3D9, "D3DPERF_SetOptions");
            g_origD3DPERF_SetRegion = (D3DPERF_SetRegion_t)GetProcAddress(g_hRemixD3D9, "D3DPERF_SetRegion");
            LogMsg("Loaded d3d9_remix.dll successfully");
            LogMsg("  Direct3DCreate9: %p", g_origDirect3DCreate9);
            LogMsg("  Direct3DCreate9Ex: %p", g_origDirect3DCreate9Ex);
        } else {
            LogMsg("ERROR: Failed to load d3d9_remix.dll!");
            MessageBoxA(nullptr, "Failed to load d3d9_remix.dll!\n\nMake sure Remix's d3d9.dll is renamed to d3d9_remix.dll",
                       "Camera Proxy Error", MB_OK | MB_ICONERROR);
        }
    }
    else if (fdwReason == DLL_PROCESS_DETACH) {
        if (g_logFile) {
            LogMsg("=== Camera Proxy unloading ===");
            LogMsg("Total frames: %d", g_frameCount);
            fclose(g_logFile);
        }
        if (g_hRemixD3D9) {
            FreeLibrary(g_hRemixD3D9);
        }
    }
    return TRUE;
}

// Exported functions
extern "C" {
    IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT SDKVersion) {
        LogMsg("Direct3DCreate9 called (SDK version: %d)", SDKVersion);

        if (!g_origDirect3DCreate9) {
            LogMsg("ERROR: g_origDirect3DCreate9 is null!");
            return nullptr;
        }

        IDirect3D9* realD3D9 = g_origDirect3DCreate9(SDKVersion);
        if (!realD3D9) {
            LogMsg("ERROR: Original Direct3DCreate9 returned null!");
            return nullptr;
        }

        LogMsg("Wrapping IDirect3D9");
        return new WrappedD3D9(realD3D9);
    }

    HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D) {
        LogMsg("Direct3DCreate9Ex called (SDK version: %d)", SDKVersion);

        if (!g_origDirect3DCreate9Ex) {
            LogMsg("ERROR: g_origDirect3DCreate9Ex is null!");
            return E_FAIL;
        }

        IDirect3D9Ex* realD3D9Ex = nullptr;
        HRESULT hr = g_origDirect3DCreate9Ex(SDKVersion, &realD3D9Ex);

        if (SUCCEEDED(hr) && realD3D9Ex) {
            LogMsg("Wrapping IDirect3D9Ex");
            *ppD3D = new WrappedD3D9Ex(realD3D9Ex);
        } else {
            LogMsg("ERROR: Original Direct3DCreate9Ex failed: 0x%08X", hr);
            *ppD3D = nullptr;
        }

        return hr;
    }

    // D3DPERF forwarding functions
    int WINAPI Proxy_D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR name) {
        if (g_origD3DPERF_BeginEvent) return g_origD3DPERF_BeginEvent(col, name);
        return 0;
    }

    int WINAPI Proxy_D3DPERF_EndEvent(void) {
        if (g_origD3DPERF_EndEvent) return g_origD3DPERF_EndEvent();
        return 0;
    }

    DWORD WINAPI Proxy_D3DPERF_GetStatus(void) {
        if (g_origD3DPERF_GetStatus) return g_origD3DPERF_GetStatus();
        return 0;
    }

    BOOL WINAPI Proxy_D3DPERF_QueryRepeatFrame(void) {
        if (g_origD3DPERF_QueryRepeatFrame) return g_origD3DPERF_QueryRepeatFrame();
        return FALSE;
    }

    void WINAPI Proxy_D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR name) {
        if (g_origD3DPERF_SetMarker) g_origD3DPERF_SetMarker(col, name);
    }

    void WINAPI Proxy_D3DPERF_SetOptions(DWORD options) {
        if (g_origD3DPERF_SetOptions) g_origD3DPERF_SetOptions(options);
    }

    void WINAPI Proxy_D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR name) {
        if (g_origD3DPERF_SetRegion) g_origD3DPERF_SetRegion(col, name);
    }
}
