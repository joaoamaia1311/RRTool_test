/* gpu.cpp - GPU drawing backend for the ROB RUN overlay.
 *
 * D3D11 owns the swap chain, DirectComposition presents its transparent surface,
 * and Direct2D/DirectWrite draw shapes and text. The CPU-rasterized character
 * mesh is uploaded as a texture below the 2D overlay; a shader expands its
 * silhouette to draw an outline.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "gpu.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <vector>

#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite_3.h>
#include <dcomp.h>
#include <d3dcompiler.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3dcompiler.lib")

/* State. */

static HWND                     g_hwnd;
static int                      g_w, g_h, g_ready;

static ID3D11Device*            g_dev;
static ID3D11DeviceContext*     g_ctx;
static IDXGISwapChain1*         g_swap;
static HANDLE                  g_frame_event;
static UINT                    g_swap_flags;
static volatile LONG           g_frame_granted;
static IDCompositionDevice*     g_dcomp;
static IDCompositionTarget*     g_dtarget;
static IDCompositionVisual*     g_dvisual;

static ID2D1Factory1*           g_d2df;
static ID2D1Device*             g_d2ddev;
static ID2D1DeviceContext*      g_d2dc;
static ID2D1Bitmap1*            g_target;

static IDWriteFactory5*         g_dwf;
static IDWriteFontCollection1*  g_fontcoll;
static IDWriteTextFormat*       g_fmt;          /* rebuilt when the size changes */
static float                    g_fmt_px = 13.0f;

static ID2D1SolidColorBrush*    g_brush;
static RGBA                     g_brush_col;
static int                      g_brush_valid;

static RGBA                     g_pen_col = 0xFFFFFFFFu;
static float                    g_pen_w   = 1.0f;
static int                      g_batch;        /* inside BeginDraw */

/* chams layer */
static ID3D11Texture2D*         g_chtex;
static ID3D11ShaderResourceView* g_chsrv;
static ID3D11SamplerState*      g_chsmp;
static ID3D11VertexShader*      g_vs;
static ID3D11PixelShader*       g_ps;
static ID3D11Buffer*            g_cb;
static ID3D11BlendState*        g_blend;
static ID3D11RasterizerState*   g_rast;
static ID3D11DepthStencilState* g_nodepth;
static int                      g_chw, g_chh;
static int                      g_ch_ok;
struct CHAM_VERTEX { float x,y,r,g,b,a; };
static std::vector<CHAM_VERTEX> g_mesh_vertices;
static ID3D11VertexShader* g_mesh_vs;
static ID3D11PixelShader* g_mesh_ps;
static ID3D11InputLayout* g_mesh_layout;
static ID3D11Buffer* g_mesh_buffer;
static unsigned g_mesh_buffer_bytes;

typedef struct { float texel[2]; float radius; float opacity; float col[4]; } CH_PARAMS;

/* Shaders. */

static const char* VS_SRC =
"struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
"VSOut main(uint id : SV_VertexID) {\n"
"    VSOut o;\n"
"    float2 p = float2((id == 2) ? 3.0 : -1.0, (id == 1) ? 3.0 : -1.0);\n"
"    o.pos = float4(p, 0.0, 1.0);\n"
"    o.uv  = float2((p.x + 1.0) * 0.5, 1.0 - (p.y + 1.0) * 0.5);\n"
"    return o;\n"
"}\n";

/* Draw the fill over an outline expanded by radius pixels. Twelve sample
 * directions approximate the border for small outline widths.
 */
