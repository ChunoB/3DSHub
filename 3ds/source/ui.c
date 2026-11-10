#include "ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <3ds/util/utf.h>

#define Z 0.5f   // todos na mesma profundidade: o que é desenhado depois fica por cima

C3D_RenderTarget *alvoCima, *alvoCimaDir, *alvoBaixo;

static C2D_TextBuf bufTexto;
static float alturaLinha = 30;   // altura de linha da fonte do sistema, em pixels

// estado da barra de status (atualizado de tempos em tempos, não a cada quadro)
static u32 quadro;
static char hora[8] = "--:--";
static u8 bateria = 5, carregando = 0, wifi = 0;
static bool mcuOk;
static u8 porcento = 0xFF;                   // bateria de 0 a 100 (0xFF = não sabe)
static bool temIP;

// Posição de cada pixel dentro do bloco 8x8 da textura (ordem Z), já separada em
// coluna e linha: posição = zCol[x & 7] + zLin[y & 7]. Montada uma vez em uiIniciar.
static u8 zCol[8], zLin[8];

// ---------------------------------------------------------------- telas

void uiIniciar(void) {
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(6000);
    C2D_Prepare();
    alvoCima    = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    alvoCimaDir = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
    alvoBaixo   = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    bufTexto = C2D_TextBufNew(6000);
    FINF_s *info = C2D_FontGetInfo(NULL);
    if (info && info->lineFeed) alturaLinha = info->lineFeed;
    ptmuInit();
    mcuOk = R_SUCCEEDED(mcuHwcInit());       // porcentagem exata da bateria
    for (int i = 0; i < 8; i++) {
        zCol[i] = (i & 1) | ((i & 2) << 1) | ((i & 4) << 2);
        zLin[i] = zCol[i] << 1;
    }
}

void uiEncerrar(void) {
    uiSomFim();
    ptmuExit();
    if (mcuOk) mcuHwcExit();
    C2D_TextBufDelete(bufTexto);
    C2D_Fini();
    C3D_Fini();
}

static void atualizarStatus(void) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (tm) strftime(hora, sizeof(hora), "%H:%M", tm);
    PTMU_GetBatteryLevel(&bateria);          // 0 a 5
    PTMU_GetBatteryChargeState(&carregando);
    if (!mcuOk || R_FAILED(MCUHWC_GetBatteryLevel(&porcento)) || porcento > 100) porcento = 0xFF;
    wifi = osGetWifiStrength();              // 0 a 3
    temIP = gethostid() != 0;
}

void uiQuadro(void) {
    uiSomQuadro();
    if (quadro++ % 60 == 0) atualizarStatus();
    C2D_TextBufClear(bufTexto);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
}

void uiTela(C3D_RenderTarget *alvo) {
    C2D_TargetClear(alvo, COR_FUNDO);
    C2D_SceneBegin(alvo);
}

void uiTelaGrad(C3D_RenderTarget *alvo, u32 corCima, u32 corBaixo) {
    C2D_TargetClear(alvo, corBaixo);
    C2D_SceneBegin(alvo);
    uiRetGrad(0, 0, alvo == alvoBaixo ? 320 : 400, 240, corCima, corBaixo);
}

void uiFim(void) {
    C3D_FrameEnd(0);
}

// ---------------------------------------------------------------- cor e movimento

u32 uiMistura(u32 a, u32 b, float t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    u32 r = 0;
    for (int i = 0; i < 32; i += 8) {
        float ca = (a >> i) & 0xFF, cb = (b >> i) & 0xFF;
        r |= (u32)(ca + (cb - ca) * t + 0.5f) << i;
    }
    return r;
}

u32 uiAlfa(u32 cor, float alfa) {
    if (alfa < 0) alfa = 0;
    if (alfa > 1) alfa = 1;
    return (cor & 0x00FFFFFFu) | ((u32)((cor >> 24) * alfa + 0.5f) << 24);
}

float uiSuave(float atual, float alvo, float taxa) {
    float d = alvo - atual;
    if (d > -0.002f && d < 0.002f) return alvo;
    return atual + d * taxa;
}

// ---------------------------------------------------------------- formas

void uiRet(float x, float y, float w, float h, u32 cor) {
    if (w <= 0 || h <= 0) return;
    C2D_DrawRectSolid(x, y, Z, w, h, cor);
}

void uiRetArred(float x, float y, float w, float h, float r, u32 cor) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    if (r < 0.5f) {
        uiRet(x, y, w, h, cor);
        return;
    }
    uiRet(x + r, y, w - 2 * r, h, cor);              // miolo, de cima a baixo
    uiRet(x, y + r, r, h - 2 * r, cor);              // lateral esquerda
    uiRet(x + w - r, y + r, r, h - 2 * r, cor);      // lateral direita
    C2D_DrawCircleSolid(x + r, y + r, Z, r, cor);    // cantos
    C2D_DrawCircleSolid(x + w - r, y + r, Z, r, cor);
    C2D_DrawCircleSolid(x + r, y + h - r, Z, r, cor);
    C2D_DrawCircleSolid(x + w - r, y + h - r, Z, r, cor);
}

