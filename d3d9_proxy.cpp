/**
 * Mirror's Edge Camera Proxy for RTX Remix
 *
 * Auto-detects the ViewProjection matrix from vertex shader constants,
 * decomposes it into View + Projection, and feeds both to RTX Remix.
 *
 * Build with Visual Studio Developer Command Prompt (x86):
 *   do_build.bat
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cmath>
#include <cstring>

#pragma comment(lib, "user32.lib")

// Configuration
struct ProxyConfig {
    bool enableLogging = true;
    int diagnosticFrames = 10;   // Log all candidates for N frames after first candidate seen
    float aspect = 16.0f / 9.0f; // Display aspect ratio
    float zNear = 10.0f;
    float zFar = 100000.0f;
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

// ---- Matrix math helpers ----

void MultiplyMatrix4x4(const float* A, const float* B, float* out) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += A[r * 4 + k] * B[k * 4 + c];
            }
            out[r * 4 + c] = sum;
        }
    }
}

void TransposeMatrix4x4(const float* in, float* out) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[c * 4 + r] = in[r * 4 + c];
}

void CreateIdentityMatrix(D3DMATRIX* out) {
    memset(out, 0, sizeof(D3DMATRIX));
    out->_11 = out->_22 = out->_33 = out->_44 = 1.0f;
}

// Multiply two D3DMATRIX (row-vector convention: result = A * B)
void MultiplyD3D(const D3DMATRIX* A, const D3DMATRIX* B, D3DMATRIX* out) {
    MultiplyMatrix4x4((const float*)A, (const float*)B, (float*)out);
}

// Invert a rigid-body View matrix (orthonormal rotation + translation)
// V = | R  0 |    V^-1 = | R^T          0 |
//     | t  1 |            | -t*R^T       1 |
void InvertView(const D3DMATRIX* V, D3DMATRIX* out) {
    memset(out, 0, sizeof(D3DMATRIX));
    // Transpose the 3x3 rotation
    out->_11 = V->_11; out->_12 = V->_21; out->_13 = V->_31;
    out->_21 = V->_12; out->_22 = V->_22; out->_23 = V->_32;
    out->_31 = V->_13; out->_32 = V->_23; out->_33 = V->_33;
    // Translation: -t * R^T
    float tx = V->_41, ty = V->_42, tz = V->_43;
    out->_41 = -(tx*out->_11 + ty*out->_21 + tz*out->_31);
    out->_42 = -(tx*out->_12 + ty*out->_22 + tz*out->_32);
    out->_43 = -(tx*out->_13 + ty*out->_23 + tz*out->_33);
    out->_44 = 1.0f;
}

// Invert D3D LH perspective projection
// P = | xS  0   0   0 |    P^-1 = | 1/xS  0     0     0    |
//     | 0   yS  0   0 |            | 0     1/yS  0     0    |
//     | 0   0   A   1 |            | 0     0     0     1/B  |
//     | 0   0   B   0 |            | 0     0     1    -A/B  |
void InvertProj(const D3DMATRIX* P, D3DMATRIX* out) {
    memset(out, 0, sizeof(D3DMATRIX));
    float xS = P->_11, yS = P->_22, A = P->_33, B = P->_43;
    if (fabsf(xS) < 0.0001f || fabsf(yS) < 0.0001f || fabsf(B) < 0.0001f) {
        CreateIdentityMatrix(out);
        return;
    }
    out->_11 = 1.0f / xS;
    out->_22 = 1.0f / yS;
    out->_34 = 1.0f / B;
    out->_43 = 1.0f;
    out->_44 = -A / B;
}

// ---- Column-major VP detection for UE3 ----
//
// UE3 stores matrices COLUMN-MAJOR in shader constant registers:
//   c0 = f[0..3]   = column 0
//   c1 = f[4..7]   = column 1
//   c2 = f[8..11]  = column 2
//   c3 = f[12..15] = column 3
//
// For VP = Proj * View (column-vector convention), the "perspective row"
// (row 3 in column-major = {f[3], f[7], f[11], f[15]}) contains:
//   {f[3], f[7], f[11]} = camera forward direction (unit vec for identity-World)
//   f[15] = -dot(forward, eye_position) = camera distance
//
// Cross-register rows give projection-scaled view axes:
//   Row 0: {f[0], f[4], f[8], f[12]} = xS * (right, -right·eye)
//   Row 1: {f[1], f[5], f[9], f[13]} = yS * (up, -up·eye)

int ScoreAsVP(const float* f) {
    for (int i = 0; i < 16; i++) {
        if (!isfinite(f[i])) return 0;
    }

    int score = 0;

    // "Perspective row" xyz magnitude: camera forward direction
    // For identity-World VP, this should be ~1.0 (unit forward vector)
    float prMag = sqrtf(f[3]*f[3] + f[7]*f[7] + f[11]*f[11]);
    if (prMag >= 0.8f && prMag <= 1.2f) score += 5;
    else return 0; // Hard requirement: perspective row must be ~unit length

    // Bonus for very close to 1.0 (identity World, most accurate VP)
    if (fabsf(prMag - 1.0f) < 0.05f) score += 3;

    // Projection scales from cross-register row magnitudes
    float xS = sqrtf(f[0]*f[0] + f[4]*f[4] + f[8]*f[8]);
    float yS = sqrtf(f[1]*f[1] + f[5]*f[5] + f[9]*f[9]);

    // Realistic projection: FOV between ~30deg and ~140deg
    if (xS >= 0.3f && xS <= 5.0f) score += 2;
    else return 0;

    if (yS >= 0.3f && yS <= 5.0f) score += 2;
    else return 0;

    // f[15] = -dot(forward, eye) = camera distance, should be substantial
    if (fabsf(f[15]) > 10.0f) score += 2;

    return score;
}

// Decompose column-major UE3 VP into D3D row-vector View + Projection for Remix.
//
// From column-major VP = Proj * View:
//   Row 0: {f[0], f[4], f[8]}  = xS * right_direction
//   Row 1: {f[1], f[5], f[9]}  = yS * up_direction
//   Row 3: {f[3], f[7], f[11]} = forward_direction (perspective row)
//   f[12] = -xS * dot(right, eye)
//   f[13] = -yS * dot(up, eye)
//   f[15] = -dot(forward, eye)
bool DecomposeVP_ColMajor(const float* vp, D3DMATRIX* viewOut, D3DMATRIX* projOut, D3DMATRIX* gameProjOut) {
    // Extract projection scales from cross-register rows
    float xS = sqrtf(vp[0]*vp[0] + vp[4]*vp[4] + vp[8]*vp[8]);
    float yS = sqrtf(vp[1]*vp[1] + vp[5]*vp[5] + vp[9]*vp[9]);

    if (xS < 0.001f || yS < 0.001f) return false;

    // Right direction (normalize row 0 xyz)
    float rx = vp[0] / xS, ry = vp[4] / xS, rz = vp[8] / xS;

    // Up direction (normalize row 1 xyz)
    float ux = vp[1] / yS, uy = vp[5] / yS, uz = vp[9] / yS;

    // Forward direction from perspective row (row 3 xyz)
    float fwdMag = sqrtf(vp[3]*vp[3] + vp[7]*vp[7] + vp[11]*vp[11]);
    if (fwdMag < 0.001f) return false;
    float fx = vp[3] / fwdMag, fy = vp[7] / fwdMag, fz = vp[11] / fwdMag;

    // Camera position from dot products
    float rDotEye = -vp[12] / xS;
    float uDotEye = -vp[13] / yS;
    float fDotEye = -vp[15];

    // Reconstruct eye position: eye = rDotEye*right + uDotEye*up + fDotEye*forward
    float eyeX = rDotEye * rx + uDotEye * ux + fDotEye * fx;
    float eyeY = rDotEye * ry + uDotEye * uy + fDotEye * fy;
    float eyeZ = rDotEye * rz + uDotEye * uz + fDotEye * fz;

    // Build D3D LH row-vector View matrix
    // In D3D row-vector convention: viewPos = worldPos * View
    // View = | rx   ux   fx   0 |
    //        | ry   uy   fy   0 |
    //        | rz   uz   fz   0 |
    //        | tx   ty   tz   1 |
    // where tx = -dot(right, eye), ty = -dot(up, eye), tz = -dot(fwd, eye)
    float tx = -(rx*eyeX + ry*eyeY + rz*eyeZ);
    float ty = -(ux*eyeX + uy*eyeY + uz*eyeZ);
    float tz = -(fx*eyeX + fy*eyeY + fz*eyeZ);

    memset(viewOut, 0, sizeof(D3DMATRIX));
    viewOut->_11 = rx;  viewOut->_12 = ux;  viewOut->_13 = fx;  viewOut->_14 = 0;
    viewOut->_21 = ry;  viewOut->_22 = uy;  viewOut->_23 = fy;  viewOut->_24 = 0;
    viewOut->_31 = rz;  viewOut->_32 = uz;  viewOut->_33 = fz;  viewOut->_34 = 0;
    viewOut->_41 = tx;  viewOut->_42 = ty;  viewOut->_43 = tz;  viewOut->_44 = 1.0f;

    // Log game's actual projection parameters (Row 2 cross-register = A * forward)
    static bool loggedProj = false;
    if (!loggedProj) {
        float r2Mag = sqrtf(vp[2]*vp[2] + vp[6]*vp[6] + vp[10]*vp[10]);
        float A_game = r2Mag;
        float B_game = vp[14] - A_game * vp[15];
        LogMsg("GAME PROJ: A=%.4f B=%.2f xS=%.4f yS=%.4f (zNear_est=%.1f zFar_est=%.1f)",
               A_game, B_game, xS, yS,
               (fabsf(A_game) > 0.001f) ? -B_game/A_game : 0.0f,
               (fabsf(A_game-1.0f) > 0.001f) ? (-B_game/A_game)*A_game/(A_game-1.0f) : 999999.0f);
        loggedProj = true;
    }

    // Use synthetic projection with reasonable depth range for Remix
    // (Game's A=4.34 gives zNear~=camera_distance which clips everything)
    float zN = g_config.zNear;   // default 10.0
    float zF = g_config.zFar;    // default 100000.0
    float A_synth = zF / (zF - zN);
    float B_synth = -zN * zF / (zF - zN);

    memset(projOut, 0, sizeof(D3DMATRIX));
    projOut->_11 = xS;
    projOut->_22 = yS;
    projOut->_33 = A_synth;
    projOut->_34 = 1.0f;
    projOut->_43 = B_synth;

    // Also output game's ACTUAL projection (for accurate VP^-1 computation)
    float r2Mag = sqrtf(vp[2]*vp[2] + vp[6]*vp[6] + vp[10]*vp[10]);
    float A_game = r2Mag;
    float B_game = vp[14] - A_game * vp[15];
    memset(gameProjOut, 0, sizeof(D3DMATRIX));
    gameProjOut->_11 = xS;
    gameProjOut->_22 = yS;
    gameProjOut->_33 = A_game;
    gameProjOut->_34 = 1.0f;
    gameProjOut->_43 = B_game;

    return true;
}

// Forward declarations
class WrappedD3D9Device;
class WrappedD3D9;

// Detection state
enum DetectState { SCANNING, LOCKED };

/**
 * Wrapped IDirect3DDevice9 - intercepts SetVertexShaderConstantF
 */