static const char* PS_SRC =
"Texture2D tex : register(t0);\n"
"SamplerState smp : register(s0);\n"
"cbuffer P : register(b0) { float2 texel; float radius; float opacity; float4 col; };\n"
"float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {\n"
"    float4 s = tex.Sample(smp, uv);\n"
"    if (s.a > 0.004) return float4(s.rgb, s.a * opacity);\n"
"    if (radius <= 0.0) return float4(0,0,0,0);\n"
"    float m = 0.0;\n"
"    float2 d = float2(texel.x * radius, texel.y * radius);\n"
"    [unroll] for (int i = 0; i < 12; i++) {\n"
"        float a = (float)i * 0.5235988;\n"
"        float2 o = float2(cos(a), sin(a)) * d;\n"
"        m = max(m, tex.Sample(smp, uv + o).a);\n"
"        m = max(m, tex.Sample(smp, uv + o * 0.5).a);\n"
"    }\n"
"    if (m > 0.004) return float4(col.rgb * col.a, col.a);\n"
"    return float4(0,0,0,0);\n"
"}\n";

/* Helpers. */

static void release_target(void) {
    if (g_target) { g_target->Release(); g_target = NULL; }
}

static void ensure_batch(void) {
    if (g_batch || !g_d2dc) return;
    g_d2dc->BeginDraw();
    g_d2dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g_d2dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_d2dc->SetTransform(D2D1::Matrix3x2F::Identity());
    g_batch = 1;
}

static ID2D1SolidColorBrush* brush_for(RGBA c) {
    if (g_brush_valid && g_brush_col == c && g_brush) return g_brush;
    if (g_brush) { g_brush->Release(); g_brush = NULL; }
    float r = ((c >> 16) & 0xFF) / 255.0f;
    float g = ((c >> 8) & 0xFF) / 255.0f;
    float b = (c & 0xFF) / 255.0f;
    float a = ((c >> 24) & 0xFF) / 255.0f;
    if (FAILED(g_d2dc->CreateSolidColorBrush(D2D1::ColorF(r, g, b, a), &g_brush))) {
        g_brush = NULL; g_brush_valid = 0; return NULL;
    }
    g_brush_col = c; g_brush_valid = 1;
    return g_brush;
}

/* Text. */

static void font_from_file(void) {
    char exe[MAX_PATH] = {0}, path[MAX_PATH * 2];
    GetModuleFileNameA(NULL, exe, sizeof(exe) - 1);
    char* slash = strrchr(exe, '\\');
    if (slash) *(slash + 1) = 0;
    snprintf(path, sizeof(path), "%sTTNormsTrlCnd-Medium.otf", exe);

    wchar_t wpath[MAX_PATH * 2];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH * 2);
    if (GetFileAttributesW(wpath) == INVALID_FILE_ATTRIBUTES) return;

    IDWriteFontFile* file = NULL;
    if (FAILED(g_dwf->CreateFontFileReference(wpath, NULL, &file))) return;

    IDWriteFontSetBuilder1* b = NULL;
    IDWriteFontSet* set = NULL;
    if (SUCCEEDED(g_dwf->CreateFontSetBuilder(&b)) && b) {
        if (SUCCEEDED(b->AddFontFile(file)) && SUCCEEDED(b->CreateFontSet(&set)) && set)
            g_dwf->CreateFontCollectionFromFontSet(set, &g_fontcoll);
    }
    if (set) set->Release();
    if (b) b->Release();
    file->Release();
}

static int make_format(float px) {
    if (g_fmt) { g_fmt->Release(); g_fmt = NULL; }
    const wchar_t* fam = L"TT Norms Std Trl Cnd";
    HRESULT hr = E_FAIL;
    if (g_fontcoll) {
        hr = g_dwf->CreateTextFormat(fam, g_fontcoll, DWRITE_FONT_WEIGHT_MEDIUM,
                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                     px, L"en-us", &g_fmt);
    }
    if (FAILED(hr) || !g_fmt) {
        hr = g_dwf->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                     px, L"en-us", &g_fmt);
    }
    if (FAILED(hr) || !g_fmt) return 0;
    g_fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g_fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    g_fmt_px = px;
    return 1;
}

void gpu_font(float px) {
    if (px < 6.0f) px = 6.0f;
    if (px > 96.0f) px = 96.0f;
    if (g_fmt && fabsf(px - g_fmt_px) < 0.5f) return;
    make_format(px);
}

