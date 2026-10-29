// Steam — a biblioteca da Steam do PC no 3DS, e o jogo na tela de cima.
//
// Biblioteca:
//   cima/baixo (L/R = página) ou toque : escolher (a capa aparece na tela de cima)
//   A ou tocar de novo                 : jogar (o jogo abre no PC)
//   X                                  : atualizar a lista
//   B ou START                         : voltar ao menu
// Jogando:
//   botões, circle pad e C-stick vão para o PC como controle de Xbox (pelo Controle)
//   tela de baixo: LT/RT (o Old 3DS não tem ZL/ZR) ou touchpad (mouse)
//   START + SELECT                     : voltar para a biblioteca (o jogo continua no PC)

#include "comum.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORTA          5012
#define PORTA_CONTROLE 5005
#define MAX_JOGOS      200
#define MAX_JPEG       (256 * 1024)

#define CAPA_W 400                      // capa grande da tela de cima (formato 460x215 da Steam)
#define CAPA_H 187
#define MINI_W 64                       // miniatura da lista
#define MINI_H 30
#define MINI_COLS (UI_IMG_LARG / MINI_W)
#define N_SLOTS   (MINI_COLS * (UI_IMG_ALT / MINI_H))   // miniaturas que cabem no atlas (64)

#define COR_VAZIO 0x2127                // COR_CARTAO em RGB565: capa que não veio

typedef struct {
    char appid[16];
    char nome[164];                     // até 40 letras em UTF-8 (acento ocupa 2 bytes ou mais)
    int minutos;                        // tempo jogado
} Jogo;

static Jogo jogos[MAX_JOGOS];
static int nJogos;
static int donoSlot[N_SLOTS];           // de qual jogo é cada miniatura do atlas (-1 = nenhum)
static UiImagem atlas, capa, quadroG, quadroP;   // quadroG 400x240, quadroP 200x120 (modo rápido)
static int capaDe = -1;                 // de qual jogo é a capa grande carregada
static u8 *jpeg;                        // MAX_JPEG + 1 (o +1 é o 0 do fim dos textos)
// Imagem do jogo (botão na tela de baixo, passa de um para o outro):
//   Nítido = 400x240; Rápido = 200x120 ampliada e suavizada (mais quadros por segundo);
//   Retro  = 200x120 com pixels quadrados e linhas de TV antiga (custa o mesmo que o Rápido)
enum { IMG_NITIDO, IMG_RAPIDO, IMG_RETRO, N_MODOS_IMG };
static const char *nomeModoImg[N_MODOS_IMG] = { "Nítido", "Rápido", "Retro" };
static int modoImg = -1;                // -1 = ainda não escolheu (depende de Old/New 3DS)
static int larguraPedida(void) { return modoImg == IMG_NITIDO ? 400 : 200; }

// ---------------------------------------------------------------- rede

static bool enviarTexto(int sock, const char *fmt, ...) {
    char cmd[64];
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(cmd, sizeof(cmd), fmt, a);
    va_end(a);
    return n > 0 && n < (int)sizeof(cmd) && enviarTudo(sock, cmd, n, 3000);
}

// "XXXX" + u32 tamanho + nExtra bytes + corpo (o corpo fica em 'jpeg')
static bool receberResposta(int sock, const char *magica, u8 *extra, int nExtra, u32 *tam) {
    char cab[8];
    if (!receberTudo(sock, cab, 8, 8000) || memcmp(cab, magica, 4)) return false;
    memcpy(tam, cab + 4, 4);
    if (nExtra && !receberTudo(sock, extra, nExtra, 8000)) return false;
    if (*tam > MAX_JPEG) return false;
    return *tam == 0 || receberTudo(sock, jpeg, *tam, 8000);
}

