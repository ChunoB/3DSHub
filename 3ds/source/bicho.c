// Bichinho — um bichinho em pixel art que mora no 3DS e sente o PC.
//
//   tela de cima : o bichinho no quarto dele, com fome/alegria e o que ele percebe do PC
//   tela de baixo: o rosto dele de perto. Esfregue a caneta = carinho;
//                  arraste uma comida da bandeja até a boca = comer
//   START        : voltar ao menu
//
// O que ele sente do PC (pc/bicho.py, UDP 5014, uma vez por segundo):
//   música tocando = dança · CPU alta = sua · PC parado/madrugada = dorme ·
//   muitas janelas abertas = fica bravo. Sem o PC, ele vive normalmente.
// Fome e alegria ficam salvas em sdmc:/3ds/3dshub_bicho.cfg e baixam com o tempo,
// até com o app fechado.

#include "comum.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PORTA       5014
#define ARQ_BICHO   "sdmc:/3ds/3dshub_bicho.cfg"

// ---------------------------------------------------------------- pixel art
// Corpo 16x14. o = contorno, b = corpo, l = barriga, . = transparente
#define SPR_W 16
#define SPR_H 14
static const char *corpo[SPR_H] = {
    "...oo......oo...",
    "..obbo....obbo..",
    "..obbboooobbbo..",
    ".obbbbbbbbbbbbo.",
    "obbbbbbbbbbbbbbo",
    "obbbbbbbbbbbbbbo",
    "obbbbbbbbbbbbbbo",
    "obbbbllllllbbbbo",
    "obbbllllllllbbbo",
    "obbbllllllllbbbo",
    "obbbbllllllbbbbo",
    ".obbbbbbbbbbbbo.",
    "..oobbbbbbbboo..",
    "....oooooooo....",
};

// comidas 8x8: r = vermelho, g = verde, c = marrom, a = azul, w = branco, p = rosa, y = amarelo
static const char *maca[8] = {
    "....gc..", "...gc...", ".rrrcrr.", "rrwrrrrr",
    "rwrrrrrr", "rrrrrrrr", ".rrrrrr.", "..rr.rr.",
};
static const char *peixe[8] = {
    "........", "..aaaa..", ".aaaaaa.a", "awaaaaaaa",
    "aaaaaaaaa", ".aaaaaa.a", "..aaaa..", "........",
};
static const char *bolo[8] = {
    "...yy...", "...r....", ".pppppp.", "pwpwpwpp",
    "pppppppp", "wwwwwwww", "cccccccc", "cccccccc",
};

static u32 corPixel(char c) {
    switch (c) {
        case 'o': return HEX(0x3B2414);
        case 'b': return HEX(0xE89A4C);
        case 'l': return HEX(0xF8D6A8);
        case 'e': return HEX(0x241811);
        case 'w': return HEX(0xFFFFFF);
        case 'p': return HEX(0xF07C8C);
        case 'r': return HEX(0xD8343C);
        case 'g': return HEX(0x4CAF3C);
        case 'c': return HEX(0x7A4A28);
        case 'a': return HEX(0x3C8CDC);
        case 'y': return HEX(0xF4D03C);
        default:  return 0;
    }
}

static void pixels(const char **linhas, int w, int h, float x, float y, float esc) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w && linhas[j][i]; i++) {
            u32 cor = corPixel(linhas[j][i]);
            if (cor) uiRet(x + i * esc, y + j * esc, esc + 0.5f, esc + 0.5f, cor);
        }
}

static void px(float x, float y, float esc, int i, int j, u32 cor) {
    uiRet(x + i * esc, y + j * esc, esc + 0.5f, esc + 0.5f, cor);
}

// ---------------------------------------------------------------- estado

enum { CARA_NORMAL, CARA_PISCA, CARA_DORME, CARA_FELIZ, CARA_BRAVO, CARA_TRISTE, CARA_COME, CARA_CANTA };

static struct {
    float fome, alegria;                // 0 a 100 (fome 100 = de barriga cheia)
    // o que veio do PC
    int cpu, ram, musica, ocioso, janelas, hora;
    char titulo[96];
    u64 ultimoPC;                       // quando chegou a última resposta (0 = nunca)
    // animações
    u64 comendoAte, felizAte, recusaAte;
    char fala[64];
    u64 falaAte;
    u32 skinPC;                         // crc da skin que o PC tem (0 = nenhuma)
} b;

