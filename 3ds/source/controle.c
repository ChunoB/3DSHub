// Controle — o 3DS vira controle/teclado + touchpad do PC, com mira por giroscópio.
//
// Atalhos:
//   START + SELECT : voltar ao menu
//   SELECT + B     : liga/desliga o giroscópio (calibra ao ligar: deixe parado)
//   SELECT + A     : força o modo teclado
//   SELECT + X     : tela de toque MOUSE / CÂMERA (analógico direito)
//   SELECT + Y     : tela de cima: controle desenhado / legenda dos botões

#include "comum.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/socket.h>

#define PORTA          5005
#define CENTRO_X       160
#define CENTRO_Y       120
#define RAIO_CAMERA    100
#define STICK_MAX      140
#define FRAMES_CALIB   40
#define FAIXA_ROLAGEM  280      // o servidor trata x >= 280 como rolagem

#define FLAG_GIRO      (1 << 0)
#define FLAG_TECLADO   (1 << 1)

#define COR_TOQUE      C2D_Color32(0x7F, 0x77, 0xDD, 0x70)   // roxo translúcido

typedef PacoteControle Pacote;           // comum.h (a Steam manda o mesmo pacote)

static s16 limitar(float v, float max) {
    if (v >  max) v =  max;
    if (v < -max) v = -max;
    return (s16)v;
}

// ---------------------------------------------------------------- legenda dos botões
// O PC manda "3DSM" + "BOTAO=o que faz\n" do perfil ativo (quando muda e a cada
// segundo). A tela de cima mostra isso; um Hub antigo não manda, e fica "-".

enum { B_L, B_R, B_ZL, B_ZR, B_A, B_B, B_X, B_Y, B_CIMA, B_BAIXO, B_ESQ, B_DIR,
       B_START, B_SELECT, B_CIRCLE, B_CSTICK, N_BOTOES };
static const char *chavesLegenda[N_BOTOES] = {
    "L", "R", "ZL", "ZR", "A", "B", "X", "Y", "CIMA", "BAIXO", "ESQUERDA", "DIREITA",
    "START", "SELECT", "CIRCLE", "CSTICK",
};
static char legenda[N_BOTOES][64];      // UTF-8: acento ocupa 2 bytes
static u64 ultimaLegenda;               // quando chegou a última (0 = nunca)

static void lerLegenda(char *texto) {
    char *salvo;
    for (char *l = strtok_r(texto, "\n", &salvo); l; l = strtok_r(NULL, "\n", &salvo)) {
        char *igual = strchr(l, '=');
        if (!igual) continue;
        *igual = 0;
        for (int i = 0; i < N_BOTOES; i++)
            if (!strcmp(chavesLegenda[i], l)) snprintf(legenda[i], sizeof(legenda[0]), "%s", igual + 1);
    }
    ultimaLegenda = osGetTime();
}

// ---------------------------------------------------------------- desenho

typedef struct {
    bool conectado, giro, teclado, modoCamera, tocando;
    bool trocarVista;               // SELECT+Y: inverte a vista padrão do modo
    int toqueModo;                  // perfil do PC: TOQUE_MOUSE / TOQUE_GATILHOS / TOQUE_CONTROLE
    int zona;                       // onde o dedo está na tela no modo gatilhos/controle (Z_*)
    int calibrando;
    float guinada, inclinacao, rolagem;
    const char *perfil, *modo;
    u32 keys;
    int cx, cy, sx, sy, tx, ty;
} Estado;

// uma linha da legenda: "chip" com o nome do botão + o que ele faz
static void linhaBotao(float x, float y, const char *botao, const char *acao, bool aceso) {
    uiRetArred(x, y, 46, 16, 8, aceso ? COR_ROXO : COR_CARTAO2);
    uiTexto(x + 23, y + 1.5f, 11, aceso ? COR_BRANCO : COR_MUDO, CENTRO, 0, "%s", botao);
    uiTexto(x + 52, y + 1, 12, aceso ? COR_ROXO_CL : COR_TEXTO, ESQ, 118, "%s", acao[0] ? acao : "-");
}

