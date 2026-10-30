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
static bool temIP;

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
}

void uiEncerrar(void) {
    ptmuExit();
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
    wifi = osGetWifiStrength();              // 0 a 3
    temIP = gethostid() != 0;
}

void uiQuadro(void) {
    if (quadro++ % 60 == 0) atualizarStatus();
    C2D_TextBufClear(bufTexto);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
}

void uiTela(C3D_RenderTarget *alvo) {
    C2D_TargetClear(alvo, COR_FUNDO);
    C2D_SceneBegin(alvo);
}

void uiFim(void) {
    C3D_FrameEnd(0);
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
    float dx = alinhamento == CENTRO ? x - w / 2 : alinhamento == DIR ? x - w : x;
    C2D_DrawText(&t, C2D_WithColor, dx, y, Z, escala, escala, cor);
    return w;
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
    if (icone >= 0) uiIcone(icone, 13, UI_BARRA / 2, 15, cor, COR_BARRA);
    uiTexto(icone >= 0 ? 26 : 8, 4, 14, cor, ESQ, 200, "%s", titulo);

    // bateria (5 níveis) no canto
    float bx = 372, by = 6.5f;
    uiRet(bx, by, 20, 10, COR_MUDO);
    uiRet(bx + 1.3f, by + 1.3f, 17.4f, 7.4f, COR_BARRA);
    uiRet(bx + 20, by + 3, 1.8f, 4, COR_MUDO);
    float nivel = bateria > 5 ? 1 : bateria / 5.0f;
    uiRet(bx + 2.3f, by + 2.3f, 15.4f * nivel, 5.4f, bateria <= 1 ? COR_VERMELHO : COR_TEXTO2);
    if (carregando) uiIcone(IC_RAIO, bx - 7, UI_BARRA / 2, 12, COR_AMARELO, COR_BARRA);

    // Wi-Fi: 3 barrinhas (cinza = sem rede)
    float wx = 344;
    for (int i = 0; i < 3; i++) {
        float h = 4 + i * 3;
        bool acesa = temIP && wifi > i;
        uiRet(wx + i * 5, 16 - h, 3.5f, h, acesa ? COR_TEXTO2 : COR_LINHA);
    }
    uiTexto(338, 4, 14, COR_TEXTO2, DIR, 0, "%s", hora);
}

void uiBotao(float x, float y, float w, float h, const char *rotulo, int icone,
             u32 corFundo, u32 corConteudo) {
    uiRetArred(x, y, w, h, 9, corFundo);
    bool temTexto = rotulo && rotulo[0];
    if (icone >= 0 && temTexto && w >= 2.2f * h) {           // ícone à esquerda, texto ao lado
        float t = h * 0.45f > 22 ? 22 : h * 0.45f;
        uiIcone(icone, x + 10 + t / 2, y + h / 2, t, corConteudo, corFundo);
        uiTexto(x + 18 + t, y + h / 2 - 8, 15, corConteudo, ESQ, w - 26 - t, "%s", rotulo);
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

void uiAviso(const char *titulo, const char *texto, int icone, u32 cor) {
    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        touchPosition tp;
        hidTouchRead(&tp);
        if (down & (KEY_START | KEY_A)) break;
        if ((down & KEY_TOUCH) && uiDentro(&tp, 60, 150, 200, 56)) break;

        uiQuadro();
        uiTela(alvoCima);
        uiBarraStatus("3DS Hub", IC_ENGRENAGEM, COR_MUDO);
        uiCirculo(48, 70, 26, cor);
        uiIcone(icone, 48, 70, 30, COR_BRANCO, cor);
        uiTexto(88, 52, 20, COR_TEXTO, ESQ, 300, "%s", titulo);
        uiTexto(20, 110, 14, COR_TEXTO2, ESQ, 0, "%s", texto);

        uiTela(alvoBaixo);
        uiTexto(160, 60, 14, COR_MUDO, CENTRO, 300, "Toque em OK ou aperte A");
        uiBotao(60, 150, 200, 56, "OK", IC_OK, cor, COR_BRANCO);
        uiFim();
    }
}

// ---------------------------------------------------------------- imagens

// A imagem vai para a textura pela CPU, no formato nativo da GPU do 3DS, sem a
// transferência automática (que deixava dúvida sobre a orientação):
//   - blocos de 8x8 pixels, da esquerda para a direita e de cima para baixo: a
//     linha 0 da memória da textura é o TOPO (v = 1). Confirmado no 3DS: contando
//     de baixo para cima, a imagem saiu exatamente de cabeça para baixo;
//   - dentro do bloco, ordem Z (bits de x e y intercalados, x no bit mais baixo).

static inline u32 ordemZ(u32 x, u32 y) {          // x, y de 0 a 7 dentro do bloco
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
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
    if (!im->ok) return;
    u16 *destino = (u16 *)im->tex.data;
    const u32 blocosPorLinha = UI_IMG_LARG / 8;
    for (int r = 0; r < im->h; r++) {                   // r = linha da imagem, de cima para baixo
        u32 y = r;                                      // y = linha da textura (0 = topo, v = 1)
        u32 inicioLinha = (y >> 3) * blocosPorLinha * 64;
        const u16 *origem = im->pixels + r * UI_IMG_LARG;
        for (int x = 0; x < im->w; x++)
            destino[inicioLinha + (x >> 3) * 64 + ordemZ(x & 7, y & 7)] = origem[x];
    }
    C3D_TexFlush(&im->tex);                             // a GPU lê da memória, não do cache da CPU
}

void uiImagemDesenhar(UiImagem *im, float x, float y) {
    uiImagemDesenharEscala(im, x, y, 1.0f);
}

void uiImagemDesenharEscala(UiImagem *im, float x, float y, float escala) {
    if (im->ok) C2D_DrawImageAt(im->img, x, y, Z, NULL, escala, escala);
}

void uiImagemEnviarParte(UiImagem *im, int x0, int y0, int w, int h) {
    if (!im->ok) return;
    u16 *destino = (u16 *)im->tex.data;
    const u32 blocosPorLinha = UI_IMG_LARG / 8;
    for (int y = y0; y < y0 + h && y < UI_IMG_ALT; y++) {
        u32 inicioLinha = (y >> 3) * blocosPorLinha * 64;
        const u16 *origem = im->pixels + y * UI_IMG_LARG;
        for (int x = x0; x < x0 + w && x < UI_IMG_LARG; x++)
            destino[inicioLinha + (x >> 3) * 64 + ordemZ(x & 7, y & 7)] = origem[x];
    }
    C3D_TexFlush(&im->tex);
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