// ---------------------------------------------------------------- skin (pc/bicho)
// Uma skin troca o desenho por quadros de imagem (ver pc/bicho.py). Fica guardada em
// ARQ_SKIN (u32 crc + pacote) e funciona sem o PC; quando o PC tem outra, baixa de novo.
#define ARQ_SKIN "sdmc:/3ds/3dshub_bicho_skin.bin"
#define MAX_SKIN (128 * 1024)
enum { S_NORMAL, S_DANCA, S_DORME, S_BRAVO, S_CALOR, S_FELIZ, N_SKIN };
typedef struct { int x, y, w, h; } QuadroSkin;
static struct { int n; QuadroSkin q[8]; } skinEst[N_SKIN];
static UiImagem skinAtlas;
static bool skinOk;
static u32 skinCrc;
static int skinMaxW, skinMaxH;
// download em pedaços
static u8 *baixando;
static int baixTam, baixParte, baixTotal;
static u32 baixCrc;
static u64 baixPedido;

static bool montarSkin(const u8 *d, int n) {
    skinOk = false;
    if (n < 5 || memcmp(d, "SKN1", 4)) return false;
    if (!skinAtlas.ok && !uiImagemCriarAlfa(&skinAtlas, UI_IMG_LARG, UI_IMG_ALT)) return false;
    memset(skinAtlas.pixels, 0, UI_IMG_LARG * UI_IMG_ALT * sizeof(u16));
    memset(skinEst, 0, sizeof(skinEst));
    skinMaxW = skinMaxH = 1;
    int p = 5, ax = 0, ay = 0, alturaLinha = 0;
    for (int e = 0; e < d[4] && e < N_SKIN; e++) {
        if (p >= n) return false;
        int nq = d[p++];
        for (int k = 0; k < nq; k++) {
            if (p + 2 > n) return false;
            int w = d[p], h = d[p + 1];
            p += 2;
            if (p + w * h * 2 > n) return false;
            if (ax + w > UI_IMG_LARG) { ax = 0; ay += alturaLinha + 1; alturaLinha = 0; }   // prateleiras
            if (ay + h > UI_IMG_ALT) return false;
            for (int y = 0; y < h; y++) memcpy(skinAtlas.pixels + (ay + y) * UI_IMG_LARG + ax, d + p + y * w * 2, w * 2);
            if (k < 8) {
                skinEst[e].q[k] = (QuadroSkin){ ax, ay, w, h };
                skinEst[e].n = k + 1;
            }
            if (w > skinMaxW) skinMaxW = w;
            if (h > skinMaxH) skinMaxH = h;
            ax += w + 1;
            if (h > alturaLinha) alturaLinha = h;
            p += w * h * 2;
        }
    }
    uiImagemEnviar(&skinAtlas);
    uiImagemFiltro(&skinAtlas, false);          // pixel art: sem borrar
    skinOk = skinEst[S_NORMAL].n > 0;
    return skinOk;
}

static void carregarSkin(void) {
    FILE *f = fopen(ARQ_SKIN, "rb");
    if (!f) return;
    u8 *d = malloc(MAX_SKIN);
    u32 crc = 0;
    int n = d && fread(&crc, 4, 1, f) == 1 ? (int)fread(d, 1, MAX_SKIN, f) : 0;
    fclose(f);
    if (n > 0 && crc32(0, d, n) == crc && montarSkin(d, n)) skinCrc = crc;
    free(d);
}

static void salvarSkin(const u8 *d, int n, u32 crc) {
    FILE *f = fopen(ARQ_SKIN, "wb");
    if (!f) return;
    fwrite(&crc, 4, 1, f);
    fwrite(d, 1, n, f);
    fclose(f);
}

static int estadoSkin(int cara, int h);   // abaixo, depois dos humores