static void text_run(float x, float y, const wchar_t* s, int n, RGBA c, int flags) {
    if (!g_d2dc || !g_fmt || n <= 0) return;
    ensure_batch();

    float w = 0.0f;
    IDWriteTextLayout* lay = NULL;
    if (SUCCEEDED(g_dwf->CreateTextLayout(s, (UINT32)n, g_fmt, 4096.0f, 4096.0f, &lay)) && lay) {
        DWRITE_TEXT_METRICS m;
        if (SUCCEEDED(lay->GetMetrics(&m))) w = m.width;
    }
    float dx = 0.0f;
    if (flags & RT_CENTER) dx = -w * 0.5f;
    else if (flags & RT_RIGHT) dx = -w;

    D2D1_RECT_F rc = D2D1::RectF(x + dx, y, x + dx + 4096.0f, y + g_fmt_px * 3.0f);
    if (flags & RT_OUTLINE) {
        RGBA halo = 0xC0000000u;
        ID2D1SolidColorBrush* hb = brush_for(halo);
        if (hb) {
            static const float ox[4] = { -1.0f, 1.0f, 0.0f, 0.0f };
            static const float oy[4] = { 0.0f, 0.0f, -1.0f, 1.0f };
            for (int i = 0; i < 4; i++) {
                D2D1_RECT_F r2 = D2D1::RectF(rc.left + ox[i], rc.top + oy[i],
                                             rc.right + ox[i], rc.bottom + oy[i]);
                g_d2dc->DrawTextLayout(D2D1::Point2F(r2.left, r2.top), lay, hb,
                                       D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
            g_brush_valid = 0;   /* brush cache: the halo brush may have replaced it */
        }
    }
    ID2D1SolidColorBrush* fb = brush_for(c);
    if (fb) g_d2dc->DrawTextLayout(D2D1_POINT_2F{rc.left, rc.top}, lay, fb,
                                   D2D1_DRAW_TEXT_OPTIONS_NONE);
    if (lay) lay->Release();
}

void r_text_w(float x, float y, const wchar_t* s, int len, RGBA c, int flags) {
    text_run(x, y, s, len, c, flags);
}

void r_text(float x, float y, const char* utf8, RGBA c, int flags) {
    if (!utf8 || !*utf8) return;
    wchar_t buf[512];
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buf, 512);
    if (n <= 0) return;
    text_run(x, y, buf, n - 1, c, flags);
}

float r_text_height(void) { return g_fmt_px * 1.30f; }

float r_text_width(const char* utf8) {
    if (!utf8 || !*utf8 || !g_dwf || !g_fmt) return 0.0f;
    wchar_t buf[512];
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buf, 512);
    if (n <= 0) return 0.0f;
    IDWriteTextLayout* lay = NULL;
    float w = 0.0f;
    if (SUCCEEDED(g_dwf->CreateTextLayout(buf, (UINT32)(n - 1), g_fmt, 4096.0f, 4096.0f, &lay)) && lay) {
        DWRITE_TEXT_METRICS m;
        if (SUCCEEDED(lay->GetMetrics(&m))) w = m.width;
        lay->Release();
    }
    return w;
}

/* Primitives. */

void gpu_pen(RGBA c, float width) { g_pen_col = c; g_pen_w = width < 1.0f ? 1.0f : width; }

void r_line(float x1, float y1, float x2, float y2) {
    if (!g_d2dc) return;
    ensure_batch();
    ID2D1SolidColorBrush* b = brush_for(g_pen_col);
    if (b) g_d2dc->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), b, g_pen_w);
}

void r_line_c(float x1, float y1, float x2, float y2, RGBA c, float w) {
    if (!g_d2dc) return;
    ensure_batch();
    ID2D1SolidColorBrush* b = brush_for(c);
    if (b) g_d2dc->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), b, w < 1.0f ? 1.0f : w);
}