// ---- tela de toque como botões (o perfil do PC escolhe; 3ª linha do "3DSA")
//   gatilhos (Genshin): metade esquerda = LT, direita = RT
//   controle (It Takes Two): LT RT LB RB + analógico direito
// Os botões saem como ZL/ZR/L/R no pacote: o PC trata igual aos botões de verdade.
enum { TOQUE_MOUSE, TOQUE_GATILHOS, TOQUE_CONTROLE };
enum { Z_NADA, Z_LT, Z_RT, Z_LB, Z_RB, Z_STICK };

// layout do modo controle
#define BT_X1 8      // coluna 1 (LT, LB)
#define BT_X2 68     // coluna 2 (RT, RB)
#define BT_W  56
#define BT_Y1 8      // linha 1 (LT, RT)
#define BT_Y2 104    // linha 2 (LB, RB)
#define BT_H  92
#define ST_X  220    // analógico de toque
#define ST_Y  102
#define ST_R  86

// só no modo controle, fora do modo câmera, com o Hub pedindo
static int modoToque(const Estado *e) {
    if (!e->conectado || e->modoCamera || e->teclado || !strcmp(e->modo, "teclado")) return TOQUE_MOUSE;
    return e->toqueModo;
}
static bool toqueGatilhosAtivo(const Estado *e) { return modoToque(e) != TOQUE_MOUSE; }

static int zonaDoToque(int modo, int x, int y) {
    if (modo == TOQUE_GATILHOS) return x < CENTRO_X ? Z_LT : Z_RT;
    if (modo != TOQUE_CONTROLE) return Z_NADA;
    if (x < BT_X2 + BT_W + 4) {
        bool col1 = x < BT_X2 - 2, lin1 = y < BT_Y2 - 2;
        return lin1 ? (col1 ? Z_LT : Z_RT) : (col1 ? Z_LB : Z_RB);
    }
    return Z_STICK;
}

static u32 bitsDaZona(int z) {
    switch (z) {
        case Z_LT: return KEY_ZL;
        case Z_RT: return KEY_ZR;
        case Z_LB: return KEY_L;
        case Z_RB: return KEY_R;
        default:   return 0;
    }
}

// ---- vista "controle desenhado": onde estão os analógicos, o que está apertado e o giroscópio

// gatilho/ombro: retângulo arredondado com o nome
static void ombro(float x, float y, float w, const char *nome, bool aceso) {
    uiRetArred(x, y, w, 16, 8, aceso ? COR_ROXO : COR_CARTAO2);
    uiTexto(x + w / 2, y + 1.5f, 11, aceso ? COR_BRANCO : COR_MUDO, CENTRO, 0, "%s", nome);
}

// botão redondo (A, B, X, Y)
static void botaoRedondo(float cx, float cy, const char *nome, bool aceso) {
    uiCirculo(cx, cy, 10, aceso ? COR_ROXO : COR_CARTAO2);
    uiTexto(cx, cy - 7, 12, aceso ? COR_BRANCO : COR_TEXTO2, CENTRO, 0, "%s", nome);
}

// analógico: anel + bolinha na posição (vx, vy de -max a max; y para cima)
static void analogico(float cx, float cy, float r, int vx, int vy, int max, const char *nome) {
    float fx = vx / (float)max, fy = vy / (float)max;
    float d = sqrtf(fx * fx + fy * fy);
    if (d > 1) { fx /= d; fy /= d; }               // não sai do anel
    bool mexeu = vx * vx + vy * vy > 15 * 15;
    uiAnel(cx, cy, r, 2, mexeu ? COR_ROXO : COR_LINHA, COR_CARTAO2);
    uiLinha(cx, cy, cx + fx * (r - 6), cy - fy * (r - 6), 1.5f, mexeu ? COR_ROXO_CL : COR_LINHA);
    uiCirculo(cx + fx * (r - 6), cy - fy * (r - 6), r * 0.32f, mexeu ? COR_ROXO_CL : COR_MUDO);
    uiTexto(cx, cy + r + 2, 10, COR_MUDO, CENTRO, 0, "%s", nome);
}