// Desenha o quadro da skin no lugar do desenho: centrado, apoiado embaixo da caixa w x h.
static void desenharSkin(float x, float y, float caixaW, float caixaH, int estado, u64 agora) {
    if (skinEst[estado].n == 0) estado = S_NORMAL;
    int n = skinEst[estado].n;
    const QuadroSkin *q = &skinEst[estado].q[(agora / (estado == S_DORME ? 500 : 160)) % n];
    float esc = floorf(fminf(caixaW / skinMaxW, caixaH / skinMaxH));
    if (esc < 1) esc = 1;
    uiImagemDesenharParte(&skinAtlas, q->x, q->y, q->w, q->h, x + (caixaW - q->w * esc) / 2,
                          y + caixaH - q->h * esc, esc);
}

#define MAX_CORACOES 16
static struct { float x, y, vy, vida; bool cima; } coracoes[MAX_CORACOES];

static void falar(const char *t, int ms) {
    snprintf(b.fala, sizeof(b.fala), "%s", t);
    b.falaAte = osGetTime() + ms;
}

static float limitar(float v) { return v < 0 ? 0 : v > 100 ? 100 : v; }

static void carregar(void) {
    b.fome = 70;
    b.alegria = 70;
    FILE *f = fopen(ARQ_BICHO, "r");
    if (!f) return;
    float fome, alegria;
    long long quando;
    if (fscanf(f, "%f %f %lld", &fome, &alegria, &quando) == 3) {
        // o tempo com o app fechado também conta (sem exagero: no máximo 2 dias)
        double horas = difftime(time(NULL), (time_t)quando) / 3600.0;
        if (horas < 0) horas = 0;
        if (horas > 48) horas = 48;
        b.fome = limitar(fome - horas * 6);
        b.alegria = limitar(alegria - horas * 3);
    }
    fclose(f);
}

static void salvar(void) {
    FILE *f = fopen(ARQ_BICHO, "w");
    if (!f) return;
    fprintf(f, "%.1f %.1f %lld\n", b.fome, b.alegria, (long long)time(NULL));
    fclose(f);
}

// ---------------------------------------------------------------- PC

static bool comPC(void) { return b.ultimoPC && osGetTime() - b.ultimoPC < 5000; }

static void lerPC(char *t) {
    char *salvo;
    for (char *l = strtok_r(t, "\n", &salvo); l; l = strtok_r(NULL, "\n", &salvo)) {
        char *igual = strchr(l, '=');
        if (!igual) continue;
        *igual = 0;
        const char *v = igual + 1;
        if (!strcmp(l, "cpu")) b.cpu = atoi(v);
        else if (!strcmp(l, "ram")) b.ram = atoi(v);
        else if (!strcmp(l, "musica")) b.musica = atoi(v);
        else if (!strcmp(l, "ocioso")) b.ocioso = atoi(v);
        else if (!strcmp(l, "janelas")) b.janelas = atoi(v);
        else if (!strcmp(l, "skin")) b.skinPC = strtoul(v, NULL, 10);
        else if (!strcmp(l, "hora")) b.hora = atoi(v);
        else if (!strcmp(l, "titulo")) snprintf(b.titulo, sizeof(b.titulo), "%s", v);
    }
    b.ultimoPC = osGetTime();
}

// O humor que manda (do mais forte para o mais fraco)
enum { H_NORMAL, H_FOME, H_DANCA, H_CALOR, H_BRAVO, H_DORME };

static bool dormindo(void) {
    if (!comPC()) return false;
    return b.ocioso >= 180 || (b.hora >= 1 && b.hora < 6 && !b.musica);
}

static int humor(void) {
    if (dormindo()) return H_DORME;
    if (comPC() && b.janelas >= 15) return H_BRAVO;
    if (comPC() && b.cpu >= 70) return H_CALOR;
    if (comPC() && b.musica) return H_DANCA;
    if (b.fome < 30) return H_FOME;
    return H_NORMAL;
}

// ---------------------------------------------------------------- desenho do bichinho

// Desenha o bichinho com o canto de cima-esquerdo em (x, y); esc = tamanho do pixel.
static int humorAtual;

static int estadoSkin(int cara, int h) {
    if (cara == CARA_DORME) return S_DORME;
    if (cara == CARA_COME || (cara == CARA_FELIZ && h != H_DANCA)) return S_FELIZ;
    switch (h) {
        case H_DANCA: return S_DANCA;
        case H_BRAVO: return S_BRAVO;
        case H_CALOR: return S_CALOR;
        default:      return S_NORMAL;
    }
}