void r_rect(float x, float y, float w, float h, RGBA c, float stroke) {
    if (!g_d2dc || w <= 0.0f || h <= 0.0f) return;
    ensure_batch();
    ID2D1SolidColorBrush* b = brush_for(c);
    if (b) g_d2dc->DrawRectangle(D2D1::RectF(x, y, x + w, y + h), b,
                                 stroke < 1.0f ? 1.0f : stroke);
}

void r_fill(float x, float y, float w, float h, RGBA c) {
    if (!g_d2dc || w <= 0.0f || h <= 0.0f) return;
    ensure_batch();
    ID2D1SolidColorBrush* b = brush_for(c);
    if (b) g_d2dc->FillRectangle(D2D1::RectF(x, y, x + w, y + h), b);
}

void r_ellipse(float cx, float cy, float rx, float ry, RGBA c, float stroke) {
    if (!g_d2dc || rx <= 0.0f) return;
    ensure_batch();
    ID2D1SolidColorBrush* b = brush_for(c);
    if (b) g_d2dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), b,
                               stroke < 1.0f ? 1.0f : stroke);
}

void r_fill_ellipse(float cx, float cy, float rx, float ry, RGBA c) {
    if (!g_d2dc || rx <= 0.0f) return;
    ensure_batch();
    ID2D1SolidColorBrush* b = brush_for(c);
    if (b) g_d2dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), b);
}

static int mesh_resources(void) {
    if (g_mesh_vs && g_mesh_ps && g_mesh_layout) return 1;
    if (!g_dev) return 0;
    static const char* vs="struct I{float2 p:POSITION;float4 c:COLOR;};struct O{float4 p:SV_POSITION;float4 c:COLOR;};O main(I i){O o;o.p=float4(i.p,0,1);o.c=i.c;return o;}";
    static const char* ps="float4 main(float4 p:SV_POSITION,float4 c:COLOR):SV_TARGET{return c;}";
    ID3DBlob *vb=NULL,*pb=NULL,*error=NULL;
    if (FAILED(D3DCompile(vs,strlen(vs),NULL,NULL,NULL,"main","vs_5_0",0,0,&vb,&error))) { if(error)error->Release(); return 0; }
    if (FAILED(D3DCompile(ps,strlen(ps),NULL,NULL,NULL,"main","ps_5_0",0,0,&pb,&error))) { if(error)error->Release(); vb->Release(); return 0; }
    D3D11_INPUT_ELEMENT_DESC layout[2]={{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
                                        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0}};
    HRESULT a=g_dev->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),NULL,&g_mesh_vs);
    HRESULT b=g_dev->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),NULL,&g_mesh_ps);
    HRESULT c=g_dev->CreateInputLayout(layout,2,vb->GetBufferPointer(),vb->GetBufferSize(),&g_mesh_layout);
    vb->Release(); pb->Release();
    if (SUCCEEDED(a)&&SUCCEEDED(b)&&SUCCEEDED(c)) return 1;
    if(g_mesh_vs){g_mesh_vs->Release();g_mesh_vs=NULL;}
    if(g_mesh_ps){g_mesh_ps->Release();g_mesh_ps=NULL;}
    if(g_mesh_layout){g_mesh_layout->Release();g_mesh_layout=NULL;}
    return 0;
}