// cruz direcional
static void cruz(float cx, float cy, u32 k) {
    const float t = 11;                               // tamanho de cada braço
    uiRet(cx - t / 2, cy - t / 2, t, t, COR_CARTAO2);
    uiRetArred(cx - t / 2, cy - t * 1.5f, t, t, 2, k & KEY_DUP ? COR_ROXO : COR_CARTAO2);
    uiRetArred(cx - t / 2, cy + t / 2, t, t, 2, k & KEY_DDOWN ? COR_ROXO : COR_CARTAO2);
    uiRetArred(cx - t * 1.5f, cy - t / 2, t, t, 2, k & KEY_DLEFT ? COR_ROXO : COR_CARTAO2);
    uiRetArred(cx + t / 2, cy - t / 2, t, t, 2, k & KEY_DRIGHT ? COR_ROXO : COR_CARTAO2);
}

static void desenharControle(const Estado *e) {
    u32 k = e->keys;
    uiRetArred(16, 62, 368, 144, 12, COR_CARTAO);

    // ombros e gatilhos (ZL/ZR só existem no New 3DS; no perfil com toque = gatilhos,
    // LT/RT vêm da tela de baixo)
    bool tg = toqueGatilhosAtivo(e);
    ombro(24, 68, 64, "L", k & KEY_L);
    if (tg) {
        ombro(92, 68, 40, "LT", k & KEY_ZL);
        ombro(268, 68, 40, "RT", k & KEY_ZR);
    } else {
        ombro(92, 68, 40, "ZL", k & KEY_ZL);
        ombro(268, 68, 40, "ZR", k & KEY_ZR);
    }
    ombro(312, 68, 64, "R", k & KEY_R);

    // esquerda: circle pad e cruz
    analogico(62, 118, 26, e->cx, e->cy, 150, "Circle pad");
    cruz(62, 180, k);

    // direita: C-stick (ou a tela no modo câmera) e A B X Y
    analogico(284, 108, 16, e->sx, e->sy, STICK_MAX,
              e->modoCamera ? "Toque (câmera)" : modoToque(e) == TOQUE_CONTROLE ? "Analóg. dir." : "C-stick");
    botaoRedondo(338, 128, "X", k & KEY_X);
    botaoRedondo(316, 150, "Y", k & KEY_Y);
    botaoRedondo(360, 150, "A", k & KEY_A);
    botaoRedondo(338, 172, "B", k & KEY_B);

    // meio: a mira do giroscópio (o ponto anda para onde o mouse vai)
    const float gx = 200, gy = 122, gw = 52, gh = 30;
    uiRetArred(gx - gw, gy - gh, gw * 2, gh * 2, 8, COR_CARTAO2);
    uiLinha(gx - gw + 6, gy, gx + gw - 6, gy, 1, COR_LINHA);
    uiLinha(gx, gy - gh + 6, gx, gy + gh - 6, 1, COR_LINHA);
    if (e->giro && !e->calibrando) {
        // mesma direção do mouse no PC (guinada invertida; o vertical vem da rolagem)
        float fx = -e->guinada / 150.0f, fy = e->rolagem / 150.0f;
        if (fx > 1) fx = 1;
        if (fx < -1) fx = -1;
        if (fy > 1) fy = 1;
        if (fy < -1) fy = -1;
        uiCirculo(gx + fx * (gw - 8), gy + fy * (gh - 8), 6, COR_AGUA_CL);
    } else {
        uiTexto(gx, gy - 7, 11, COR_MUDO, CENTRO, 0, "%s", e->calibrando ? "calibrando..." : "giro desligado");
    }
    uiTexto(gx, gy + gh + 2, 10, COR_MUDO, CENTRO, 0, "Giroscópio");

    // toque e SELECT / START
    ombro(158, 172, 84, e->modoCamera ? "Toque: câmera" : tg ? "Toque: botões" : "Toque", e->tocando);
    ombro(150, 190, 46, "SELECT", k & KEY_SELECT);
    ombro(204, 190, 46, "START", k & KEY_START);
}