static void desenharBicho(float x, float y, float esc, int cara, bool suor, u64 agora) {
    if (skinOk) {
        desenharSkin(x, y, SPR_W * esc, SPR_H * esc, estadoSkin(cara, humorAtual), agora);
        return;
    }
    pixels(corpo, SPR_W, SPR_H, x, y, esc);
    u32 olho = corPixel('e'), branco = corPixel('w'), corBoca = corPixel('e');

    // bochechas
    if (cara == CARA_FELIZ || cara == CARA_COME || cara == CARA_CANTA) {
        px(x, y, esc, 2, 7, corPixel('p'));
        px(x, y, esc, 3, 7, corPixel('p'));
        px(x, y, esc, 12, 7, corPixel('p'));
        px(x, y, esc, 13, 7, corPixel('p'));
    }

    // olhos (colunas 4-5 e 10-11, linhas 5-6)
    for (int lado = 0; lado < 2; lado++) {
        int c = lado ? 10 : 4;
        switch (cara) {
            case CARA_PISCA:
            case CARA_DORME:
                px(x, y, esc, c, 6, olho);
                px(x, y, esc, c + 1, 6, olho);
                break;
            case CARA_FELIZ:
            case CARA_CANTA:                 // olhinhos ^ ^
                px(x, y, esc, c - 1 + lado, 6, olho);
                px(x, y, esc, c + lado, 5, olho);
                px(x, y, esc, c + 1 + lado, 6, olho);
                break;
            default:
                px(x, y, esc, c, 5, branco);
                px(x, y, esc, c + 1, 5, olho);
                px(x, y, esc, c, 6, olho);
                px(x, y, esc, c + 1, 6, olho);
                break;
        }
    }
    // sobrancelhas
    if (cara == CARA_BRAVO) {              // \ /
        px(x, y, esc, 3, 3, olho);  px(x, y, esc, 4, 4, olho);  px(x, y, esc, 5, 4, olho);
        px(x, y, esc, 12, 3, olho); px(x, y, esc, 11, 4, olho); px(x, y, esc, 10, 4, olho);
    } else if (cara == CARA_TRISTE) {      // / \.
        px(x, y, esc, 4, 3, olho);  px(x, y, esc, 5, 3, olho); px(x, y, esc, 3, 4, olho);
        px(x, y, esc, 10, 3, olho); px(x, y, esc, 11, 3, olho); px(x, y, esc, 12, 4, olho);
    }

    // boca (colunas 6-9, linhas 8-9)
    switch (cara) {
        case CARA_COME: {                  // mastigando: abre e fecha
            bool aberta = (agora / 150) % 2;
            px(x, y, esc, 7, 8, corBoca); px(x, y, esc, 8, 8, corBoca);
            if (aberta) { px(x, y, esc, 7, 9, corPixel('r')); px(x, y, esc, 8, 9, corPixel('r')); }
            break;
        }
        case CARA_CANTA:
            px(x, y, esc, 7, 8, corBoca); px(x, y, esc, 8, 8, corBoca);
            px(x, y, esc, 7, 9, corPixel('r')); px(x, y, esc, 8, 9, corPixel('r'));
            break;
        case CARA_BRAVO:
            for (int i = 6; i <= 9; i++) px(x, y, esc, i, 9, corBoca);
            break;
        case CARA_TRISTE:
            px(x, y, esc, 7, 8, corBoca); px(x, y, esc, 8, 8, corBoca);
            px(x, y, esc, 6, 9, corBoca); px(x, y, esc, 9, 9, corBoca);
            break;
        case CARA_DORME:
            px(x, y, esc, 7, 9, corBoca); px(x, y, esc, 8, 9, corBoca);
            break;
        default:                           // sorriso
            px(x, y, esc, 6, 8, corBoca); px(x, y, esc, 9, 8, corBoca);
            px(x, y, esc, 7, 9, corBoca); px(x, y, esc, 8, 9, corBoca);
            break;
    }

    // gotas de suor (CPU quente)
    if (suor) {
        float cair = fmodf((agora % 3600000) / 400.0f, 1.0f) * 3 * esc;
        u32 azul = HEX(0x7FC8F8);
        uiRet(x - 1.5f * esc, y + 2 * esc + cair, esc, esc * 1.6f, azul);
        uiRet(x + 16.5f * esc, y + 3 * esc + cair * 0.8f, esc, esc * 1.6f, azul);
    }
}