static bool pedirLista(int sock) {
    u32 tam;
    if (!enviarTexto(sock, "J\n") || !receberResposta(sock, "3DSJ", NULL, 0, &tam)) return false;
    jpeg[tam] = 0;
    nJogos = 0;
    char *salvo;
    for (char *l = strtok_r((char *)jpeg, "\n", &salvo); l && nJogos < MAX_JOGOS; l = strtok_r(NULL, "\n", &salvo)) {
        char *t1 = strchr(l, '\t');
        if (!t1) continue;
        *t1 = 0;
        char *t2 = strchr(t1 + 1, '\t');
        if (t2) *t2 = 0;
        Jogo *j = &jogos[nJogos++];
        snprintf(j->appid, sizeof(j->appid), "%s", l);
        snprintf(j->nome, sizeof(j->nome), "%s", t1 + 1);
        j->minutos = t2 ? atoi(t2 + 1) : 0;
    }
    for (int i = 0; i < N_SLOTS; i++) donoSlot[i] = -1;
    capaDe = -1;
    return true;
}

static void pintar(u16 *destino, int larguraLinha, int w, int h, u16 cor) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) destino[y * larguraLinha + x] = cor;
}

// Capa do jogo i em w x h, decodificada direto em 'destino'.
static bool pedirCapa(int sock, int i, int w, int h, u16 *destino) {
    u32 tam;
    int dw, dh;
    if (!enviarTexto(sock, "C %s %d %d\n", jogos[i].appid, w, h) || !receberResposta(sock, "3DSC", NULL, 0, &tam))
        return false;
    if (!tam || !jpegDecodificar(jpeg, tam, destino, UI_IMG_LARG, w, h, &dw, &dh))
        pintar(destino, UI_IMG_LARG, w, h, COR_VAZIO);
    return true;
}

static int slotX(int s) { return (s % MINI_COLS) * MINI_W; }
static int slotY(int s) { return (s / MINI_COLS) * MINI_H; }

static bool carregarMini(int sock, int i) {
    int s = i % N_SLOTS;
    if (!pedirCapa(sock, i, MINI_W, MINI_H, atlas.pixels + slotY(s) * UI_IMG_LARG + slotX(s))) return false;
    uiImagemEnviarParte(&atlas, slotX(s), slotY(s), MINI_W, MINI_H);
    donoSlot[s] = i;
    return true;
}

static bool carregarCapa(int sock, int i) {
    if (!pedirCapa(sock, i, CAPA_W, CAPA_H, capa.pixels)) return false;
    uiImagemEnviar(&capa);
    capaDe = i;
    return true;
}

static void textoTempo(char *s, size_t n, int minutos) {
    if (minutos <= 0)      snprintf(s, n, "Nunca jogado");
    else if (minutos < 60) snprintf(s, n, "%d min jogados", minutos);
    else                   snprintf(s, n, "%d h %02d min jogados", minutos / 60, minutos % 60);
}

// ---------------------------------------------------------------- biblioteca: desenho

#define LIN_Y0    6
#define LIN_H     36
#define LIN_PASSO 40
#define LINHAS    5                     // jogos visíveis na lista de uma vez
static int inicioLista;

static float linhaY(int i) { return LIN_Y0 + (i - inicioLista) * LIN_PASSO; }

static void desenharCapaGrande(int i) {
    if (i >= 0 && capaDe == i) {
        uiImagemDesenhar(&capa, 0, UI_BARRA);
    } else {
        uiRet(0, UI_BARRA, 400, CAPA_H, COR_CARTAO);
        uiIcone(IC_PLAY, 200, UI_BARRA + CAPA_H / 2, 40, COR_LINHA, COR_CARTAO);
    }
}