void uiCirculo(float cx, float cy, float r, u32 cor) {
    if (r > 0) C2D_DrawCircleSolid(cx, cy, Z, r, cor);
}

void uiAnel(float cx, float cy, float r, float espessura, u32 cor, u32 corDentro) {
    uiCirculo(cx, cy, r, cor);
    uiCirculo(cx, cy, r - espessura, corDentro);
}

void uiLinha(float x0, float y0, float x1, float y1, float espessura, u32 cor) {
    C2D_DrawLine(x0, y0, cor, x1, y1, cor, espessura, Z);
}

void uiTri(float x0, float y0, float x1, float y1, float x2, float y2, u32 cor) {
    C2D_DrawTriangle(x0, y0, cor, x1, y1, cor, x2, y2, cor, Z);
}

void uiQuad(float cx, float cy, float w, float h, float a, u32 cor) {
    float c = cosf(a), s = sinf(a), hw = w / 2, hh = h / 2;
    // os 4 cantos girados em volta do centro
    float x0 = cx + (-hw * c - -hh * s), y0 = cy + (-hw * s + -hh * c);
    float x1 = cx + ( hw * c - -hh * s), y1 = cy + ( hw * s + -hh * c);
    float x2 = cx + ( hw * c -  hh * s), y2 = cy + ( hw * s +  hh * c);
    float x3 = cx + (-hw * c -  hh * s), y3 = cy + (-hw * s +  hh * c);
    uiTri(x0, y0, x1, y1, x2, y2, cor);
    uiTri(x0, y0, x2, y2, x3, y3, cor);
}

void uiBarra(float x, float y, float w, float h, float f, u32 cor, u32 corFundo) {
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    uiRetArred(x, y, w, h, h / 2, corFundo);
    if (f > 0) uiRetArred(x, y, w * f < h ? h : w * f, h, h / 2, cor);
}

void uiRetGrad(float x, float y, float w, float h, u32 corCima, u32 corBaixo) {
    if (w <= 0 || h <= 0) return;
    C2D_DrawRectangle(x, y, Z, w, h, corCima, corCima, corBaixo, corBaixo);
}

void uiCartaoCor(float x, float y, float w, float h, float raio, u32 cor, u32 corBorda) {
    uiRetArred(x, y, w, h, raio, corBorda);
    uiRetArred(x + 1, y + 1, w - 2, h - 2, raio - 1, cor);
}

void uiCartao(float x, float y, float w, float h, float raio) {
    uiCartaoCor(x, y, w, h, raio, COR_CARTAO, COR_BORDA);
}

void uiCartaoSel(float x, float y, float w, float h, float raio, bool escolhido, u32 corEscolhido) {
    if (escolhido) uiRetArred(x, y, w, h, raio, corEscolhido);
    else uiCartao(x, y, w, h, raio);
}

// ---------------------------------------------------------------- texto

static float larguraCaractere(u32 codigo, float escala) {
    charWidthInfo_s *info = C2D_FontGetCharWidthInfo(NULL, C2D_FontGlyphIndexFromCodePoint(NULL, codigo));
    return info ? info->charWidth * escala : 0;
}

float uiTexto(float x, float y, float tamanho, u32 cor, int alinhamento, float larguraMax,
              const char *fmt, ...) {
    char s[200];
    va_list args;
    va_start(args, fmt);
    vsnprintf(s, sizeof(s), fmt, args);
    va_end(args);

    float escala = tamanho / alturaLinha;

    // Não cabe? Corta no último caractere em que "..." ainda cabe.
    // Mede pela largura de cada letra da fonte (sem gastar o buffer de texto).
    if (larguraMax > 0) {
        float reticencias = 3 * larguraCaractere('.', escala);
        float w = 0;
        const u8 *p = (const u8 *)s, *corte = NULL;
        while (*p) {
            u32 codigo;
            ssize_t n = decode_utf8(&codigo, p);
            if (n <= 0) break;
            float cw = larguraCaractere(codigo, escala);
            if (!corte && w + cw > larguraMax - reticencias) corte = p;
            w += cw;
            p += n;
        }
        if (w > larguraMax && corte) {
            size_t pos = corte - (const u8 *)s;
            if (pos > sizeof(s) - 4) pos = sizeof(s) - 4;
            memcpy(s + pos, "...", 4);
        }
    }

    C2D_Text t;
    if (!C2D_TextParse(&t, bufTexto, s)) return 0;
    C2D_TextOptimize(&t);
    float w, h;
    C2D_TextGetDimensions(&t, escala, escala, &w, &h);
    // com várias linhas, cada uma é alinhada por conta própria (centro ou direita)
    u32 modo = alinhamento == CENTRO ? C2D_AlignCenter : alinhamento == DIR ? C2D_AlignRight : C2D_AlignLeft;
    C2D_DrawText(&t, C2D_WithColor | modo, x, y, Z, escala, escala, cor);
    return w;
}