static void desenharCoracao(float x, float y, float t, u32 cor) {
    uiCirculo(x - t * 0.5f, y, t * 0.6f, cor);
    uiCirculo(x + t * 0.5f, y, t * 0.6f, cor);
    uiTri(x - t * 1.1f, y + t * 0.2f, x + t * 1.1f, y + t * 0.2f, x, y + t * 1.4f, cor);
}

static void desenharNota(float x, float y, float t, u32 cor) {
    uiCirculo(x, y + t, t * 0.55f, cor);
    uiRet(x + t * 0.35f, y - t * 0.6f, t * 0.25f, t * 1.6f, cor);
    uiRet(x + t * 0.35f, y - t * 0.6f, t * 0.8f, t * 0.25f, cor);
}

// ---------------------------------------------------------------- tela de cima

static int caraAtual(int h, u64 agora) {
    if (agora < b.comendoAte) return CARA_COME;
    if (agora < b.recusaAte) return CARA_TRISTE;
    if (agora < b.felizAte) return CARA_FELIZ;
    switch (h) {
        case H_DORME: return CARA_DORME;
        case H_BRAVO: return CARA_BRAVO;
        case H_FOME:  return CARA_TRISTE;
        case H_DANCA: return (agora / 600) % 3 == 0 ? CARA_CANTA : CARA_FELIZ;
        default:      return (agora % 4000) < 140 ? CARA_PISCA : CARA_NORMAL;   // pisca de vez em quando
    }
}

static void barra(float x, float y, const char *nome, float valor, u32 cor) {
    uiTexto(x, y, 11, COR_TEXTO2, ESQ, 0, "%s", nome);
    uiBarra(x + 52, y + 3, 90, 9, valor / 100, cor, COR_CARTAO2);
}