class WrappedD3D9Device : public IDirect3DDevice9 {
private:
    IDirect3DDevice9* m_real;

    // Auto-detect state
    DetectState m_detectState = SCANNING;
    int m_vpRegister = 0;           // Register where MVP/VP is uploaded (default c0 for UE3)
    int m_consecutiveFrames = 0;    // Frames with consistent VP at this register
    float m_prevVP44 = 0.0f;       // Previous frame's _44 value (tracks camera Z movement)

    // Per-frame best VP candidate from c0
    float m_frameBestVP[16];
    int m_frameBestScore = 0;
    bool m_hasFrameCandidate = false;

    // Diagnostic
    int m_diagStartFrame = -1;
    int m_diagLogsThisFrame = 0;

    // Decomposed matrices
    D3DMATRIX m_lastView;
    D3DMATRIX m_lastProj;       // Synthetic proj for Remix (reasonable zNear/zFar)
    D3DMATRIX m_lastGameProj;   // Game's actual proj (for VP^-1 computation)
    D3DMATRIX m_pendingView;
    D3DMATRIX m_pendingProj;
    D3DMATRIX m_pendingGameProj;
    D3DMATRIX m_vpInverse;      // (GameProj * View)^-1 for World extraction
    bool m_hasCamera = false;
    bool m_pendingUpdate = false;
    bool m_hasVPInverse = false;
    int m_worldLogCount = 0;    // Diagnostic: count World matrix logs

public:
    WrappedD3D9Device(IDirect3DDevice9* real) : m_real(real) {
        memset(&m_lastView, 0, sizeof(D3DMATRIX));
        memset(&m_lastProj, 0, sizeof(D3DMATRIX));
        memset(&m_pendingView, 0, sizeof(D3DMATRIX));
        memset(&m_pendingProj, 0, sizeof(D3DMATRIX));
        memset(&m_lastGameProj, 0, sizeof(D3DMATRIX));
        memset(&m_pendingGameProj, 0, sizeof(D3DMATRIX));
        memset(&m_vpInverse, 0, sizeof(D3DMATRIX));
        memset(m_frameBestVP, 0, sizeof(m_frameBestVP));
        LogMsg("WrappedD3D9Device created, wrapping device at %p", real);
    }