float uiLarguraTexto(const char *s, float tamanho) {
    float escala = tamanho / alturaLinha, w = 0;
    const u8 *p = (const u8 *)s;
    while (*p) {
        u32 codigo;
        ssize_t n = decode_utf8(&codigo, p);
        if (n <= 0) break;
        w += larguraCaractere(codigo, escala);
        p += n;
    }
    return w;
}

float uiLarguraTextoMax(const char *s, float tamanho, float larguraMax) {
    float w = uiLarguraTexto(s, tamanho);
    return larguraMax > 0 && w > larguraMax ? larguraMax : w;
}

// ---------------------------------------------------------------- ícones
// Desenhados numa grade de 24x24 (u = 1/24 do tamanho), centrados em (cx, cy).

static void altoFalante(float cx, float cy, float u, u32 cor) {
    uiRet(cx - 9 * u, cy - 3.5f * u, 3.5f * u, 7 * u, cor);
    uiTri(cx - 6 * u, cy - 3.5f * u, cx + 1 * u, cy - 8 * u, cx + 1 * u, cy + 8 * u, cor);
    uiTri(cx - 6 * u, cy - 3.5f * u, cx + 1 * u, cy + 8 * u, cx - 6 * u, cy + 3.5f * u, cor);
}

static void monitor(float cx, float cy, float u, u32 cor, u32 fundo) {
    uiRetArred(cx - 10 * u, cy - 8 * u, 20 * u, 13 * u, 2 * u, cor);
    uiRet(cx - 8 * u, cy - 6 * u, 16 * u, 9 * u, fundo);
    uiRet(cx - 1 * u, cy + 5 * u, 2 * u, 3 * u, cor);
    uiRetArred(cx - 5 * u, cy + 7.5f * u, 10 * u, 2 * u, u, cor);
}