static void desenharCima(int h, u64 agora) {
    uiTela(alvoCima);
    // céu: dia, fim de tarde ou noite (pela hora do PC; sem PC, do 3DS)
    int hora = comPC() ? b.hora : (int)((osGetTime() / 3600000) % 24);
    u32 ceu = (hora >= 7 && hora < 17) ? HEX(0x5DA9E9) : (hora >= 17 && hora < 19) ? HEX(0xE88A5C) : HEX(0x1B2148);
    uiRet(0, UI_BARRA, 400, 240 - UI_BARRA, ceu);
    if (hora < 7 || hora >= 19)                       // estrelinhas
        for (int i = 0; i < 14; i++) uiRet((i * 97) % 400, UI_BARRA + 8 + (i * 53) % 90, 2, 2, HEX(0xF4F5FA));
    uiRet(0, 168, 400, 72, HEX(0x6E4B2E));            // chão de madeira
    for (int x = 0; x < 400; x += 40) uiRet(x, 168, 2, 72, HEX(0x5A3C24));
    uiBarraStatus("Bichinho", IC_BICHO, COR_BICHO_CL);

    // o bichinho: dança pulando, respira devagar dormindo
    float esc = 6, w = SPR_W * esc, hh = SPR_H * esc;
    float x = 200 - w / 2, y = 172 - hh;
    if (h == H_DANCA && agora >= b.comendoAte) {
        x += sinf((agora % 3600000) / 180.0f) * 14;
        y -= fabsf(sinf((agora % 3600000) / 180.0f)) * 16;
    } else if (h == H_DORME) {
        y += sinf((agora % 3600000) / 700.0f) * 2;
    }
    uiRet(x + 10, 170, w - 20, 5, HEX(0x4A321E));    // sombra
    desenharBicho(x, y, esc, caraAtual(h, agora), h == H_CALOR, agora);

    if (h == H_DORME)                                // Zzz
        for (int i = 0; i < 3; i++) {
            float t = fmodf((agora % 3600000) / 900.0f + i / 3.0f, 1.0f);
            uiTexto(x + w + 4 + t * 20, y - t * 40, 12 + t * 8, COR_BRANCO, ESQ, 0, "z");
        }
    if (h == H_DANCA)                                // notas musicais
        for (int i = 0; i < 2; i++) {
            float t = fmodf((agora % 3600000) / 1200.0f + i * 0.5f, 1.0f);
            desenharNota(i ? x + w + 10 : x - 20, y + 10 - t * 50, 7, i ? COR_KARAOKE_CL : COR_AMARELO);
        }
    if (h == H_BRAVO) {                              // símbolo de raiva
        float bx = x + w - 6, by = y - 4;
        uiQuad(bx, by, 14, 3, 0.785f, COR_VERMELHO);
        uiQuad(bx, by, 14, 3, -0.785f, COR_VERMELHO);
    }

    // balão de fala
    if (agora < b.falaAte) {
        float lw = uiTexto(-1000, 0, 12, COR_TEXTO, ESQ, 0, "%s", b.fala) + 16;
        float bx = 200 - lw / 2, by = y - 30;
        if (by < UI_BARRA + 2) by = UI_BARRA + 2;
        uiRetArred(bx, by, lw, 20, 8, COR_BRANCO);
        uiTexto(200, by + 3, 12, HEX(0x241811), CENTRO, 0, "%s", b.fala);
    }

    // fome / alegria e o que ele sente do PC
    uiRetArred(6, 182, 388, 54, 10, COR_ESCURO);
    barra(14, 188, "Barriga", b.fome, b.fome < 30 ? COR_VERMELHO : COR_VERDE);
    barra(14, 204, "Alegria", b.alegria, b.alegria < 30 ? COR_AMARELO : COR_ROSA_CL);
    const char *sente;
    char buf[128];
    if (baixando) {
        snprintf(buf, sizeof(buf), "Baixando a roupa nova do PC... %d%%", baixTotal ? baixParte * 100 / baixTotal : 0);
        sente = buf;
    } else if (!comPC()) sente = "Sem sinal do PC. Ele espera você.";
    else switch (h) {
        case H_DORME: sente = b.ocioso >= 180 ? "O PC está parado... dormindo" : "Madrugada... dormindo"; break;
        case H_BRAVO: snprintf(buf, sizeof(buf), "%d janelas abertas! Que bagunça", b.janelas); sente = buf; break;
        case H_CALOR: snprintf(buf, sizeof(buf), "CPU a %d%%! Que calor", b.cpu); sente = buf; break;
        case H_DANCA: snprintf(buf, sizeof(buf), "Dançando: %s", b.titulo[0] ? b.titulo : "música do PC"); sente = buf; break;
        case H_FOME:  sente = "Com fome... arraste uma comida"; break;
        default:      snprintf(buf, sizeof(buf), "PC tranquilo (CPU %d%%, %d janelas)", b.cpu, b.janelas); sente = buf; break;
    }
    uiTexto(14, 220, 11, COR_TEXTO2, ESQ, 372, "%s", sente);
}

// ---------------------------------------------------------------- tela de baixo

#define ROSTO_ESC 11
#define ROSTO_X   (160 - SPR_W * ROSTO_ESC / 2)
#define ROSTO_Y   -10                   // a parte de baixo do corpo sai da tela
#define BOCA_X    (ROSTO_X + 6 * ROSTO_ESC)
#define BOCA_Y    (ROSTO_Y + 7 * ROSTO_ESC)
#define BANDEJA_Y 192

typedef struct { const char **spr; const char *nome; float fome, alegria; } Comida;
static const Comida comidas[3] = {
    { maca,  "Fruta", 20, 3 },
    { peixe, "Peixe", 32, 5 },
    { bolo,  "Bolo",  12, 15 },
};
static float comidaX(int i) { return 52 + i * 90; }