static void desenharBiblioteca(int sel) {
    uiTela(alvoCima);
    uiBarraStatus("Steam", IC_PLAY, COR_STEAM_CL);
    if (nJogos == 0) {
        uiRetArred(16, 40, 64, 64, 16, COR_STEAM);
        uiIcone(IC_PLAY, 48, 72, 34, COR_BRANCO, COR_STEAM);
        uiTexto(94, 50, 20, COR_TEXTO, ESQ, 290, "Nenhum jogo");
        uiTexto(94, 78, 13, COR_STEAM_CL, ESQ, 290, "na Steam deste PC");
        uiTexto(16, 120, 13, COR_TEXTO2, ESQ, 0,
                "O 3DS Hub mostra os jogos instalados na Steam.\n"
                "Instale um jogo no PC e aperte X para atualizar.");
    } else {
        desenharCapaGrande(sel);
        char tempo[40];
        textoTempo(tempo, sizeof(tempo), jogos[sel].minutos);
        float wt = uiTexto(392, 216, 12, COR_MUDO, DIR, 0, "%s", tempo);
        uiTexto(8, 214, 15, COR_TEXTO, ESQ, 380 - wt - 16, "%s", jogos[sel].nome);
    }

    uiTela(alvoBaixo);
    for (int i = inicioLista; i < nJogos && i < inicioLista + LINHAS; i++) {
        bool s = i == sel;
        float y = linhaY(i);
        uiRetArred(6, y, 308, LIN_H, 8, s ? COR_STEAM : COR_CARTAO);
        int slot = i % N_SLOTS;
        if (donoSlot[slot] == i) uiImagemDesenharParte(&atlas, slotX(slot), slotY(slot), MINI_W, MINI_H, 10, y + 3, 1);
        else                     uiRet(10, y + 3, MINI_W, MINI_H, COR_CARTAO2);
        uiTexto(82, y + 3, 13, s ? COR_BRANCO : COR_TEXTO, ESQ, 226, "%s", jogos[i].nome);
        char tempo[40];
        textoTempo(tempo, sizeof(tempo), jogos[i].minutos);
        uiTexto(82, y + 19, 10, s ? COR_TEXTO : COR_MUDO, ESQ, 226, "%s", tempo);
    }
    if (nJogos) uiTexto(314, 212, 11, COR_MUDO, DIR, 0, "%d/%d", sel + 1, nJogos);
    uiTexto(6, 212, 11, COR_MUDO, ESQ, 0, "A: jogar   X: atualizar   B: menu");
    uiTexto(6, 226, 10, COR_MUDO, ESQ, 0, "No jogo: START+SELECT volta para cá");
}

static void desenharConectando(void) {
    uiQuadro();
    uiTela(alvoCima);
    uiBarraStatus("Steam", IC_PLAY, COR_STEAM_CL);
    uiTexto(200, 96, 20, COR_TEXTO, CENTRO, 0, "Conectando ao PC...");
    uiTexto(200, 128, 13, COR_MUDO, CENTRO, 0, "%s", modoRemoto ? "pela internet" : ipPC);
    uiTela(alvoBaixo);
    uiTexto(160, 108, 14, COR_MUDO, CENTRO, 0, "B ou START: cancelar");
    uiFim();
}

// ---------------------------------------------------------------- jogando: controle
// Os botões vão como no módulo Controle (mesmo pacote, mesma porta): o perfil do
// PC (Xbox, teclado, Minecraft...) vale aqui também. O laço só roda a cada quadro
// recebido, então a entrada também é lida e enviada durante as esperas de rede
// (aoEsperarRede, a cada ~16 ms).

#define BOT_Y   6
#define BOT_H   34
#define BOT_W   74
#define AREA_Y0 46                      // tela de toque: LT/RT ou touchpad
#define AREA_Y1 200
static float botX(int b) { return 6 + b * (BOT_W + 4); }

enum { Z_NADA, Z_BOTOES, Z_LT, Z_RT, Z_MOUSE };

static struct {
    int sock;
    struct sockaddr_in dest;
    PacoteControle p;
    u32 seq;
    u64 ultimoEnvio;
    bool toqueMouse;                    // tela de baixo = touchpad (senão LT/RT)
    int zona;                           // onde o dedo encostou (Z_*)
    u32 down;                           // botões apertados desde a última volta do laço
    touchPosition toqueDown;
    bool sair;                          // START + SELECT
} ctl;