void uiIcone(int icone, float cx, float cy, float tamanho, u32 cor, u32 fundo) {
    float u = tamanho / 24.0f;
    #define P(px, py) cx + (px) * u, cy + (py) * u      // ponto na grade
    switch (icone) {
    case IC_CONTROLE:
        uiRetArred(cx - 10 * u, cy - 6 * u, 20 * u, 11 * u, 5 * u, cor);
        uiCirculo(cx - 7 * u, cy + 3.5f * u, 4 * u, cor);
        uiCirculo(cx + 7 * u, cy + 3.5f * u, 4 * u, cor);
        uiRet(cx - 9 * u, cy - 1.5f * u, 5 * u, 2 * u, fundo);       // direcional
        uiRet(cx - 7.5f * u, cy - 3 * u, 2 * u, 5 * u, fundo);
        uiCirculo(cx + 5.5f * u, cy - 2 * u, 1.3f * u, fundo);       // botões
        uiCirculo(cx + 8 * u, cy + 0.3f * u, 1.3f * u, fundo);
        break;
    case IC_GRAFICO:
        uiLinha(P(-9, 5), P(-4, -1), 2.4f * u, cor);
        uiLinha(P(-4, -1), P(1, 3), 2.4f * u, cor);
        uiLinha(P(1, 3), P(9, -7), 2.4f * u, cor);
        uiLinha(P(-10, 9), P(10, 9), 1.8f * u, cor);
        break;
    case IC_PIANO:
        uiRetArred(cx - 10 * u, cy - 8 * u, 20 * u, 16 * u, 2 * u, cor);
        for (int i = -1; i <= 1; i++) uiRet(cx + (i * 5 - 0.6f) * u, cy - 8 * u, 1.2f * u, 16 * u, fundo);
        uiRet(cx - 6.5f * u, cy - 8 * u, 3 * u, 9 * u, fundo);       // teclas pretas
        uiRet(cx + 3.5f * u, cy - 8 * u, 3 * u, 9 * u, fundo);
        break;
    case IC_CAMERA:
        uiRetArred(cx - 10 * u, cy - 6 * u, 20 * u, 14 * u, 3 * u, cor);
        uiRetArred(cx - 4 * u, cy - 9 * u, 8 * u, 4 * u, 1.5f * u, cor);
        uiAnel(cx, cy + 1 * u, 4.8f * u, 2 * u, fundo, cor);
        break;
    case IC_ESPELHO:
        monitor(cx, cy, u, cor, fundo);
        uiRet(cx - 4 * u, cy - 3.5f * u, 8 * u, 4 * u, cor);            // janela dentro da tela
        break;
    case IC_PC:
        monitor(cx, cy, u, cor, fundo);
        break;
    case IC_ENGRENAGEM:
        for (int i = 0; i < 8; i++) {
            float a = i * (float)M_PI / 4;
            uiQuad(cx + cosf(a) * 7.5f * u, cy + sinf(a) * 7.5f * u, 4 * u, 4 * u, a, cor);
        }
        uiCirculo(cx, cy, 7 * u, cor);
        uiCirculo(cx, cy, 3 * u, fundo);
        break;
    case IC_PLAY:
        uiTri(P(-6, -8), P(8, 0), P(-6, 8), cor);
        break;
    case IC_PAUSE:
        uiRetArred(cx - 6 * u, cy - 8 * u, 4 * u, 16 * u, u, cor);
        uiRetArred(cx + 2 * u, cy - 8 * u, 4 * u, 16 * u, u, cor);
        break;
    case IC_PROXIMA:
        uiTri(P(-7, -7), P(4, 0), P(-7, 7), cor);
        uiRet(cx + 4.5f * u, cy - 7 * u, 2.5f * u, 14 * u, cor);
        break;
    case IC_ANTERIOR:
        uiTri(P(7, -7), P(-4, 0), P(7, 7), cor);
        uiRet(cx - 7 * u, cy - 7 * u, 2.5f * u, 14 * u, cor);
        break;
    case IC_VOL_MAIS:
        altoFalante(cx, cy, u, cor);
        uiRet(cx + 3 * u, cy - 1.1f * u, 7 * u, 2.2f * u, cor);
        uiRet(cx + 5.4f * u, cy - 3.5f * u, 2.2f * u, 7 * u, cor);
        break;
    case IC_VOL_MENOS:
        altoFalante(cx, cy, u, cor);
        uiRet(cx + 3 * u, cy - 1.1f * u, 7 * u, 2.2f * u, cor);
        break;
    case IC_MUDO:
        altoFalante(cx, cy, u, cor);
        uiQuad(cx + 6.5f * u, cy, 8 * u, 2.2f * u, (float)M_PI / 4, cor);
        uiQuad(cx + 6.5f * u, cy, 8 * u, 2.2f * u, -(float)M_PI / 4, cor);
        break;
    case IC_MUSICA:
        uiCirculo(cx - 3 * u, cy + 5 * u, 3.6f * u, cor);
        uiRet(cx - 0.2f * u, cy - 8 * u, 2.2f * u, 13 * u, cor);
        uiQuad(cx + 4 * u, cy - 6 * u, 6.5f * u, 2.4f * u, 0.55f, cor);
        break;
    case IC_OK:
        uiLinha(P(-7, 0), P(-2, 5), 2.8f * u, cor);
        uiLinha(P(-2, 5), P(7, -6), 2.8f * u, cor);
        break;
    case IC_X:
        uiLinha(P(-6, -6), P(6, 6), 2.8f * u, cor);
        uiLinha(P(-6, 6), P(6, -6), 2.8f * u, cor);
        break;
    case IC_SETA_BAIXO:
        uiLinha(P(0, -8), P(0, 2), 2.6f * u, cor);
        uiTri(P(-6, 1), P(6, 1), P(0, 8), cor);
        break;
    case IC_SETA_CIMA:
        uiLinha(P(0, 8), P(0, -2), 2.6f * u, cor);
        uiTri(P(-6, -1), P(6, -1), P(0, -8), cor);
        break;
    case IC_LUPA:
        uiAnel(cx - 2 * u, cy - 2 * u, 6.5f * u, 2.3f * u, cor, fundo);
        uiLinha(P(3, 3), P(8.5f, 8.5f), 3 * u, cor);
        break;
    case IC_LED:
        uiCirculo(cx, cy, 3.5f * u, cor);
        for (int i = 0; i < 8; i++) {
            float a = i * (float)M_PI / 4;
            uiLinha(cx + cosf(a) * 6 * u, cy + sinf(a) * 6 * u,
                    cx + cosf(a) * 9 * u, cy + sinf(a) * 9 * u, 1.8f * u, cor);
        }
        break;
    case IC_RAIO:
        uiTri(P(2, -10), P(-6, 1), P(1, 1), cor);
        uiTri(P(-2, 10), P(6, -1), P(-1, -1), cor);
        break;
    case IC_LISTA:
        for (int i = -1; i <= 1; i++) {
            uiCirculo(cx - 7 * u, cy + i * 6 * u, 1.6f * u, cor);
            uiRetArred(cx - 4 * u, cy + (i * 6 - 1.1f) * u, 13 * u, 2.2f * u, u, cor);
        }
        break;
    case IC_TELA_CHEIA:
        uiLinha(P(-9, -4), P(-9, -9), 2.4f * u, cor); uiLinha(P(-9, -9), P(-4, -9), 2.4f * u, cor);
        uiLinha(P(4, -9), P(9, -9), 2.4f * u, cor);   uiLinha(P(9, -9), P(9, -4), 2.4f * u, cor);
        uiLinha(P(9, 4), P(9, 9), 2.4f * u, cor);     uiLinha(P(9, 9), P(4, 9), 2.4f * u, cor);
        uiLinha(P(-4, 9), P(-9, 9), 2.4f * u, cor);   uiLinha(P(-9, 9), P(-9, 4), 2.4f * u, cor);
        break;
    case IC_GIRO:
        uiAnel(cx, cy, 9 * u, 2 * u, cor, fundo);
        uiAnel(cx, cy, 5 * u, 2 * u, cor, fundo);
        uiCirculo(cx, cy, 1.8f * u, cor);
        break;
    case IC_TECLADO:
        uiRetArred(cx - 11 * u, cy - 7 * u, 22 * u, 14 * u, 2.5f * u, cor);
        for (int l = 0; l < 2; l++)
            for (int c = 0; c < 5; c++)
                uiRet(cx + (-8.5f + c * 4) * u, cy + (-4.5f + l * 4) * u, 2.4f * u, 2.4f * u, fundo);
        uiRet(cx - 5 * u, cy + 3.5f * u, 10 * u, 2 * u, fundo);
        break;
    case IC_BICHO:                       // bolinha com orelhas e olhos
        uiCirculo(cx, cy + 2 * u, 9 * u, cor);
        uiTri(P(-8, -3), P(-6, -11), P(-2, -6), cor);
        uiTri(P(8, -3), P(6, -11), P(2, -6), cor);
        uiCirculo(cx - 3.5f * u, cy + 1 * u, 1.8f * u, fundo);
        uiCirculo(cx + 3.5f * u, cy + 1 * u, 1.8f * u, fundo);
        uiRet(cx - 2 * u, cy + 5 * u, 4 * u, 1.4f * u, fundo);
        break;
    case IC_MICROFONE:
        uiRetArred(cx - 4 * u, cy - 11 * u, 8 * u, 13 * u, 4 * u, cor);
        uiAnel(cx, cy - 1 * u, 7 * u, 1.8f * u, cor, fundo);
        uiRet(cx - 8 * u, cy - 9 * u, 16 * u, 8 * u, fundo);          // tira a parte de cima do arco
        uiRetArred(cx - 4 * u, cy - 11 * u, 8 * u, 13 * u, 4 * u, cor);
        uiRet(cx - 1 * u, cy + 6 * u, 2 * u, 4 * u, cor);
        uiRetArred(cx - 5 * u, cy + 9 * u, 10 * u, 2 * u, u, cor);
        break;
    case IC_PEIXE:
        uiCirculo(cx - 2 * u, cy, 7 * u, cor);
        uiQuad(cx - 2 * u, cy, 13 * u, 9 * u, 0, cor);
        uiTri(P(4, 0), P(11, -7), P(11, 7), cor);                        // rabo
        uiCirculo(cx - 5 * u, cy - 1.5f * u, 1.6f * u, fundo);           // olho
        break;
    }
    #undef P
}