static void desenharBaixo(int h, u64 agora, int arrastando, touchPosition tp) {
    uiTela(alvoBaixo);
    uiRet(0, 0, 320, 240, HEX(0xF8E8D0));
    int cara = caraAtual(h, agora);
    if (cara == CARA_PISCA && h != H_DORME) cara = CARA_NORMAL;
    desenharBicho(ROSTO_X, ROSTO_Y, ROSTO_ESC, cara, h == H_CALOR, agora);

    for (int i = 0; i < MAX_CORACOES; i++)
        if (coracoes[i].vida > 0)
            desenharCoracao(coracoes[i].x, coracoes[i].y, 7 * coracoes[i].vida + 3,
                            C2D_Color32(0xF0, 0x5C, 0x8C, (u8)(255 * (coracoes[i].vida > 1 ? 1 : coracoes[i].vida))));

    // bandeja de comida
    uiRetArred(10, BANDEJA_Y - 6, 300, 52, 12, HEX(0xC9A27A));
    for (int i = 0; i < 3; i++) {
        if (i == arrastando) continue;
        pixels(comidas[i].spr, 9, 8, comidaX(i), BANDEJA_Y, 4);
        uiTexto(comidaX(i) + 16, BANDEJA_Y + 32, 9, HEX(0x5A3C24), CENTRO, 0, "%s", comidas[i].nome);
    }
    if (arrastando >= 0) pixels(comidas[arrastando].spr, 9, 8, tp.px - 18, tp.py - 16, 4.5f);
    else uiTexto(160, 166, 10, HEX(0x8A6A4A), CENTRO, 0, "Esfregue para fazer carinho - START: sair");
}

// ---------------------------------------------------------------- módulo