static void enviarControle(void) {
    u64 agora = osGetTime();
    if (agora - ctl.ultimoEnvio < 15) return;        // ~60 por segundo, como o Controle
    ctl.ultimoEnvio = agora;

    u32 held = hidKeysHeld();
    circlePosition cp, cs;
    touchPosition tp;
    hidCircleRead(&cp);
    hidCstickRead(&cs);
    hidTouchRead(&tp);

    // a zona é a de onde o dedo encostou: arrastar não troca LT por RT sem querer
    bool tocando = held & KEY_TOUCH;
    if (!tocando) ctl.zona = Z_NADA;
    else if (ctl.zona == Z_NADA)
        ctl.zona = tp.py < AREA_Y0 - 4 ? Z_BOTOES : ctl.toqueMouse ? Z_MOUSE : tp.px < 160 ? Z_LT : Z_RT;

    PacoteControle *p = &ctl.p;
    memset(p, 0, sizeof(*p));
    memcpy(p->magic, "3DS2", 4);
    p->seq = ctl.seq++;
    p->keys = held;
    if ((held & KEY_START) && (held & KEY_SELECT)) p->keys &= ~(KEY_START | KEY_SELECT);   // é o atalho de sair
    if (ctl.zona == Z_LT) p->keys |= KEY_ZL;
    if (ctl.zona == Z_RT) p->keys |= KEY_ZR;
    p->cx = cp.dx;  p->cy = cp.dy;
    p->sx = cs.dx;  p->sy = cs.dy;
    if (ctl.zona == Z_MOUSE) {
        p->tocando = 1;
        p->tx = tp.px;
        p->ty = tp.py;
    }
    enviarPara(ctl.sock, p, sizeof(*p), &ctl.dest);

    // respostas do Controle (perfil e legenda): aqui não são usadas
    static char resp[1024];
    while (receberDe(ctl.sock, resp, sizeof(resp)) >= 4) {}
}

static void amostrarEntrada(void) {
    hidScanInput();
    u32 down = hidKeysDown(), held = hidKeysHeld();
    if (down & KEY_TOUCH) hidTouchRead(&ctl.toqueDown);
    ctl.down |= down;
    if ((held & KEY_START) && (held & KEY_SELECT)) ctl.sair = true;
    enviarControle();
}

// ---------------------------------------------------------------- jogando: desenho