// ---------------------------------------------------------------- componentes

void uiBarraStatus(const char *titulo, int icone, u32 cor) {
    uiRet(0, 0, 400, UI_BARRA, COR_BARRA);
    uiRet(0, UI_BARRA - 1, 400, 1, COR_BORDA);
    if (icone >= 0) uiIcone(icone, 13, UI_BARRA / 2, 15, cor, COR_BARRA);
    uiTexto(icone >= 0 ? 26 : 8, 4, 14, cor, ESQ, 200, "%s", titulo);

    // bateria (5 níveis) no canto
    float bx = 372, by = 6.5f;
    uiRet(bx, by, 20, 10, COR_MUDO);
    uiRet(bx + 1.3f, by + 1.3f, 17.4f, 7.4f, COR_BARRA);
    uiRet(bx + 20, by + 3, 1.8f, 4, COR_MUDO);
    float nivel = porcento <= 100 ? porcento / 100.0f : bateria > 5 ? 1 : bateria / 5.0f;
    bool baixa = porcento <= 100 ? porcento <= 15 : bateria <= 1;
    uiRet(bx + 2.3f, by + 2.3f, 15.4f * nivel, 5.4f, baixa ? COR_VERMELHO : COR_TEXTO2);

    // porcentagem ao lado do ícone (e o raio, carregando)
    float x = bx - 4;
    if (porcento <= 100) x -= uiTexto(x, 5, 12, baixa && !carregando ? COR_VERMELHO : COR_TEXTO2, DIR, 0, "%d%%", porcento) + 4;
    if (carregando) {
        uiIcone(IC_RAIO, x - 4, UI_BARRA / 2, 12, COR_AMARELO, COR_BARRA);
        x -= 12;
    }

    // Wi-Fi: 3 barrinhas (cinza = sem rede)
    float wx = x - 18;
    for (int i = 0; i < 3; i++) {
        float h = 4 + i * 3;
        bool acesa = temIP && wifi > i;
        uiRet(wx + i * 5, 16 - h, 3.5f, h, acesa ? COR_TEXTO2 : COR_LINHA);
    }
    uiTexto(wx - 6, 4, 14, COR_TEXTO2, DIR, 0, "%s", hora);
}

