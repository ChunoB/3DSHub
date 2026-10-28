// Controle — o 3DS vira controle/teclado + touchpad do PC, com mira por giroscópio.
//
// Atalhos:
//   START + SELECT : voltar ao menu
//   SELECT + B     : liga/desliga o giroscópio (calibra ao ligar: deixe parado)
//   SELECT + A     : força o modo teclado
//   SELECT + X     : tela de toque MOUSE / CÂMERA (analógico direito)

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

// O servidor lê com '<4sIIhhhhHHBBhhh' (32 bytes).
typedef struct __attribute__((packed)) {
    char magic[4];       // "3DS2"
    u32  seq;
    u32  keys;
    s16  cx, cy;
    s16  sx, sy;
    u16  tx, ty;
    u8   tocando;
    u8   flags;
    s16  guinada, inclinacao, rolagem;   // giroscópio em décimos de grau/s
} Pacote;

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
    int calibrando;
    float guinada, inclinacao;
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
               e->modoCamera ? (temLegenda ? legenda[B_CSTICK] : "Analógico direito") : "Touchpad (mouse)",
               e->tocando);
    #undef ACAO
    if (e->conectado && !temLegenda)
        uiTexto(200, 194, 10, COR_MUDO, CENTRO, 0, "(atualize o Hub do PC para ver o que cada botão faz)");

    // giroscópio
    uiIcone(IC_GIRO, 26, 222, 16, e->giro ? COR_AGUA_CL : COR_MUDO, COR_FUNDO);
    if (e->calibrando)
        uiTexto(40, 214, 13, COR_AMARELO, ESQ, 0, "Giroscópio: calibrando, deixe parado");
    else if (e->giro)
        uiTexto(40, 214, 13, COR_AGUA_CL, ESQ, 0, "Giroscópio ligado  (%.0f°/s, %.0f°/s)", e->guinada, e->inclinacao);
    else
        uiTexto(40, 214, 13, COR_MUDO, ESQ, 0, "Giroscópio desligado");
    uiTexto(384, 214, 13, COR_MUDO, DIR, 0, "SELECT+B");
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

    uiTexto(8, 206, 11, COR_MUDO, ESQ, 0, "SELECT+A teclado   SELECT+X câmera");
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
        } else {
            p.sx = cs.dx;  p.sy = cs.dy;
            p.tocando = tocando ? 1 : 0;
            p.tx = tocando ? tp.px : 0;
            p.ty = tocando ? tp.py : 0;
        }
        sendto(sock, &p, sizeof(p), 0, (struct sockaddr *)&dest, sizeof(dest));

        // resposta do PC: "3DSA" + perfil + "\n" + modo
        // respostas do PC: "3DSA" (perfil e modo) e "3DSM" (o que cada botão faz)
        static char resp[1024];
        int n;
        while ((n = recvfrom(sock, resp, sizeof(resp) - 1, 0, NULL, NULL)) >= 4) {
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
                snprintf(modo, sizeof(modo), "%.15s", quebra + 1);
            }
        }

        e.conectado = ultimoAck && osGetTime() - ultimoAck < 1000;
        e.keys = held;
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
        sendto(sock, &p, sizeof(p), 0, (struct sockaddr *)&dest, sizeof(dest));
    }
    close(sock);
    HIDUSER_DisableGyroscope();
    irrstExit();
}