static void desenharJogo(int i, bool temImagem, bool imagemRapida, float fps, u64 abrindoMs, bool confirmar) {
    uiTela(alvoCima);
    if (temImagem && imagemRapida) {
        // a imagem pequena dobra de tamanho: suavizada (Rápido) ou com pixels
        // quadrados e as linhas escuras das TVs antigas (Retro). Quem faz é a GPU.
        bool retro = modoImg == IMG_RETRO;
        uiImagemFiltro(&quadroP, !retro);
        uiImagemDesenharEscala(&quadroP, 0, 0, 2.0f);
        if (retro)
            for (int y = 1; y < 240; y += 2) uiRet(0, y, 400, 1, C2D_Color32(0, 0, 0, 0x58));
    } else if (temImagem) {
        uiImagemDesenhar(&quadroG, 0, 0);
    } else {
        uiBarraStatus("Steam", IC_PLAY, COR_STEAM_CL);
        desenharCapaGrande(i);
        uiRetArred(16, 150, 368, 52, 12, COR_ESCURO);
        uiTexto(200, 156, 15, COR_TEXTO, CENTRO, 350, "Abrindo %s no PC... %d s", jogos[i].nome, (int)(abrindoMs / 1000));
        uiTexto(200, 178, 12, COR_MUDO, CENTRO, 350, "O jogo aparece aqui quando a janela dele abrir");
        uiTexto(8, 214, 15, COR_TEXTO, ESQ, 384, "%s", jogos[i].nome);
    }

    uiTela(alvoBaixo);
    const char *rotulos[4] = { "Voltar", nomeModoImg[modoImg], ctl.toqueMouse ? "Mouse" : "LT / RT",
                               confirmar ? "Certeza?" : "Fechar" };
    for (int b = 0; b < 4; b++) {
        u32 fundo = b == 3 && confirmar ? COR_VERMELHO : b == 1 || b == 2 ? COR_CARTAO2 : COR_CARTAO;
        uiBotao(botX(b), BOT_Y, BOT_W, BOT_H, rotulos[b], -1, fundo, COR_TEXTO);
    }

    if (ctl.toqueMouse) {
        // touchpad + faixa de rolagem à direita (igual ao Controle: x >= 280 rola)
        uiRetArred(6, AREA_Y0, 268, AREA_Y1 - AREA_Y0, 12, COR_CARTAO);
        uiTexto(140, 100, 12, COR_MUDO, CENTRO, 0, "Touchpad: arraste para mover");
        uiTexto(140, 116, 12, COR_MUDO, CENTRO, 0, "Toque: clique   Segurar: direito");
        uiRetArred(280, AREA_Y0, 34, AREA_Y1 - AREA_Y0, 12, COR_CARTAO2);
        uiIcone(IC_SETA_CIMA, 297, AREA_Y0 + 20, 14, COR_MUDO, COR_CARTAO2);
        uiIcone(IC_SETA_BAIXO, 297, AREA_Y1 - 20, 14, COR_MUDO, COR_CARTAO2);
        touchPosition tp;
        hidTouchRead(&tp);
        if (ctl.zona == Z_MOUSE) uiCirculo(tp.px, tp.py, 14, C2D_Color32(0x75, 0xB0, 0x22, 0x80));
    } else {
        bool lt = ctl.zona == Z_LT, rt = ctl.zona == Z_RT;
        uiRetArred(6, AREA_Y0, 151, AREA_Y1 - AREA_Y0, 14, lt ? COR_STEAM : COR_CARTAO);
        uiRetArred(163, AREA_Y0, 151, AREA_Y1 - AREA_Y0, 14, rt ? COR_STEAM : COR_CARTAO);
        uiTexto(81, 100, 36, lt ? COR_BRANCO : COR_TEXTO, CENTRO, 0, "LT");
        uiTexto(238, 100, 36, rt ? COR_BRANCO : COR_TEXTO, CENTRO, 0, "RT");
        uiTexto(81, 142, 11, lt ? COR_BRANCO : COR_MUDO, CENTRO, 0, "gatilho esquerdo");
        uiTexto(238, 142, 11, rt ? COR_BRANCO : COR_MUDO, CENTRO, 0, "gatilho direito");
    }

    float wf = temImagem ? uiTexto(314, 206, 11, COR_STEAM_CL, DIR, 0, "%.1f fps", fps) : 0;
    uiTexto(6, 206, 11, COR_TEXTO2, ESQ, 300 - wf, "%s", jogos[i].nome);
    uiTexto(6, 222, 10, COR_MUDO, ESQ, 0, "START+SELECT: biblioteca (o jogo continua no PC)");
}

// ---------------------------------------------------------------- jogando
// Retorna false se a conexão com o PC caiu.