void uiBotao(float x, float y, float w, float h, const char *rotulo, int icone,
             u32 corFundo, u32 corConteudo) {
    if (corFundo == COR_CARTAO) uiCartao(x, y, w, h, 9);
    else uiRetArred(x, y, w, h, 9, corFundo);
    bool temTexto = rotulo && rotulo[0];
    if (icone >= 0 && temTexto && w >= 2.2f * h) {           // ícone à esquerda, texto ao lado
        float t = h * 0.45f > 22 ? 22 : h * 0.45f;
        float wt = uiLarguraTextoMax(rotulo, 15, w - 26 - t);
        float x0 = x + (w - (t + 8 + wt)) / 2;          // ícone + texto juntos, no meio do botão
        uiIcone(icone, x0 + t / 2, y + h / 2, t, corConteudo, corFundo);
        uiTexto(x0 + t + 8, y + h / 2 - 8, 15, corConteudo, ESQ, w - 26 - t, "%s", rotulo);
    } else if (icone >= 0 && temTexto) {                     // ícone em cima, texto embaixo
        float t = h * 0.38f > 26 ? 26 : h * 0.38f;
        uiIcone(icone, x + w / 2, y + h / 2 - 7, t, corConteudo, corFundo);
        uiTexto(x + w / 2, y + h - 19, 12, corConteudo, CENTRO, w - 8, "%s", rotulo);
    } else if (icone >= 0) {
        float t = (w < h ? w : h) * 0.5f;
        uiIcone(icone, x + w / 2, y + h / 2, t, corConteudo, corFundo);
    } else if (temTexto) {
        uiTexto(x + w / 2, y + h / 2 - 8, 15, corConteudo, CENTRO, w - 10, "%s", rotulo);
    }
}

bool uiDentro(const touchPosition *tp, float x, float y, float w, float h) {
    return tp->px >= x && tp->px < x + w && tp->py >= y && tp->py < y + h;
}

// ---------------------------------------------------------------- dicas de botão

#define DICA_TEXTO 11.0f

// Só uma letra (A, B, X, Y, L, R) vira um círculo; o resto (START, L/R...) uma pílula.
static float larguraBotaoDica(const char *botao) {
    if (!botao || !botao[0]) return 0;
    if (!botao[1]) return UI_DICA;
    return uiLarguraTexto(botao, 10) + 10;
}

float uiLarguraDica(const char *botao, const char *texto) {
    float w = larguraBotaoDica(botao);
    if (texto && texto[0]) w += (w > 0 ? 4 : 0) + uiLarguraTexto(texto, DICA_TEXTO);
    return w;
}

float uiDica(float x, float y, const char *botao, const char *texto) {
    float wb = larguraBotaoDica(botao);
    if (wb > 0) {
        uiRetArred(x, y, wb, UI_DICA, UI_DICA / 2.0f, COR_LINHA);
        if (!botao[1]) uiTexto(x + wb / 2, y + 1.5f, 10, COR_BRANCO, CENTRO, 0, "%s", botao);
        else           uiTexto(x + wb / 2, y + 1.5f, 10, COR_BRANCO, CENTRO, 0, "%s", botao);
    }
    float w = wb;
    if (texto && texto[0]) {
        if (w > 0) w += 4;
        w += uiTexto(x + w, y + 0.5f, DICA_TEXTO, COR_TEXTO2, ESQ, 0, "%s", texto);
    }
    return w;
}

#define DICA_ESPACO 10
void uiDicas(float x, float y, int alinhamento, ...) {
    const char *pares[16];
    int n = 0;
    va_list args;
    va_start(args, alinhamento);
    while (n < 16) {
        const char *botao = va_arg(args, const char *);
        if (!botao) break;
        pares[n++] = botao;
        pares[n++] = va_arg(args, const char *);
        if (!pares[n - 1]) { pares[n - 1] = ""; break; }
    }
    va_end(args);

    float total = 0;
    for (int i = 0; i < n; i += 2) total += uiLarguraDica(pares[i], pares[i + 1]) + (i ? DICA_ESPACO : 0);
    if (alinhamento == CENTRO) x -= total / 2;
    else if (alinhamento == DIR) x -= total;
    for (int i = 0; i < n; i += 2) x += uiDica(x, y, pares[i], pares[i + 1]) + DICA_ESPACO;
}