// ---- vista "legenda": o que cada botão faz no perfil ativo

static void desenharLegenda(const Estado *e) {
    // o que cada botão faz (acende quando apertado)
    uiRetArred(16, 62, 368, 144, 12, COR_CARTAO);
    u32 k = e->keys;
    bool temLegenda = e->conectado && ultimaLegenda && osGetTime() - ultimaLegenda < 5000;
    #define ACAO(b) (temLegenda ? legenda[b] : "")
    bool moveuCircle = e->cx * e->cx + e->cy * e->cy > 15 * 15;
    float y0 = 68, passo = 18;          // 7 linhas: 68 a 192, e o aviso cabe embaixo
    linhaBotao(24, y0 + 0 * passo, "L", ACAO(B_L), k & KEY_L);
    linhaBotao(24, y0 + 1 * passo, "Cima", ACAO(B_CIMA), k & KEY_DUP);
    linhaBotao(24, y0 + 2 * passo, "Baixo", ACAO(B_BAIXO), k & KEY_DDOWN);
    linhaBotao(24, y0 + 3 * passo, "Esq.", ACAO(B_ESQ), k & KEY_DLEFT);
    linhaBotao(24, y0 + 4 * passo, "Dir.", ACAO(B_DIR), k & KEY_DRIGHT);
    linhaBotao(24, y0 + 5 * passo, "Circle", ACAO(B_CIRCLE), moveuCircle);
    linhaBotao(24, y0 + 6 * passo, "SELECT", ACAO(B_SELECT), k & KEY_SELECT);

    linhaBotao(204, y0 + 0 * passo, "R", ACAO(B_R), k & KEY_R);
    linhaBotao(204, y0 + 1 * passo, "A", ACAO(B_A), k & KEY_A);
    linhaBotao(204, y0 + 2 * passo, "B", ACAO(B_B), k & KEY_B);
    linhaBotao(204, y0 + 3 * passo, "X", ACAO(B_X), k & KEY_X);
    linhaBotao(204, y0 + 4 * passo, "Y", ACAO(B_Y), k & KEY_Y);
    linhaBotao(204, y0 + 5 * passo, "START", ACAO(B_START), k & KEY_START);
    // a tela de toque: touchpad, ou o analógico direito no modo câmera (SELECT+X)
    linhaBotao(204, y0 + 6 * passo, "Toque",
               e->modoCamera ? (temLegenda ? legenda[B_CSTICK] : "Analógico direito")
               : modoToque(e) == TOQUE_GATILHOS ? "Esq. = LT  Dir. = RT"
               : modoToque(e) == TOQUE_CONTROLE ? "LT RT LB RB + analóg." : "Touchpad (mouse)",
               e->tocando);
    #undef ACAO
    if (e->conectado && !temLegenda)
        uiTexto(200, 194, 10, COR_MUDO, CENTRO, 0, "(atualize o Hub do PC para ver o que cada botão faz)");
}