int r_fill_triangles(const float* xy, int vertex_count, const unsigned short* indices, int triangle_count, RGBA color) {
    if (!xy || !indices || vertex_count<=0 || triangle_count<=0 || g_w<=0 || g_h<=0 || !mesh_resources()) return 0;
    float alpha=((color>>24)&255)/255.0f;
    float r=((color>>16)&255)/255.0f*alpha, g=((color>>8)&255)/255.0f*alpha, b=(color&255)/255.0f*alpha;
    int drawn=0;
    for (int i=0;i<triangle_count;++i) {
        int ia=indices[i*3],ib=indices[i*3+1],ic=indices[i*3+2];
        if (ia>=vertex_count || ib>=vertex_count || ic>=vertex_count) continue;
        float ax=xy[ia*2],ay=xy[ia*2+1], bx=xy[ib*2],by=xy[ib*2+1], cx=xy[ic*2],cy=xy[ic*2+1];
        if (!isfinite(ax)||!isfinite(ay)||!isfinite(bx)||!isfinite(by)||!isfinite(cx)||!isfinite(cy)) continue;
        if (fabsf(ax)>g_w*4.0f || fabsf(bx)>g_w*4.0f || fabsf(cx)>g_w*4.0f ||
            fabsf(ay)>g_h*4.0f || fabsf(by)>g_h*4.0f || fabsf(cy)>g_h*4.0f) continue;
        if (g_mesh_vertices.size()+3>400000) break;
        g_mesh_vertices.push_back({ax*2/g_w-1,1-ay*2/g_h,r,g,b,alpha});
        g_mesh_vertices.push_back({bx*2/g_w-1,1-by*2/g_h,r,g,b,alpha});
        g_mesh_vertices.push_back({cx*2/g_w-1,1-cy*2/g_h,r,g,b,alpha}); ++drawn;
    }
    return drawn;
}

void gpu_mesh_flush(void) {
    if (g_mesh_vertices.empty() || !g_ctx || !g_dev || g_batch || !g_rast || !g_nodepth || !g_blend) return;
    unsigned bytes=(unsigned)(g_mesh_vertices.size()*sizeof(CHAM_VERTEX));
    if (!g_mesh_buffer || bytes>g_mesh_buffer_bytes) {
        if (g_mesh_buffer) { g_mesh_buffer->Release(); g_mesh_buffer=NULL; }
        g_mesh_buffer_bytes=(bytes+65535u)&~65535u;
        D3D11_BUFFER_DESC desc={}; desc.ByteWidth=g_mesh_buffer_bytes; desc.Usage=D3D11_USAGE_DYNAMIC;
        desc.BindFlags=D3D11_BIND_VERTEX_BUFFER; desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_dev->CreateBuffer(&desc,NULL,&g_mesh_buffer))) { g_mesh_buffer_bytes=0; return; }
    }
    D3D11_MAPPED_SUBRESOURCE mapped={};
    if (FAILED(g_ctx->Map(g_mesh_buffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped))) return;
    memcpy(mapped.pData,g_mesh_vertices.data(),bytes); g_ctx->Unmap(g_mesh_buffer,0);
    UINT stride=sizeof(CHAM_VERTEX),offset=0; float factor[4]={0,0,0,0};
    g_ctx->IASetInputLayout(g_mesh_layout); g_ctx->IASetVertexBuffers(0,1,&g_mesh_buffer,&stride,&offset);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_mesh_vs,NULL,0); g_ctx->PSSetShader(g_mesh_ps,NULL,0);
    /* Write premultiplied pixels once: overlapping front/back triangles must
       not repeatedly increase opacity on a translucent silhouette. */
    g_ctx->RSSetState(g_rast); g_ctx->OMSetBlendState(NULL,factor,0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(g_nodepth,0); g_ctx->Draw((UINT)g_mesh_vertices.size(),0);
    g_mesh_vertices.clear();
}

void r_polyline(const float* xy, int n, RGBA c, float w) {
    if (!g_d2dc || !xy || n < 2) return;
    ensure_batch();
    ID2D1SolidColorBrush* b = brush_for(c);
    if (!b) return;
    /* Direct2D draws the polyline as consecutive line segments with round caps. */
    for (int i = 0; i + 1 < n; i++)
        g_d2dc->DrawLine(D2D1::Point2F(xy[i * 2], xy[i * 2 + 1]),
                         D2D1::Point2F(xy[i * 2 + 2], xy[i * 2 + 3]), b, w < 1.0f ? 1.0f : w);
}

/* Chams. */