void uiRolagem(float x, float y, float h, int total, int visiveis, int primeiro, u32 cor) {
    if (total <= visiveis || visiveis <= 0) return;
    uiRetArred(x, y, 3, h, 1.5f, COR_CARTAO);
    float hp = h * visiveis / total;
    if (hp < 12) hp = 12;
    float f = (float)primeiro / (total - visiveis);
    uiRetArred(x, y + (h - hp) * f, 3, hp, 1.5f, cor);
}

// ---------------------------------------------------------------- aviso

void uiAviso(const char *titulo, const char *texto, int icone, u32 cor) {
    u32 corFraca = uiMistura(COR_CARTAO, cor, 0.22f);
    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        touchPosition tp;
        hidTouchRead(&tp);
        if ((down & (KEY_START | KEY_A | KEY_B)) || ((down & KEY_TOUCH) && uiDentro(&tp, 60, 92, 200, 56))) {
            uiSomTocar(SOM_TIQUE);
            break;
        }

        uiQuadro();
        uiTela(alvoCima);
        uiBarraStatus("3DS Hub", IC_ENGRENAGEM, COR_MUDO);
        uiCartao(12, 32, 376, 196, 14);
        uiCirculo(44, 64, 20, corFraca);
        uiIcone(icone, 44, 64, 22, cor == COR_CARTAO2 ? COR_TEXTO : uiMistura(cor, COR_BRANCO, 0.45f), corFraca);
        uiTexto(74, 52, 20, COR_TEXTO, ESQ, 300, "%s", titulo);
        uiRet(28, 94, 344, 1, COR_BORDA);
        uiTexto(28, 104, 14, COR_TEXTO2, ESQ, 0, "%s", texto);

        uiTela(alvoBaixo);
        uiBotao(60, 92, 200, 56, "OK", IC_OK, cor, COR_BRANCO);
        uiDicas(160, 160, CENTRO, "A", "ou toque em OK", NULL);
        uiFim();
    }
}

bool uiConfirmar(const char *modulo, int icone, u32 cor, const char *titulo, const char *texto,
                 const char *sim, const char *nao, void (*aCadaQuadro)(void)) {
    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        touchPosition tp;
        hidTouchRead(&tp);
        bool sim_ = down & KEY_A, nao_ = down & (KEY_B | KEY_START);
        if (down & KEY_TOUCH) {
            if (uiDentro(&tp, 10, 92, 145, 56)) sim_ = true;
            if (uiDentro(&tp, 165, 92, 145, 56)) nao_ = true;
        }
        if (sim_ || nao_) {
            uiSomTocar(sim_ ? SOM_ABRIR : SOM_VOLTAR);
            return sim_;
        }
        if (aCadaQuadro) aCadaQuadro();

        uiQuadro();
        uiTela(alvoCima);
        uiBarraStatus(modulo, icone, cor);
        uiCartao(12, 32, 376, 196, 14);
        uiTexto(200, 52, 20, COR_TEXTO, CENTRO, 350, "%s", titulo);
        uiRet(28, 86, 344, 1, COR_BORDA);
        uiTexto(200, 98, 14, COR_TEXTO2, CENTRO, 0, "%s", texto);

        uiTela(alvoBaixo);
        uiBotao(10, 92, 145, 56, sim, IC_OK, cor, COR_BRANCO);
        uiBotao(165, 92, 145, 56, nao, IC_X, COR_CARTAO, COR_TEXTO);
        uiDicas(160, 160, CENTRO, "A", sim, "B", nao, NULL);
        uiFim();
    }
    return false;
}

// ---------------------------------------------------------------- imagens

// A imagem vai para a textura pela CPU, no formato nativo da GPU do 3DS, sem a
// transferência automática (que deixava dúvida sobre a orientação):
//   - blocos de 8x8 pixels, da esquerda para a direita e de cima para baixo: a
//     linha 0 da memória da textura é o TOPO (v = 1). Confirmado no 3DS: contando
//     de baixo para cima, a imagem saiu exatamente de cabeça para baixo;
//   - dentro do bloco, ordem Z (bits de x e y intercalados, x no bit mais baixo).

// Copia a região (em pixels da imagem) para a textura. É o caminho mais quente do
// Espelho e da Steam (a tela inteira a cada quadro), então anda de bloco em bloco:
// numa linha, os 8 pixels de um bloco caem em 4 pares vizinhos (posições 0, 4, 16 e
// 20 a partir da linha), e cada par vai de uma vez só, como um número de 32 bits.
typedef u32 __attribute__((may_alias)) u32par;