static bool jogar(int sock, int i) {
    u32 tam;
    if (!enviarTexto(sock, "P %s\n", jogos[i].appid) || !receberResposta(sock, "3DSO", NULL, 0, &tam)) return false;
    if (tam) {                          // o PC não conseguiu abrir: o texto é o motivo
        jpeg[tam] = 0;
        uiAviso("Steam", (const char *)jpeg, IC_X, COR_STEAM);
        return true;
    }

    memset(&ctl, 0, sizeof(ctl));
    ctl.sock = abrirUDP(PORTA_CONTROLE, &ctl.dest);
    if (ctl.sock < 0) {
        uiAviso("Steam", "Não consegui abrir o controle.\nTente de novo.", IC_X, COR_STEAM);
        return true;
    }
    irrstInit();                        // ZL/ZR e C-stick do New 3DS (no Old falha sem problema)
    aoEsperarRede = amostrarEntrada;

    bool pedido = false, caiu = false, temImagem = false, imagemRapida = false, fechou = false;
    int pedidoW = 400, pedidoH = 240;   // tamanho do quadro que está a caminho
    u64 inicio = osGetTime(), t0 = osGetTime(), confirmarAte = 0;
    int quadros = 0;
    float fps = 0;

    while (aptMainLoop()) {
        amostrarEntrada();
        u32 down = ctl.down;
        ctl.down = 0;
        touchPosition tp = ctl.toqueDown;
        if (ctl.sair) break;

        bool voltar = false;
        if ((down & KEY_TOUCH) && tp.py < AREA_Y0 - 4)
            for (int b = 0; b < 4; b++) {
                if (!uiDentro(&tp, botX(b), BOT_Y, BOT_W, BOT_H)) continue;
                if (b == 0) voltar = true;
                if (b == 1) modoImg = (modoImg + 1) % N_MODOS_IMG;
                if (b == 2) ctl.toqueMouse = !ctl.toqueMouse;
                if (b == 3) {
                    if (osGetTime() < confirmarAte) {       // segundo toque: fecha o jogo no PC
                        if (!enviarTexto(sock, "Q\n")) caiu = true;
                        confirmarAte = 0;
                    } else {
                        confirmarAte = osGetTime() + 3000;
                    }
                }
            }
        if (voltar || caiu) break;

        if (!pedido) {
            pedidoW = larguraPedida();
            pedidoH = pedidoW * 3 / 5;
            if (!enviarTexto(sock, "F %d %d\n", pedidoW, pedidoH)) { caiu = true; break; }
            pedido = true;
        }
        u8 estado;
        int recW = pedidoW, recH = pedidoH;
        if (!receberResposta(sock, "3DSG", &estado, 1, &tam)) { caiu = true; break; }
        pedido = false;
        if (estado == 2) {
            fechou = true;
            break;
        }

        if (estado == 1 && tam) {
            // pede o próximo ANTES de decodificar este: o PC captura enquanto o 3DS decodifica
            pedidoW = larguraPedida();
            pedidoH = pedidoW * 3 / 5;
            if (!enviarTexto(sock, "F %d %d\n", pedidoW, pedidoH)) { caiu = true; break; }
            pedido = true;
            UiImagem *im = recW == 200 ? &quadroP : &quadroG;
            int w, h;
            if (jpegDecodificar(jpeg, tam, im->pixels, UI_IMG_LARG, recW, recH, &w, &h)) {
                uiImagemEnviar(im);
                temImagem = true;
                imagemRapida = recW == 200;
                quadros++;
            }
        } else {
            // o jogo ainda está abrindo: pergunta de novo daqui a pouco
            u64 ate = osGetTime() + 150;
            while (osGetTime() < ate && aptMainLoop() && !ctl.sair) {
                amostrarEntrada();
                svcSleepThread(16 * 1000000LL);
            }
        }

        if (osGetTime() - t0 >= 1000) {
            fps = quadros * 1000.0f / (osGetTime() - t0);
            quadros = 0;
            t0 = osGetTime();
        }

        uiQuadro();
        desenharJogo(i, temImagem, imagemRapida, fps, osGetTime() - inicio, osGetTime() < confirmarAte);
        uiFim();
    }

    // um quadro ainda a caminho: recebe e descarta, senão ele viraria a resposta do próximo pedido
    if (pedido && !caiu) {
        u8 estado;
        if (!receberResposta(sock, "3DSG", &estado, 1, &tam)) caiu = true;
    }

    // avisa o PC que tudo foi solto
    memset(&ctl.p, 0, sizeof(ctl.p));
    memcpy(ctl.p.magic, "3DS2", 4);
    for (int k = 0; k < 5; k++) {
        ctl.p.seq = ctl.seq++;
        enviarPara(ctl.sock, &ctl.p, sizeof(ctl.p), &ctl.dest);
    }
    aoEsperarRede = NULL;
    fecharRede(ctl.sock);
    irrstExit();

    if (fechou && aptMainLoop()) uiAviso("Steam", "O jogo fechou no PC.", IC_OK, COR_STEAM);
    return !caiu;
}

// ---------------------------------------------------------------- módulo