void modBicho(void) {
    memset(&b, 0, sizeof(b));
    memset(coracoes, 0, sizeof(coracoes));
    carregar();
    skinOk = false;
    skinCrc = 0;
    carregarSkin();
    baixando = NULL;

    struct sockaddr_in dest;
    int sock = abrirUDP(PORTA, &dest);

    u64 agora = osGetTime(), ultimoPedido = 0, ultimoSalvo = agora, ultimoTempo = agora;
    int arrastando = -1;                 // comida que o dedo está levando (-1 = nenhuma)
    float esfregado = 0;                 // caminho do dedo no rosto (vira carinho)
    touchPosition antes = { 0, 0 };
    int humorAntes = -1;

    falar(b.fome < 30 ? "Tô com fome..." : "Oi! :)", 2500);

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown(), held = hidKeysHeld();
        if (down & KEY_START) break;
        agora = osGetTime();
        float dt = (agora - ultimoTempo) / 1000.0f;
        if (dt > 0.5f) dt = 0.5f;
        ultimoTempo = agora;

        // PC: pergunta uma vez por segundo
        if (sock >= 0) {
            if (agora - ultimoPedido >= 1000) {
                ultimoPedido = agora;
                enviarPara(sock, "3DSB", 4, &dest);
            }
            static char resp[1200];
            int n;
            while ((n = receberDe(sock, resp, sizeof(resp) - 1)) >= 4) {
                resp[n] = 0;
                if (!memcmp(resp, "3DSB", 4)) lerPC(resp + 4);
                else if (!memcmp(resp, "3DSb", 4) && n >= 12 && baixando) {      // pedaço da skin
                    u32 crc;
                    u16 parte, total;
                    memcpy(&crc, resp + 4, 4);
                    memcpy(&parte, resp + 8, 2);
                    memcpy(&total, resp + 10, 2);
                    if (crc != baixCrc || parte != baixParte || baixTam + n - 12 > MAX_SKIN) continue;
                    memcpy(baixando + baixTam, resp + 12, n - 12);
                    baixTam += n - 12;
                    baixTotal = total;
                    baixParte++;
                    baixPedido = 0;                                 // pede o próximo já
                    if (baixParte >= baixTotal) {                   // chegou tudo
                        if (crc32(0, baixando, baixTam) == baixCrc && montarSkin(baixando, baixTam)) {
                            skinCrc = baixCrc;
                            salvarSkin(baixando, baixTam, baixCrc);
                            falar("Roupa nova!", 2500);
                        }
                        free(baixando);
                        baixando = NULL;
                    }
                }
            }
            // o PC tem uma skin diferente da guardada: baixa em pedaços
            if (b.skinPC && b.skinPC != skinCrc && !baixando && comPC()) {
                baixando = malloc(MAX_SKIN);
                baixTam = baixParte = baixTotal = 0;
                baixCrc = b.skinPC;
                baixPedido = 0;
            }
            if (baixando && agora - baixPedido > 300) {
                baixPedido = agora;
                u8 pedido[6];
                u16 parte = baixParte;
                memcpy(pedido, "3DSb", 4);
                memcpy(pedido + 4, &parte, 2);
                enviarPara(sock, pedido, sizeof(pedido), &dest);
            }
        }

        // o tempo passa: fome e alegria baixam devagar; música alegra, bagunça irrita
        int h = humor();
        humorAtual = h;
        b.fome = limitar(b.fome - dt * (100.0f / (2 * 3600)));           // ~2 h de cheio a vazio
        b.alegria = limitar(b.alegria - dt * (100.0f / (3 * 3600)));
        if (h == H_DANCA) b.alegria = limitar(b.alegria + dt * 0.3f);
        if (h == H_BRAVO) b.alegria = limitar(b.alegria - dt * 0.1f);
        if (b.fome < 15) b.alegria = limitar(b.alegria - dt * 0.05f);
        if (h != humorAntes && agora > b.falaAte) {
            static const char *frases[] = { "", "Tô com fome...", "Essa música é boa!", "Ufa, que calor!",
                                            "Fecha essas janelas!", "Zzz..." };
            if (humorAntes >= 0 && frases[h][0]) falar(frases[h], 2500);
            humorAntes = h;
        }

        // toque: comida ou carinho
        touchPosition tp;
        hidTouchRead(&tp);
        if (down & KEY_TOUCH) {
            arrastando = -1;
            for (int i = 0; i < 3; i++)
                if (tp.px >= comidaX(i) - 6 && tp.px < comidaX(i) + 42 && tp.py >= BANDEJA_Y - 6) arrastando = i;
            esfregado = 0;
            antes = tp;
        }
        if (held & KEY_TOUCH) {
            if (arrastando < 0 && tp.py < BANDEJA_Y - 10 && h != H_DORME) {
                float dx = tp.px - antes.px, dy = tp.py - antes.py;
                esfregado += sqrtf(dx * dx + dy * dy);
                if (esfregado > 60) {                   // carinho!
                    esfregado = 0;
                    b.alegria = limitar(b.alegria + 2);
                    b.felizAte = agora + 1200;
                    for (int i = 0; i < MAX_CORACOES; i++)
                        if (coracoes[i].vida <= 0) {
                            coracoes[i].x = tp.px;
                            coracoes[i].y = tp.py;
                            coracoes[i].vida = 1.4f;
                            break;
                        }
                }
            }
            antes = tp;
        } else if (arrastando >= 0) {                    // soltou a comida
            bool naBoca = (skinOk && antes.py < BANDEJA_Y - 20) || (antes.px >= BOCA_X - 40 && antes.px < BOCA_X + 4 * ROSTO_ESC + 40 &&
                          antes.py >= BOCA_Y - 40 && antes.py < BOCA_Y + 3 * ROSTO_ESC + 40);
            if (naBoca) {
                const Comida *c = &comidas[arrastando];
                if (h == H_DORME) {
                    falar("Zzz... (ele está dormindo)", 2000);
                } else if (b.fome >= 95 && c->spr != bolo) {
                    b.recusaAte = agora + 1200;
                    falar("Tô cheio!", 1800);
                } else {
                    b.fome = limitar(b.fome + c->fome);
                    b.alegria = limitar(b.alegria + c->alegria);
                    b.comendoAte = agora + 1500;
                    b.felizAte = agora + 3000;
                    falar(c->spr == bolo ? "Bolo!! <3" : "Nham nham!", 1800);
                }
            }
            arrastando = -1;
        }
        if (held & KEY_TOUCH) antes = tp;

        for (int i = 0; i < MAX_CORACOES; i++)
            if (coracoes[i].vida > 0) {
                coracoes[i].y -= dt * 60;
                coracoes[i].vida -= dt;
            }

        if (agora - ultimoSalvo > 30000) {
            ultimoSalvo = agora;
            salvar();
        }

        uiQuadro();
        desenharCima(h, agora);
        desenharBaixo(h, agora, arrastando, tp);
        uiFim();
    }

    salvar();
    if (sock >= 0) fecharRede(sock);
    free(baixando);
    baixando = NULL;
    uiImagemLiberar(&skinAtlas);
    skinOk = false;
}