static int chams_resources(void) {
    ID3DBlob* vsb = NULL, *psb = NULL, *err = NULL;
    if (FAILED(D3DCompile(VS_SRC, strlen(VS_SRC), NULL, NULL, NULL, "main", "vs_5_0", 0, 0, &vsb, &err))) {
        if (err) err->Release();
        return 0;
    }
    if (FAILED(D3DCompile(PS_SRC, strlen(PS_SRC), NULL, NULL, NULL, "main", "ps_5_0", 0, 0, &psb, &err))) {
        if (err) err->Release();
        vsb->Release();
        return 0;
    }
    g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &g_vs);
    g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), NULL, &g_ps);
    vsb->Release(); psb->Release();

    D3D11_BUFFER_DESC cbd = {0};
    cbd.ByteWidth = sizeof(CH_PARAMS);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&cbd, NULL, &g_cb);

    D3D11_SAMPLER_DESC sd = {0};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&sd, &g_chsmp);

    D3D11_BLEND_DESC bd = {0};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;             /* premultiplied */
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dev->CreateBlendState(&bd, &g_blend);

    D3D11_RASTERIZER_DESC rd = {0};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = FALSE;
    rd.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&rd, &g_rast);

    D3D11_DEPTH_STENCIL_DESC dd = {0};
    dd.DepthEnable = FALSE;
    g_dev->CreateDepthStencilState(&dd, &g_nodepth);

    return g_vs && g_ps && g_cb && g_chsmp && g_blend && g_rast && g_nodepth;
}

void gpu_chams_alloc(int w, int h) {
    if (!g_dev) return;
    if (g_chtex && g_chw == w && g_chh == h) return;
    if (g_chsrv) { g_chsrv->Release(); g_chsrv = NULL; }
    if (g_chtex) { g_chtex->Release(); g_chtex = NULL; }
    if (w <= 0 || h <= 0) return;

    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&td, NULL, &g_chtex))) { g_chtex = NULL; return; }
    g_dev->CreateShaderResourceView(g_chtex, NULL, &g_chsrv);
    g_chw = w; g_chh = h;

    if (!g_ch_ok) g_ch_ok = chams_resources();
}

void gpu_chams_upload(const uint32_t* px, int w, int h, int stride,
                      int x0, int y0, int x1, int y1) {
    if (!g_chtex || !px || !g_ctx) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 <= x0 || y1 <= y0) return;
    if (x1 > g_chw) x1 = g_chw;
    if (y1 > g_chh) y1 = g_chh;

    D3D11_BOX box = { (UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1 };
    const uint8_t* src = (const uint8_t*)px + (size_t)y0 * stride + (size_t)x0 * 4;
    g_ctx->UpdateSubresource(g_chtex, 0, &box, src, (UINT)stride, 0);
}