static void desenharCima(const Estado *e) {
    uiTela(alvoCima);
    uiBarraStatus("Controle", IC_CONTROLE, COR_ROXO_CL);

    // conexão, perfil e modo numa faixa só
    bool modoTeclado = e->teclado || !strcmp(e->modo, "teclado");
    uiRetArred(16, 28, 368, 28, 10, COR_CARTAO);
    uiCirculo(30, 42, 5, e->conectado ? COR_AGUA_CL : COR_AMARELO);
    uiTexto(42, 34, 13, e->conectado ? COR_TEXTO : COR_AMARELO, ESQ, 120, "%s",
            e->conectado ? "Conectado" : "Procurando o PC...");
    if (e->conectado)
        uiTexto(374, 34, 13, COR_ROXO_CL, DIR, 220, "%s  ·  %s%s", e->perfil[0] ? e->perfil : "-",
                modoTeclado ? "Teclado" : "Controle", e->teclado ? " (forçado)" : "");

    // modo controle mostra o controle desenhado; modo teclado, a legenda. SELECT+Y inverte.
    bool vistaLegenda = modoTeclado != e->trocarVista;
    if (vistaLegenda) desenharLegenda(e);
    else desenharControle(e);

    // giroscópio
    uiIcone(IC_GIRO, 26, 222, 16, e->giro ? COR_AGUA_CL : COR_MUDO, COR_FUNDO);
    if (e->calibrando)
        uiTexto(40, 214, 13, COR_AMARELO, ESQ, 0, "Giroscópio: calibrando, deixe parado");
    else if (e->giro)
        uiTexto(40, 214, 13, COR_AGUA_CL, ESQ, 0, "Giroscópio ligado  (%.0f°/s, %.0f°/s)", e->guinada, e->rolagem);
    else
        uiTexto(40, 214, 13, COR_MUDO, ESQ, 0, "Giroscópio desligado  (SELECT+B)");
    uiTexto(384, 214, 13, COR_MUDO, DIR, 0, "SELECT+Y: %s", vistaLegenda ? "controle" : "legenda");
}