static void enviarRegiao(UiImagem *im, int x0, int y0, int x1, int y1) {
    x0 &= ~7;                                        // blocos inteiros (o que sobra em
    x1 = (x1 + 7) & ~7;                              // volta já é igual na imagem)
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > UI_IMG_LARG) x1 = UI_IMG_LARG;
    if (y1 > UI_IMG_ALT) y1 = UI_IMG_ALT;
    int blocos = (x1 - x0) >> 3;
    if (blocos <= 0 || y1 <= y0) return;
    u16 *destino = (u16 *)im->tex.data;
    for (int y = y0; y < y1; y++) {
        // linha 0 da imagem = topo da textura (v = 1)
        u32par *d = (u32par *)(destino + ((y >> 3) * (UI_IMG_LARG / 8) + (x0 >> 3)) * 64 + zLin[y & 7]);
        const u32par *o = (const u32par *)(im->pixels + y * UI_IMG_LARG + x0);
        for (int b = 0; b < blocos; b++, d += 32, o += 4) {
            d[0]  = o[0];
            d[2]  = o[1];
            d[8]  = o[2];
            d[10] = o[3];
        }
    }
    C3D_TexFlush(&im->tex);                          // a GPU lê da memória, não do cache da CPU
}

static bool criarImagem(UiImagem *im, int w, int h, GPU_TEXCOLOR formato);

bool uiImagemCriar(UiImagem *im, int w, int h) { return criarImagem(im, w, h, GPU_RGB565); }
bool uiImagemCriarAlfa(UiImagem *im, int w, int h) { return criarImagem(im, w, h, GPU_RGBA4); }

static bool criarImagem(UiImagem *im, int w, int h, GPU_TEXCOLOR formato) {
    memset(im, 0, sizeof(*im));
    if (!C3D_TexInit(&im->tex, UI_IMG_LARG, UI_IMG_ALT, formato)) return false;
    C3D_TexSetFilter(&im->tex, GPU_LINEAR, GPU_LINEAR);
    im->pixels = linearAlloc(UI_IMG_LARG * UI_IMG_ALT * sizeof(u16));
    if (!im->pixels) {
        C3D_TexDelete(&im->tex);
        return false;
    }
    memset(im->pixels, 0, UI_IMG_LARG * UI_IMG_ALT * sizeof(u16));
    memset(im->tex.data, 0, UI_IMG_LARG * UI_IMG_ALT * sizeof(u16));
    // a imagem (linhas 0..h-1, de cima para baixo) ocupa o alto da textura:
    // topo em v = 1, fundo em v = 1 - h/ALT (o resto, embaixo, não aparece)
    im->sub = (Tex3DS_SubTexture){ (u16)w, (u16)h, 0.0f, 1.0f,
                                   (float)w / UI_IMG_LARG, 1.0f - (float)h / UI_IMG_ALT };
    im->img.tex = &im->tex;
    im->img.subtex = &im->sub;
    im->w = w;
    im->h = h;
    im->ok = true;
    uiImagemEnviar(im);
    return true;
}

void uiImagemEnviar(UiImagem *im) {
    if (im->ok) enviarRegiao(im, 0, 0, im->w, im->h);
}

void uiImagemDesenhar(UiImagem *im, float x, float y) {
    uiImagemDesenharEscala(im, x, y, 1.0f);
}

void uiImagemDesenharEscala(UiImagem *im, float x, float y, float escala) {
    if (im->ok) C2D_DrawImageAt(im->img, x, y, Z, NULL, escala, escala);
}

void uiImagemEnviarParte(UiImagem *im, int x0, int y0, int w, int h) {
    if (im->ok) enviarRegiao(im, x0, y0, x0 + w, y0 + h);
}

void uiImagemFiltro(UiImagem *im, bool suave) {
    if (im->ok) C3D_TexSetFilter(&im->tex, suave ? GPU_LINEAR : GPU_NEAREST, suave ? GPU_LINEAR : GPU_NEAREST);
}

void uiImagemDesenharParte(UiImagem *im, int sx, int sy, int w, int h, float x, float y, float escala) {
    if (!im->ok) return;
    // mesma conta do uiImagemCriar: a linha 0 da imagem é o topo (v = 1)
    Tex3DS_SubTexture sub = { (u16)w, (u16)h, (float)sx / UI_IMG_LARG, 1.0f - (float)sy / UI_IMG_ALT,
                              (float)(sx + w) / UI_IMG_LARG, 1.0f - (float)(sy + h) / UI_IMG_ALT };
    C2D_Image img = { &im->tex, &sub };
    C2D_DrawImageAt(img, x, y, Z, NULL, escala, escala);
}

void uiImagemLiberar(UiImagem *im) {
    if (!im->ok) return;
    C3D_TexDelete(&im->tex);
    linearFree(im->pixels);
    im->ok = false;
}