void gpu_chams_draw(int outline_px, RGBA outline_col, float opacity) {
    if (!g_chtex || !g_chsrv || !g_ch_ok || g_batch) return;
    if (opacity <= 0.0f) return;

    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        CH_PARAMS* p = (CH_PARAMS*)m.pData;
        p->texel[0] = 1.0f / (float)g_chw;
        p->texel[1] = 1.0f / (float)g_chh;
        p->radius = (float)(outline_px < 0 ? 0 : outline_px);
        p->opacity = opacity;
        p->col[0] = ((outline_col >> 16) & 0xFF) / 255.0f;
        p->col[1] = ((outline_col >> 8) & 0xFF) / 255.0f;
        p->col[2] = (outline_col & 0xFF) / 255.0f;
        p->col[3] = ((outline_col >> 24) & 0xFF) / 255.0f;
        g_ctx->Unmap(g_cb, 0);
    }

    ID3D11ShaderResourceView* srv = g_chsrv;
    ID3D11Buffer* cb = g_cb;
    ID3D11SamplerState* smp = g_chsmp;
    ID3D11BlendState* bl = g_blend;
    ID3D11RasterizerState* rs = g_rast;
    ID3D11DepthStencilState* ds = g_nodepth;

    g_ctx->IASetInputLayout(NULL);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs, NULL, 0);
    g_ctx->PSSetShader(g_ps, NULL, 0);
    g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->PSSetSamplers(0, 1, &smp);
    g_ctx->PSSetConstantBuffers(0, 1, &cb);
    g_ctx->OMSetBlendState(bl, NULL, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(ds, 0);
    g_ctx->RSSetState(rs);
    g_ctx->Draw(3, 0);

    g_ctx->OMSetBlendState(NULL, NULL, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(NULL, 0);
    g_ctx->RSSetState(NULL);
}

/* Init. */

static int create_swap(void) {
    IDXGIDevice* dxdev = NULL;
    if (FAILED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev))) return 0;
    IDXGIAdapter* adapter = NULL;
    dxdev->GetAdapter(&adapter);
    IDXGIFactory2* fac = NULL;
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&fac);

    DXGI_SWAP_CHAIN_DESC1 sd = {0};
    sd.Width = g_w;
    sd.Height = g_h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    HRESULT hr = fac->CreateSwapChainForComposition(g_dev, &sd, NULL, &g_swap);
    if (FAILED(hr)) {
        sd.Flags = 0;
        hr = fac->CreateSwapChainForComposition(g_dev, &sd, NULL, &g_swap);
    }
    g_swap_flags = sd.Flags;
    if (SUCCEEDED(hr) && g_swap && sd.Flags) {
        IDXGISwapChain2* paced = NULL;
        if (SUCCEEDED(g_swap->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&paced))) {
            if (SUCCEEDED(paced->SetMaximumFrameLatency(1)))
                g_frame_event = paced->GetFrameLatencyWaitableObject();
            paced->Release();
        }
    }
    fac->Release();
    adapter->Release();
    dxdev->Release();
    return SUCCEEDED(hr) && g_swap;
}

int gpu_init(HWND hwnd, int w, int h) {
    g_hwnd = hwnd; g_w = w; g_h = h;

    D3D_FEATURE_LEVEL lv[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                               D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
                                   lv, ARRAYSIZE(lv), D3D11_SDK_VERSION,
                                   &g_dev, NULL, &g_ctx);
    if (FAILED(hr)) return 0;

    if (!create_swap()) return 0;

    IDXGIDevice* dxdev = NULL;
    g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev);

    D2D1_FACTORY_OPTIONS fo = { D2D1_DEBUG_LEVEL_NONE };
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 __uuidof(ID2D1Factory1), &fo, (void**)&g_d2df))) return 0;
    if (FAILED(g_d2df->CreateDevice(dxdev, &g_d2ddev))) return 0;
    if (FAILED(g_d2ddev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_d2dc))) return 0;
    g_d2dc->SetDpi(96.0f, 96.0f);

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                                   (IUnknown**)&g_dwf))) return 0;
    font_from_file();
    make_format(13.0f);

    if (FAILED(DCompositionCreateDevice(dxdev, __uuidof(IDCompositionDevice), (void**)&g_dcomp)))
        return 0;
    if (FAILED(g_dcomp->CreateTargetForHwnd(hwnd, TRUE, &g_dtarget))) return 0;
    if (FAILED(g_dcomp->CreateVisual(&g_dvisual))) return 0;
    g_dvisual->SetContent(g_swap);
    g_dtarget->SetRoot(g_dvisual);
    g_dcomp->Commit();

    if (dxdev) dxdev->Release();

    gpu_chams_alloc(w, h);
    g_ready = 1;
    return 1;
}

int gpu_ready(void) { return g_ready; }