static void desenharBaixo(const Estado *e) {
    uiTela(alvoBaixo);

    if (e->modoCamera) {
        // a tela inteira vira o analógico direito: centro = parado, anel = máximo
        uiAnel(CENTRO_X, CENTRO_Y, RAIO_CAMERA, 2, COR_LINHA, COR_FUNDO);
        uiAnel(CENTRO_X, CENTRO_Y, RAIO_CAMERA / 2, 1.5f, COR_CARTAO2, COR_FUNDO);
        uiLinha(CENTRO_X - 10, CENTRO_Y, CENTRO_X + 10, CENTRO_Y, 1.5f, COR_MUDO);
        uiLinha(CENTRO_X, CENTRO_Y - 10, CENTRO_X, CENTRO_Y + 10, 1.5f, COR_MUDO);
        if (e->tocando) {
            uiLinha(CENTRO_X, CENTRO_Y, e->tx, e->ty, 2, COR_ROXO);
            uiCirculo(e->tx, e->ty, 10, COR_ROXO);
        }
        uiTexto(8, 4, 12, COR_ROXO_CL, ESQ, 0, "Câmera (analógico direito)");
        uiTexto(312, 222, 12, COR_MUDO, DIR, 0, "SELECT+X: mouse");
        return;
    }

    if (modoToque(e) == TOQUE_CONTROLE) {
        // perfil com toque = controle (ex.: It Takes Two): LT RT LB RB + analógico direito
        static const struct { int z; float x, y; const char *nome; } bts[] = {
            { Z_LT, BT_X1, BT_Y1, "LT" }, { Z_RT, BT_X2, BT_Y1, "RT" },
            { Z_LB, BT_X1, BT_Y2, "LB" }, { Z_RB, BT_X2, BT_Y2, "RB" },
        };
        for (int i = 0; i < 4; i++) {
            bool aceso = e->zona == bts[i].z;
            uiRetArred(bts[i].x, bts[i].y, BT_W, BT_H, 12, aceso ? COR_ROXO : COR_CARTAO);
            uiTexto(bts[i].x + BT_W / 2, bts[i].y + BT_H / 2 - 14, 24, aceso ? COR_BRANCO : COR_TEXTO,
                    CENTRO, 0, "%s", bts[i].nome);
        }
        bool mexendo = e->zona == Z_STICK;
        uiAnel(ST_X, ST_Y, ST_R, 2, mexendo ? COR_ROXO : COR_LINHA, COR_CARTAO);
        uiAnel(ST_X, ST_Y, ST_R / 2, 1.5f, COR_CARTAO2, COR_CARTAO);
        uiLinha(ST_X - 10, ST_Y, ST_X + 10, ST_Y, 1.5f, COR_MUDO);
        uiLinha(ST_X, ST_Y - 10, ST_X, ST_Y + 10, 1.5f, COR_MUDO);
        if (mexendo) {
            float dx = e->tx - ST_X, dy = e->ty - ST_Y, d = sqrtf(dx * dx + dy * dy);
            if (d > ST_R) { dx = dx * ST_R / d; dy = dy * ST_R / d; }
            uiLinha(ST_X, ST_Y, ST_X + dx, ST_Y + dy, 2, COR_ROXO);
            uiCirculo(ST_X + dx, ST_Y + dy, 12, COR_ROXO);
        } else {
            uiTexto(ST_X, ST_Y + ST_R - 26, 11, COR_MUDO, CENTRO, 0, "analógico direito");
        }
        uiTexto(8, 206, 11, COR_MUDO, ESQ, 0, "Perfil do PC: toque = botões + analógico");
        uiTexto(8, 222, 11, COR_MUDO, ESQ, 0, "SELECT+X: tela toda analógico   START+SELECT: menu");
        return;
    }

    if (modoToque(e) == TOQUE_GATILHOS) {
        // perfil com toque = gatilhos (ex.: Genshin): dois botões grandes, LT e RT
        bool lt = e->zona == Z_LT, rt = e->zona == Z_RT;
        uiRetArred(8, 8, 148, 192, 14, lt ? COR_ROXO : COR_CARTAO);
        uiRetArred(164, 8, 148, 192, 14, rt ? COR_ROXO : COR_CARTAO);
        uiTexto(82, 76, 40, lt ? COR_BRANCO : COR_TEXTO, CENTRO, 0, "LT");
        uiTexto(238, 76, 40, rt ? COR_BRANCO : COR_TEXTO, CENTRO, 0, "RT");
        uiTexto(82, 124, 12, lt ? COR_BRANCO : COR_MUDO, CENTRO, 0, "gatilho esquerdo");
        uiTexto(238, 124, 12, rt ? COR_BRANCO : COR_MUDO, CENTRO, 0, "gatilho direito");
        uiTexto(8, 206, 11, COR_MUDO, ESQ, 0, "Perfil do PC: toque = LT / RT   SELECT+X câmera");
        uiTexto(8, 222, 11, COR_MUDO, ESQ, 0, "START+SELECT: voltar ao menu");
        return;
    }

    // touchpad + faixa de rolagem à direita
    uiRetArred(8, 8, FAIXA_ROLAGEM - 14, 192, 12, COR_CARTAO);
    for (int x = 40; x < FAIXA_ROLAGEM - 10; x += 32)           // pontinhos de fundo
        for (int y = 36; y < 196; y += 32)
            uiCirculo(x, y, 1.2f, COR_LINHA);
    uiTexto(137, 70, 12, COR_MUDO, CENTRO, 0, "Toque: clique");
    uiTexto(137, 86, 12, COR_MUDO, CENTRO, 0, "2 toques ou segurar: direito");
    uiTexto(137, 102, 12, COR_MUDO, CENTRO, 0, "Toque e segure: arrastar");
    // atalhos do giroscópio (o servidor do PC faz: giro_pausar_no_toque e giro_l_r_como_cliques)
    uiTexto(137, 128, 12, COR_ROXO_CL, CENTRO, 250, "Toque na tela: pausa o giroscópio");
    uiTexto(137, 144, 12, COR_ROXO_CL, CENTRO, 250, "Giroscópio ligado: L/R = cliques");
    uiTexto(137, 160, 11, COR_MUDO, CENTRO, 250, "(só no modo controle)");

    uiRetArred(FAIXA_ROLAGEM, 8, 32, 192, 12, COR_CARTAO2);
    uiIcone(IC_SETA_CIMA, FAIXA_ROLAGEM + 16, 30, 16, COR_MUDO, COR_CARTAO2);
    uiIcone(IC_SETA_BAIXO, FAIXA_ROLAGEM + 16, 178, 16, COR_MUDO, COR_CARTAO2);

    if (e->tocando) uiCirculo(e->tx, e->ty, 16, COR_TOQUE);

    uiTexto(8, 206, 11, COR_MUDO, ESQ, 0, "SELECT+A teclado  SELECT+X câmera  SELECT+Y tela");
    uiTexto(8, 222, 11, COR_MUDO, ESQ, 0, "START+SELECT: voltar ao menu");
}