    ~WrappedD3D9Device() {
        LogMsg("WrappedD3D9Device destroyed");
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

    // The key interception point
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(
        UINT StartRegister,
        const float* pConstantData,
        UINT Vector4fCount) override
    {
        // Check if this upload covers the VP register (c0-c3 for UE3)
        if (Vector4fCount >= 4 && pConstantData &&
            StartRegister <= (UINT)m_vpRegister &&
            StartRegister + Vector4fCount >= (UINT)(m_vpRegister + 4))
        {
            int offset = (m_vpRegister - (int)StartRegister) * 4;
            const float* block = pConstantData + offset;

            int score = ScoreAsVP(block);

            // Diagnostic logging
            bool inDiagWindow = (m_diagStartFrame >= 0 &&
                                 g_frameCount < m_diagStartFrame + g_config.diagnosticFrames);
            if (score > 0 && m_diagStartFrame < 0) {
                m_diagStartFrame = g_frameCount;
                inDiagWindow = true;
                LogMsg("=== DIAGNOSTIC START frame %d (column-major VP detect) ===", g_frameCount);
            }
            if (inDiagWindow && m_diagLogsThisFrame < 15) {
                float prMag = sqrtf(block[3]*block[3]+block[7]*block[7]+block[11]*block[11]);
                float xS = sqrtf(block[0]*block[0]+block[4]*block[4]+block[8]*block[8]);
                float yS = sqrtf(block[1]*block[1]+block[5]*block[5]+block[9]*block[9]);
                LogMsg("  [c0] F%d s=%d prMag=%.3f xS=%.3f yS=%.3f f15=%.1f eye=[%.1f,%.1f,%.1f]",
                       g_frameCount, score, prMag, xS, yS, block[15],
                       block[12], block[13], block[14]);
                m_diagLogsThisFrame++;
            }

            // Track best VP candidate this frame (highest score)
            if (score > m_frameBestScore) {
                m_frameBestScore = score;
                memcpy(m_frameBestVP, block, 16 * sizeof(float));
                m_hasFrameCandidate = true;
            }

            // In LOCKED mode: decompose VP, compute per-draw World
            if (m_detectState == LOCKED) {
                if (score >= 6) {
                    D3DMATRIX view, proj, gameProj;
                    if (DecomposeVP_ColMajor(block, &view, &proj, &gameProj)) {
                        memcpy(&m_pendingView, &view, sizeof(D3DMATRIX));
                        memcpy(&m_pendingProj, &proj, sizeof(D3DMATRIX));
                        memcpy(&m_pendingGameProj, &gameProj, sizeof(D3DMATRIX));
                        m_pendingUpdate = true;

                        if (!m_hasCamera) {
                            LogMsg("FIRST CAMERA: view=[%.1f,%.1f,%.1f] proj=[%.3f,%.3f] gameA=%.4f",
                                   view._41, view._42, view._43, proj._11, proj._22, gameProj._33);
                            memcpy(&m_lastView, &view, sizeof(D3DMATRIX));
                            memcpy(&m_lastProj, &proj, sizeof(D3DMATRIX));
                            memcpy(&m_lastGameProj, &gameProj, sizeof(D3DMATRIX));
                            m_hasCamera = true;
                            m_real->SetTransform(D3DTS_VIEW, &m_lastView);
                            m_real->SetTransform(D3DTS_PROJECTION, &m_lastProj);
                            // Compute VP^-1 using game's REAL projection
                            D3DMATRIX viewInv, gameProjInv;
                            InvertView(&m_lastView, &viewInv);
                            InvertProj(&m_lastGameProj, &gameProjInv);
                            MultiplyD3D(&gameProjInv, &viewInv, &m_vpInverse);
                            m_hasVPInverse = true;
                            LogMsg("VP^-1 computed (gameA=%.4f gameB=%.2f)", m_lastGameProj._33, m_lastGameProj._43);
                        }
                    }
                }

                // Per-draw World: only for full-res c0 uploads (f[14] non-zero)
                // f[14] = A*tz+B for full-res pass, ~0 for half-res (eye.z stripped)
                if (m_hasVPInverse && fabsf(block[14]) > 1.0f) {
                    float mvpD3D[16];
                    TransposeMatrix4x4(block, mvpD3D);
                    D3DMATRIX world;
                    MultiplyMatrix4x4(mvpD3D, (const float*)&m_vpInverse, (float*)&world);
                    m_real->SetTransform(D3DTS_WORLD, &world);
                    // Log first few World matrices for diagnostic
                    if (m_worldLogCount < 5) {
                        LogMsg("WORLD[%d]: diag=[%.3f,%.3f,%.3f,%.3f] trans=[%.1f,%.1f,%.1f]",
                               m_worldLogCount, world._11, world._22, world._33, world._44,
                               world._41, world._42, world._43);
                        m_worldLogCount++;
                    }
                } else if (m_hasVPInverse) {
                    // Half-res pass or non-VP: set identity World
                    D3DMATRIX identity;
                    CreateIdentityMatrix(&identity);
                    m_real->SetTransform(D3DTS_WORLD, &identity);
                }
            }
        }

        return m_real->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
    }

    // Present - per-frame operations
    HRESULT STDMETHODCALLTYPE Present(const RECT* pSourceRect, const RECT* pDestRect,
                                       HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) override {
        // SCANNING -> LOCKED transition
        if (m_detectState == SCANNING && m_hasFrameCandidate) {
            // Check if _44 changed from previous frame (camera Z is moving)
            float d44 = fabsf(m_frameBestVP[15] - m_prevVP44);
            if (d44 > 0.01f) {
                m_consecutiveFrames++;
            }

            bool inDiagWindow = (m_diagStartFrame >= 0 && g_frameCount < m_diagStartFrame + g_config.diagnosticFrames);
            if (inDiagWindow) {
                LogMsg("  Frame %d: bestScore=%d _44=%.1f d44=%.3f consec=%d",
                       g_frameCount, m_frameBestScore, m_frameBestVP[15], d44, m_consecutiveFrames);
            }

            m_prevVP44 = m_frameBestVP[15];

            // Lock after 3 frames of changing _44 (camera Z moving = real 3D camera)
            if (m_consecutiveFrames >= 3) {
                m_detectState = LOCKED;
                float lockPrMag = sqrtf(m_frameBestVP[3]*m_frameBestVP[3]+m_frameBestVP[7]*m_frameBestVP[7]+m_frameBestVP[11]*m_frameBestVP[11]);
                LogMsg("*** LOCKED on c%d-c%d as VP (col-major, prMag=%.3f, f15=%.1f) ***",
                       m_vpRegister, m_vpRegister + 3, lockPrMag, m_frameBestVP[15]);

                // Decompose immediately
                D3DMATRIX view, proj, gameProj;
                if (DecomposeVP_ColMajor(m_frameBestVP, &view, &proj, &gameProj)) {
                    memcpy(&m_lastView, &view, sizeof(D3DMATRIX));
                    memcpy(&m_lastProj, &proj, sizeof(D3DMATRIX));
                    memcpy(&m_lastGameProj, &gameProj, sizeof(D3DMATRIX));
                    m_hasCamera = true;
                    m_real->SetTransform(D3DTS_VIEW, &m_lastView);
                    m_real->SetTransform(D3DTS_PROJECTION, &m_lastProj);
                    // Compute VP^-1 using game's real projection
                    D3DMATRIX viewInv, gameProjInv;
                    InvertView(&m_lastView, &viewInv);
                    InvertProj(&m_lastGameProj, &gameProjInv);
                    MultiplyD3D(&gameProjInv, &viewInv, &m_vpInverse);
                    m_hasVPInverse = true;
                    LogMsg("  View trans=[%.1f, %.1f, %.1f] Proj xS=%.3f yS=%.3f gameA=%.4f",
                           view._41, view._42, view._43, proj._11, proj._22, gameProj._33);
                }
            }
        }

        // Apply pending camera update once per frame + recompute VP^-1
        if (m_pendingUpdate && m_hasCamera) {
            memcpy(&m_lastView, &m_pendingView, sizeof(D3DMATRIX));
            memcpy(&m_lastProj, &m_pendingProj, sizeof(D3DMATRIX));
            memcpy(&m_lastGameProj, &m_pendingGameProj, sizeof(D3DMATRIX));
            m_real->SetTransform(D3DTS_VIEW, &m_lastView);
            m_real->SetTransform(D3DTS_PROJECTION, &m_lastProj);
            // Recompute VP^-1 with game's real projection for next frame
            D3DMATRIX viewInv, gameProjInv;
            InvertView(&m_lastView, &viewInv);
            InvertProj(&m_lastGameProj, &gameProjInv);
            MultiplyD3D(&gameProjInv, &viewInv, &m_vpInverse);
            m_hasVPInverse = true;
            m_pendingUpdate = false;
        }

        // Reset per-frame state
        m_hasFrameCandidate = false;
        m_frameBestScore = 0;
        m_diagLogsThisFrame = 0;

        g_frameCount++;

        // Periodic status logging
        if (g_frameCount % 300 == 0) {
            LogMsg("=== Frame %d Status: state=%s hasCamera=%d vpReg=c%d ===",
                   g_frameCount,
                   m_detectState == LOCKED ? "LOCKED" : "SCANNING",
                   m_hasCamera, m_vpRegister);
            if (m_hasCamera) {
                LogMsg("  View trans: [%.1f, %.1f, %.1f]",
                       m_lastView._41, m_lastView._42, m_lastView._43);
                LogMsg("  Proj: xS=%.3f yS=%.3f", m_lastProj._11, m_lastProj._22);
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
        if (m_hasCamera) {
            D3DMATRIX identity;
            CreateIdentityMatrix(&identity);
            m_real->SetTransform(D3DTS_WORLD, &identity);
            m_real->SetTransform(D3DTS_VIEW, &m_lastView);
            m_real->SetTransform(D3DTS_PROJECTION, &m_lastProj);
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
    ~WrappedD3D9() { LogMsg("WrappedD3D9 destroyed"); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override { return m_real->QueryInterface(riid, ppvObj); }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_real->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = m_real->Release();
        if (count == 0) delete this;
        return count;
    }

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

    HRESULT STDMETHODCALLTYPE CreateDevice(
        UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
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
 * Wrapped IDirect3D9Ex
 */
class WrappedD3D9Ex : public IDirect3D9Ex {
private:
    IDirect3D9Ex* m_real;

public:
    WrappedD3D9Ex(IDirect3D9Ex* real) : m_real(real) {
        LogMsg("WrappedD3D9Ex created, wrapping IDirect3D9Ex at %p", real);
    }
    ~WrappedD3D9Ex() { LogMsg("WrappedD3D9Ex destroyed"); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override { return m_real->QueryInterface(riid, ppvObj); }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_real->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = m_real->Release();
        if (count == 0) delete this;
        return count;
    }

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

// Load configuration from ini file
void LoadConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* lastSlash = strrchr(path, '\\');
    if (lastSlash) {
        strcpy(lastSlash + 1, "camera_proxy.ini");
    }

    DWORD attrib = GetFileAttributesA(path);
    if (attrib == INVALID_FILE_ATTRIBUTES) return;

    g_config.enableLogging = GetPrivateProfileIntA("CameraProxy", "EnableLogging", 1, path) != 0;
    g_config.diagnosticFrames = GetPrivateProfileIntA("CameraProxy", "DiagnosticFrames", 5, path);

    char buf[64];
    GetPrivateProfileStringA("CameraProxy", "Aspect", "1.7778", buf, sizeof(buf), path);
    g_config.aspect = (float)atof(buf);
    GetPrivateProfileStringA("CameraProxy", "ZNear", "10.0", buf, sizeof(buf), path);
    g_config.zNear = (float)atof(buf);
    GetPrivateProfileStringA("CameraProxy", "ZFar", "100000.0", buf, sizeof(buf), path);
    g_config.zFar = (float)atof(buf);
}

// DLL entry point
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        LoadConfig();

        if (g_config.enableLogging) {
            g_logFile = fopen("camera_proxy.log", "w");
            LogMsg("=== Mirror's Edge Camera Proxy for RTX Remix ===");
            LogMsg("=== VP AUTO-DETECT MODE ===");
            LogMsg("Scanning all SetVertexShaderConstantF for ViewProjection signatures");
            LogMsg("Diagnostic frames: %d", g_config.diagnosticFrames);
        }

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