void gpu_shutdown(void) {
    if (g_frame_event) { CloseHandle(g_frame_event); g_frame_event = NULL; }
    InterlockedExchange(&g_frame_granted, 0);
    release_target();
    if (g_mesh_buffer) { g_mesh_buffer->Release(); g_mesh_buffer=NULL; g_mesh_buffer_bytes=0; }
    if (g_mesh_layout) { g_mesh_layout->Release(); g_mesh_layout=NULL; }
    if (g_mesh_vs) { g_mesh_vs->Release(); g_mesh_vs=NULL; }
    if (g_mesh_ps) { g_mesh_ps->Release(); g_mesh_ps=NULL; }
    g_mesh_vertices.clear();
    if (g_brush) { g_brush->Release(); g_brush = NULL; }
    if (g_fmt) { g_fmt->Release(); g_fmt = NULL; }
    if (g_fontcoll) { g_fontcoll->Release(); g_fontcoll = NULL; }
    if (g_dwf) { g_dwf->Release(); g_dwf = NULL; }
    if (g_dvisual) { g_dvisual->Release(); g_dvisual = NULL; }
    if (g_dtarget) { g_dtarget->Release(); g_dtarget = NULL; }
    if (g_dcomp) { g_dcomp->Release(); g_dcomp = NULL; }
    if (g_d2dc) { g_d2dc->Release(); g_d2dc = NULL; }
    if (g_d2ddev) { g_d2ddev->Release(); g_d2ddev = NULL; }
    if (g_d2df) { g_d2df->Release(); g_d2df = NULL; }
    if (g_swap) { g_swap->Release(); g_swap = NULL; }
    if (g_ctx) { g_ctx->Release(); g_ctx = NULL; }
    if (g_dev) { g_dev->Release(); g_dev = NULL; }
    g_ready = 0;
}

void gpu_size(int w, int h) {
    if (!g_swap || w <= 0 || h <= 0) return;
    if (w == g_w && h == g_h) return;
    release_target();
    g_batch = 0;
    g_ctx->OMSetRenderTargets(0, NULL, NULL);
    if (FAILED(g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, g_swap_flags))) return;
    g_w = w; g_h = h;
    gpu_chams_alloc(w, h);
}

/* Frame. */

void gpu_begin(void) {
    if (!g_ready || !g_swap) return;
    if (g_batch) return;   /* a frame was never presented; do not nest */
    g_mesh_vertices.clear();

    ID3D11Texture2D* back = NULL;
    if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) return;

    /* Clear the composition surface to transparent so uncovered pixels show
     * the windows beneath the overlay.
     */
    ID3D11RenderTargetView* rtv = NULL;
    if (SUCCEEDED(g_dev->CreateRenderTargetView(back, NULL, &rtv)) && rtv) {
        const float clear[4] = { 0, 0, 0, 0 };
        g_ctx->ClearRenderTargetView(rtv, clear);
        g_ctx->OMSetRenderTargets(1, &rtv, NULL);
        D3D11_VIEWPORT vp = { 0, 0, (float)g_w, (float)g_h, 0, 1 };
        g_ctx->RSSetViewports(1, &vp);
        rtv->Release();
    }

    release_target();
    IDXGISurface* surf = NULL;
    if (SUCCEEDED(back->QueryInterface(__uuidof(IDXGISurface), (void**)&surf)) && surf) {
        D2D1_BITMAP_PROPERTIES1 bp = {};
        bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        g_d2dc->CreateBitmapFromDxgiSurface(surf, &bp, &g_target);
        surf->Release();
    }
    back->Release();
    if (g_target) g_d2dc->SetTarget(g_target);
}

int gpu_frame_ready(void) {
    if (!g_frame_event) return 1;
    if (InterlockedCompareExchange(&g_frame_granted, 0, 0)) return 1;
    if (WaitForSingleObject(g_frame_event, 0) != WAIT_OBJECT_0) return 0;
    InterlockedExchange(&g_frame_granted, 1);
    return 1;
}

int gpu_present(void) {
    if (!g_ready || !g_swap) return 0;
    if (g_batch) {
        g_d2dc->SetTarget(NULL);
        g_d2dc->EndDraw();
        g_batch = 0;
    }
    g_brush_valid = 0;
    /* Present without waiting for vertical sync. If the compositor queue is
     * full, skip this present so the window thread can continue processing input.
     */
    HRESULT hr = g_swap->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
    if (FAILED(hr)) return 0;
    InterlockedExchange(&g_frame_granted, 0);
    return 1;
}