// ---------------------------------------------------------------- módulo

void modControle(void) {
    struct sockaddr_in dest;
    int sock = abrirUDP(PORTA, &dest);
    if (sock < 0) return;

    irrstInit();                 // ZL/ZR e C-stick do New 3DS (no Old falha sem problema)
    HIDUSER_EnableGyroscope();
    // coeficiente = leituras brutas por grau/s (~14,375): graus/s = bruto / coef.
    // (Antes multiplicava, e o giroscópio saía ~200x sensível demais.)
    float coef = 14.375f;
    if (R_FAILED(HIDUSER_GetGyroscopeRawToDpsCoefficient(&coef)) || coef < 1.0f) coef = 14.375f;

    Estado e;
    memset(&e, 0, sizeof(e));
    memset(legenda, 0, sizeof(legenda));   // a legenda vem de novo do PC a cada entrada
    ultimaLegenda = 0;
    char perfil[32] = "", modo[16] = "";
    e.perfil = perfil;
    e.modo = modo;
    float soma[3] = {0}, vies[3] = {0};
    u64 ultimoAck = 0;

    Pacote p;
    memcpy(p.magic, "3DS2", 4);
    u32 seq = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 held = hidKeysHeld(), down = hidKeysDown();

        if ((held & KEY_START) && (held & KEY_SELECT)) break;

        if ((held & KEY_SELECT) && (down & KEY_X)) e.modoCamera = !e.modoCamera;
        if ((held & KEY_SELECT) && (down & KEY_A)) e.teclado = !e.teclado;
        if ((held & KEY_SELECT) && (down & KEY_Y)) e.trocarVista = !e.trocarVista;
        if ((held & KEY_SELECT) && (down & KEY_B)) {
            if (e.giro || e.calibrando) {
                e.giro = false;
                e.calibrando = 0;
            } else {
                e.calibrando = FRAMES_CALIB;
                soma[0] = soma[1] = soma[2] = 0;
            }
        }

        // giroscópio: calibra o "zero" e depois converte para graus/s
        angularRate g;
        hidGyroRead(&g);
        if (e.calibrando) {
            soma[0] += g.x; soma[1] += g.y; soma[2] += g.z;
            if (--e.calibrando == 0) {
                for (int i = 0; i < 3; i++) vies[i] = soma[i] / FRAMES_CALIB;
                e.giro = true;
            }
        }
        float rol  = (g.x - vies[0]) / coef;
        float incl = (g.y - vies[1]) / coef;
        float guin = (g.z - vies[2]) / coef;
        e.guinada = guin;
        e.inclinacao = incl;
        e.rolagem = rol;

        circlePosition cp, cs;
        touchPosition tp;
        hidCircleRead(&cp);
        hidCstickRead(&cs);
        hidTouchRead(&tp);
        bool tocando = (held & KEY_TOUCH) != 0;

        p.seq  = seq++;
        p.keys = held;
        p.cx   = cp.dx;  p.cy = cp.dy;
        p.flags = (e.giro ? FLAG_GIRO : 0) | (e.teclado ? FLAG_TECLADO : 0);
        p.guinada    = e.giro ? limitar(guin * 10, 32000) : 0;
        p.inclinacao = e.giro ? limitar(incl * 10, 32000) : 0;
        p.rolagem    = e.giro ? limitar(rol * 10, 32000) : 0;

        if (e.modoCamera) {
            if (tocando) {
                p.sx = limitar((tp.px - CENTRO_X) * STICK_MAX / (float)RAIO_CAMERA, STICK_MAX);
                p.sy = limitar((CENTRO_Y - tp.py) * STICK_MAX / (float)RAIO_CAMERA, STICK_MAX);
            } else {
                p.sx = p.sy = 0;
            }
            p.tocando = 0;
            p.tx = p.ty = 0;
        } else if (modoToque(&e) != TOQUE_MOUSE) {
            // tela = botões (+ analógico): a zona é a de onde o dedo encostou, para
            // arrastar o analógico até a borda não apertar um botão sem querer
            if (!tocando) e.zona = Z_NADA;
            else if (down & KEY_TOUCH || e.zona == Z_NADA) e.zona = zonaDoToque(modoToque(&e), tp.px, tp.py);
            p.sx = cs.dx;  p.sy = cs.dy;
            if (e.zona == Z_STICK) {
                p.sx = limitar((tp.px - ST_X) * STICK_MAX / (float)ST_R, STICK_MAX);
                p.sy = limitar((ST_Y - tp.py) * STICK_MAX / (float)ST_R, STICK_MAX);
            }
            p.keys |= bitsDaZona(e.zona);
            p.tocando = 0;               // não mexe o mouse nem pausa o giroscópio
            p.tx = p.ty = 0;
        } else {
            e.zona = Z_NADA;
            p.sx = cs.dx;  p.sy = cs.dy;
            p.tocando = tocando ? 1 : 0;
            p.tx = tocando ? tp.px : 0;
            p.ty = tocando ? tp.py : 0;
        }
        enviarPara(sock, &p, sizeof(p), &dest);

        // resposta do PC: "3DSA" + perfil + "\n" + modo
        // respostas do PC: "3DSA" (perfil e modo) e "3DSM" (o que cada botão faz)
        static char resp[1024];
        int n;
        while ((n = receberDe(sock, resp, sizeof(resp) - 1)) >= 4) {
            resp[n] = 0;
            if (!memcmp(resp, "3DSM", 4)) {
                lerLegenda(resp + 4);
                continue;
            }
            if (memcmp(resp, "3DSA", 4)) continue;
            ultimoAck = osGetTime();
            char *quebra = strchr(resp + 4, '\n');
            if (quebra) {
                *quebra = 0;
                snprintf(perfil, sizeof(perfil), "%.31s", resp + 4);
                // 3ª linha (Hub novo): "gatilhos" = tocar na esquerda/direita é LT/RT
                char *quebra2 = strchr(quebra + 1, '\n');
                if (quebra2) *quebra2 = 0;
                e.toqueModo = !quebra2 ? TOQUE_MOUSE
                            : !strcmp(quebra2 + 1, "gatilhos") ? TOQUE_GATILHOS
                            : !strcmp(quebra2 + 1, "controle") ? TOQUE_CONTROLE : TOQUE_MOUSE;
                snprintf(modo, sizeof(modo), "%.15s", quebra + 1);
            }
        }

        e.conectado = ultimoAck && osGetTime() - ultimoAck < 1000;
        e.keys = p.keys;                 // inclui os botões da tela de toque
        e.cx = p.cx; e.cy = p.cy; e.sx = p.sx; e.sy = p.sy;
        e.tocando = tocando;
        e.tx = tp.px; e.ty = tp.py;

        uiQuadro();
        desenharCima(&e);
        desenharBaixo(&e);
        uiFim();
    }

    // avisa o PC que tudo foi solto
    memset(&p, 0, sizeof(p));
    memcpy(p.magic, "3DS2", 4);
    for (int i = 0; i < 5; i++) {
        p.seq = seq++;
        enviarPara(sock, &p, sizeof(p), &dest);
    }
    fecharRede(sock);
    HIDUSER_DisableGyroscope();
    irrstExit();
}