void modSteam(void) {
    desenharConectando();
    int sock = conectarTCP(PORTA, 8000);
    if (sock == CONEXAO_CANCELADA) return;
    jpeg = malloc(MAX_JPEG + 1);
    if (sock < 0 || !jpeg || !pedirLista(sock)) {
        if (sock >= 0) fecharRede(sock);
        free(jpeg);
        jpeg = NULL;
        uiAviso("Não conectou",
                "Não consegui falar com a Steam do PC.\n\n"
                "O 3DS Hub do PC está rodando e atualizado\n"
                "(versão 1.6.0 ou mais nova)? A Steam precisa\n"
                "estar instalada no PC.", IC_X, COR_STEAM);
        return;
    }
    bool okImagens = uiImagemCriar(&atlas, UI_IMG_LARG, UI_IMG_ALT) && uiImagemCriar(&capa, CAPA_W, CAPA_H) &&
                     uiImagemCriar(&quadroG, 400, 240) && uiImagemCriar(&quadroP, 200, 120);
    if (!okImagens) {
        uiImagemLiberar(&atlas);
        uiImagemLiberar(&capa);
        uiImagemLiberar(&quadroG);
        uiImagemLiberar(&quadroP);
        free(jpeg);
        jpeg = NULL;
        fecharRede(sock);
        uiAviso("Steam", "Sem memória para as imagens.", IC_X, COR_STEAM);
        return;
    }
    if (modoImg < 0) {                  // Old 3DS: imagem menor (mais quadros por segundo)
        bool novo = false;
        APT_CheckNew3DS(&novo);
        modoImg = novo ? IMG_NITIDO : IMG_RETRO;
    }

    hidSetRepeatParameters(20, 5);
    int sel = 0;
    inicioLista = 0;
    u64 tSel = osGetTime();             // quando a seleção mudou (a capa grande espera parar)
    bool caiu = false, saiu = false;

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown(), rep = hidKeysDownRepeat();
        int antes = sel, jogarEste = -1;

        if (down & (KEY_START | KEY_B)) {
            saiu = true;
            break;
        }
        if ((rep & KEY_DUP) && sel > 0) sel--;
        if ((rep & KEY_DDOWN) && sel < nJogos - 1) sel++;
        if (rep & KEY_L) sel = sel - LINHAS < 0 ? 0 : sel - LINHAS;
        if (rep & KEY_R) sel = sel + LINHAS >= nJogos ? (nJogos ? nJogos - 1 : 0) : sel + LINHAS;
        if ((down & KEY_A) && nJogos) jogarEste = sel;
        if (down & KEY_TOUCH) {
            touchPosition tp;
            hidTouchRead(&tp);
            for (int i = inicioLista; i < nJogos && i < inicioLista + LINHAS; i++)
                if (uiDentro(&tp, 6, linhaY(i), 308, LIN_H)) {
                    if (i == sel) jogarEste = i;    // tocar no escolhido = jogar
                    sel = i;
                }
        }
        if (down & KEY_X) {
            if (!pedirLista(sock)) { caiu = true; break; }
            if (sel >= nJogos) sel = 0;
        }
        if (sel < inicioLista) inicioLista = sel;
        if (sel >= inicioLista + LINHAS) inicioLista = sel - LINHAS + 1;
        if (sel != antes) tSel = osGetTime();

        if (jogarEste >= 0) {
            if (!jogar(sock, jogarEste)) { caiu = true; break; }
            if (aptShouldClose()) break;
            continue;
        }

        uiQuadro();
        desenharBiblioteca(sel);
        uiFim();

        // imagens aos poucos, uma por volta: primeiro a capa grande (quando a seleção
        // para de mudar), depois as miniaturas que estão aparecendo na lista
        if (nJogos && capaDe != sel && osGetTime() - tSel > 150) {
            if (!carregarCapa(sock, sel)) { caiu = true; break; }
        } else {
            for (int i = inicioLista; i < nJogos && i < inicioLista + LINHAS; i++)
                if (donoSlot[i % N_SLOTS] != i) {
                    if (!carregarMini(sock, i)) caiu = true;
                    break;
                }
            if (caiu) break;
        }
    }

    fecharRede(sock);
    free(jpeg);
    jpeg = NULL;
    uiImagemLiberar(&atlas);
    uiImagemLiberar(&capa);
    uiImagemLiberar(&quadroG);
    uiImagemLiberar(&quadroP);

    if (caiu && !saiu && aptMainLoop())
        uiAviso("Conexão caiu", "A conexão com o PC caiu.\nAbra a Steam de novo.", IC_X, COR_STEAM);
}
