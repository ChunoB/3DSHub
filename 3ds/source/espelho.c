// Espelho — mostra uma janela do PC na tela de cima.
//
//   cima/baixo + A ou toque : escolher a janela
//   R / L (segurar)         : aproximar / afastar (até 8x)
//   circle pad, cruz ou arrastar o dedo (aba Mover): mover a parte visível
//   aba Clicar (ou X)       : o toque vira mouse no PC (toque, segurar, arrastar)
//   Y                       : voltar para a janela inteira
//   B                       : voltar para a lista
//   X (na lista)            : atualizar a lista
//   START                   : voltar ao menu

#include "comum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <sys/socket.h>

#define PORTA       5008
#define MAX_JANELAS 12
#define MAX_JPEG    (512 * 1024)
#define ZOOM_MAX    8.0f
#define ZOOM_VEL    2.0f     // segurando R, o zoom dobra por segundo
#define PAN_VEL     0.8f     // circle pad no máximo: 80% da tela por segundo

static char janelas[MAX_JANELAS][120];   // UTF-8: acento ocupa 2 bytes
static int nJanelas;

// Zoom e centro da parte visível (0 a 1 na janela do PC).
// pedidoX/Y = centro mandado no último "F", para aplicar o ajuste que o PC
// fizer (quando a parte visível encosta na borda da janela).
static float zoom = 1, centroX = 0.5f, centroY = 0.5f, pedidoX = 0.5f, pedidoY = 0.5f;

static float limitar01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// ---------------------------------------------------------------- rede

static bool pedirLista(int sock) {
    static char buf[MAX_JANELAS * 120 + 16];
    char cab[8];
    u32 tam;
    if (!enviarTudo(sock, "L\n", 2, 3000)) return false;
    if (!receberTudo(sock, cab, 8, 5000) || memcmp(cab, "3DSL", 4)) return false;
    memcpy(&tam, cab + 4, 4);
    if (tam >= sizeof(buf)) return false;
    if (tam && !receberTudo(sock, buf, tam, 5000)) return false;
    buf[tam] = 0;

    nJanelas = 0;
    char *salvo;
    for (char *t = strtok_r(buf, "\n", &salvo); t && nJanelas < MAX_JANELAS; t = strtok_r(NULL, "\n", &salvo))
        snprintf(janelas[nJanelas++], sizeof(janelas[0]), "%s", t);
    return true;
}

static bool pedirQuadro(int sock) {
    char cmd[40];
    pedidoX = centroX;
    pedidoY = centroY;
    int n = snprintf(cmd, sizeof(cmd), "F %d %d %d\n", (int)(zoom * 100 + 0.5f),
                     (int)(centroX * 10000 + 0.5f), (int)(centroY * 10000 + 0.5f));
    return enviarTudo(sock, cmd, n, 3000);
}

// Lê a resposta de um "F": cabeçalho + JPEG. tam = 0 quando a janela sumiu.
// "3DSZ" traz também o centro que o PC realmente usou.
static bool receberQuadro(int sock, u8 *jpeg, u32 *tam) {
    char cab[8];
    if (!receberTudo(sock, cab, 8, 5000)) return false;
    bool comZoom = !memcmp(cab, "3DSZ", 4);
    if (!comZoom && memcmp(cab, "3DSF", 4)) return false;
    memcpy(tam, cab + 4, 4);
    if (comZoom) {
        u16 c[2];
        if (!receberTudo(sock, c, 4, 5000)) return false;
        // soma só a correção do PC, sem desfazer o que você moveu desde o pedido
        centroX = limitar01(centroX + (c[0] / 10000.0f - pedidoX));
        centroY = limitar01(centroY + (c[1] / 10000.0f - pedidoY));
    }
    if (*tam > MAX_JPEG) return false;
    return *tam == 0 || receberTudo(sock, jpeg, *tam, 5000);
}

// ---------------------------------------------------------------- zoom

// área da tela de baixo onde arrastar o dedo move a imagem (modo Mover)
// ou onde fica a imagem pequena para tocar (modo Clicar)
#define ARRASTO_Y0 46
#define ARRASTO_Y1 182

// Lê R/L, circle pad, cruz, toque e Y. arrastar = o dedo move a imagem (modo Mover).
static void controlarZoom(u32 down, u32 held, float dt, bool arrastar) {
    static bool tocandoAntes = false;
    static touchPosition toqueAntes;

    if (held & KEY_R) zoom *= powf(ZOOM_VEL, dt);
    if (held & KEY_L) zoom /= powf(ZOOM_VEL, dt);
    if ((down | held) & KEY_Y) {       // segurado também, para não se perder entre quadros
        zoom = 1;
        centroX = centroY = 0.5f;
    }
    if (zoom < 1) zoom = 1;
    if (zoom > ZOOM_MAX) zoom = ZOOM_MAX;

    // circle pad (com zona morta) + cruz; com mais zoom, anda menos na janela
    circlePosition cp;
    hidCircleRead(&cp);
    float vx = abs(cp.dx) > 15 ? cp.dx / 150.0f : 0;
    float vy = abs(cp.dy) > 15 ? -cp.dy / 150.0f : 0;     // circle pad para cima = subir
    if (held & KEY_DLEFT)  vx = -1;
    if (held & KEY_DRIGHT) vx = 1;
    if (held & KEY_DUP)    vy = -1;
    if (held & KEY_DDOWN)  vy = 1;
    centroX += vx * PAN_VEL * dt / zoom;
    centroY += vy * PAN_VEL * dt / zoom;

    // arrastar o dedo na área de arrasto puxa a imagem junto
    bool tocando = held & KEY_TOUCH;
    touchPosition tp;
    hidTouchRead(&tp);
    if (arrastar && tocando && tocandoAntes && toqueAntes.py >= ARRASTO_Y0 && toqueAntes.py < ARRASTO_Y1) {
        centroX -= (tp.px - toqueAntes.px) / 400.0f / zoom;
        centroY -= (tp.py - toqueAntes.py) / 240.0f / zoom;
    }
    tocandoAntes = tocando;
    toqueAntes = tp;

    centroX = limitar01(centroX);
    centroY = limitar01(centroY);
}

// ---------------------------------------------------------------- modo Clicar
// A imagem aparece pequena na tela de baixo e o toque vira mouse no PC:
//   toque = clique esquerdo, segurar = clique direito, arrastar = rodinha.
// O laço do espelho só roda a cada quadro recebido (poucos por segundo), então a
// entrada também é lida durante as esperas de rede (aoEsperarRede), a cada ~16 ms:
// assim um toque rápido entre dois quadros não se perde.

#define MINI_ESC ((ARRASTO_Y1 - ARRASTO_Y0) / 240.0f)
#define MINI_W   (400 * MINI_ESC)
#define MINI_H   (ARRASTO_Y1 - ARRASTO_Y0)
#define MINI_X   (160 - MINI_W / 2)
#define MINI_Y   ARRASTO_Y0
#define SEGURAR_MS  600     // segurar parado = clique direito
#define MOVER_PX    8       // mais que isso = arrastar (rodinha), não toque
#define ROLAR_PX    12      // pixels de arrasto por "dente" da rodinha

static bool modoClique, mostrandoJanela;
static u32 downGuardado;            // botões apertados entre uma leitura e outra do laço
static touchPosition toqueDown;     // onde o dedo encostou da última vez

typedef struct { char tipo; int u, v, n; } Gesto;   // 'T' clique, 'R' rodinha
static Gesto fila[8];
static int nFila;

static struct {
    bool ativo, moveu, direito;
    u64 t0;
    int x0, y0, ultimoY;
    int acumulado;
} g;

static bool naMini(int x, int y) {
    return x >= MINI_X && x < MINI_X + MINI_W && y >= MINI_Y && y < MINI_Y + MINI_H;
}

static void enfileirar(char tipo, int n) {
    int u = (g.x0 - MINI_X) / MINI_W * 10000, v = (g.y0 - MINI_Y) / MINI_H * 10000;
    u = u < 0 ? 0 : u > 10000 ? 10000 : u;
    v = v < 0 ? 0 : v > 10000 ? 10000 : v;
    // rodinha seguida no mesmo ponto: junta num pedido só
    if (tipo == 'R' && nFila && fila[nFila - 1].tipo == 'R' && fila[nFila - 1].u == u && fila[nFila - 1].v == v) {
        fila[nFila - 1].n += n;
        return;
    }
    if (nFila < 8) fila[nFila++] = (Gesto){ tipo, u, v, n };
}

static void amostrarEntrada(void) {
    hidScanInput();
    u32 down = hidKeysDown(), held = hidKeysHeld();
    touchPosition tp;
    hidTouchRead(&tp);
    if (down & KEY_TOUCH) toqueDown = tp;
    downGuardado |= down;

    if (!modoClique || !mostrandoJanela) {
        g.ativo = false;
        return;
    }
    u64 agora = osGetTime();
    if ((down & KEY_TOUCH) && naMini(tp.px, tp.py)) {          // encostou na imagem
        memset(&g, 0, sizeof(g));
        g.ativo = true;
        g.t0 = agora;
        g.x0 = tp.px;
        g.y0 = tp.py;
        g.ultimoY = tp.py;
        return;
    }
    if (!g.ativo) return;

    if (held & KEY_TOUCH) {
        int dx = tp.px - g.x0, dy = tp.py - g.y0;
        if (!g.moveu && !g.direito && dx * dx + dy * dy > MOVER_PX * MOVER_PX) g.moveu = true;
        if (g.moveu) {
            // dedo para cima = conteúdo sobe = rodinha para baixo (como no celular)
            g.acumulado += tp.py - g.ultimoY;
            while (g.acumulado <= -ROLAR_PX) { enfileirar('R', -1); g.acumulado += ROLAR_PX; }
            while (g.acumulado >= ROLAR_PX)  { enfileirar('R', 1);  g.acumulado -= ROLAR_PX; }
        }
        g.ultimoY = tp.py;
        if (!g.moveu && !g.direito && agora - g.t0 >= SEGURAR_MS) {
            enfileirar('T', 2);
            g.direito = true;
        }
    } else {                                                    // soltou
        if (!g.moveu && !g.direito && agora - g.t0 < SEGURAR_MS) enfileirar('T', 1);
        g.ativo = false;
    }
}

// ---------------------------------------------------------------- desenho

#define LINHA_H   28
#define LINHAS    7          // janelas visíveis na lista de uma vez
static int inicioLista;      // primeira janela visível (rolagem)

static float linhaY(int i) { return 8 + (i - inicioLista) * (LINHA_H + 2); }

static void desenharLista(int sel, const char *aviso) {
    uiTela(alvoCima);
    uiBarraStatus("Espelho", IC_ESPELHO, COR_AZUL_CL);
    uiRetArred(16, 36, 64, 64, 16, COR_AZUL);
    uiIcone(IC_ESPELHO, 48, 68, 38, COR_BRANCO, COR_AZUL);
    uiTexto(94, 46, 20, COR_TEXTO, ESQ, 290, "Escolha uma janela");
    uiTexto(94, 74, 13, COR_AZUL_CL, ESQ, 290, "Ela aparece aqui, ao vivo");
    uiTexto(16, 116, 13, COR_TEXTO2, ESQ, 0,
            "Depois de escolher: R/L para zoom e circle pad\n"
            "para mover. Na aba Clicar, toque na imagem da\n"
            "tela de baixo para clicar na janela do PC.");
    if (aviso) {
        uiRetArred(16, 186, 368, 26, 10, HEX(0x633806));
        uiTexto(28, 191, 13, COR_AMARELO, ESQ, 344, "%s", aviso);
    }
    uiTexto(16, 218, 12, COR_MUDO, ESQ, 0, "PC %s", ipPC);

    uiTela(alvoBaixo);
    if (nJanelas == 0) uiTexto(160, 100, 14, COR_MUDO, CENTRO, 0, "Nenhuma janela aberta no PC");
    for (int i = inicioLista; i < nJanelas && i < inicioLista + LINHAS; i++) {
        bool s = i == sel;
        uiRetArred(10, linhaY(i), 300, LINHA_H, 8, s ? COR_AZUL : COR_CARTAO);
        uiTexto(20, linhaY(i) + 6, 13, s ? COR_BRANCO : COR_TEXTO2, ESQ, 280, "%s", janelas[i]);
    }
    uiTexto(160, 222, 11, COR_MUDO, CENTRO, 0, "A ou toque: mostrar   X: atualizar   START: sair");
}

// abas e botões da tela de baixo enquanto mostra a janela
#define ABA_Y 8
#define ABA_H 32
typedef struct { float x, w; const char *rotulo; int icone; } BotaoEsp;
static const BotaoEsp botoesEsp[] = {
    { 10, 80, "Lista", IC_LISTA }, { 96, 100, "Inteira", IC_TELA_CHEIA },
    { 202, 51, "-", -1 }, { 259, 51, "+", -1 },          // zoom
};
#define BTN_Y 190
#define BTN_H 40

static void desenharEspelho(UiImagem *im, bool temImagem, const char *janela, float fps) {
    uiTela(alvoCima);
    if (temImagem) uiImagemDesenhar(im, 0, 0);
    else           uiTexto(200, 110, 14, COR_MUDO, CENTRO, 0, "Esperando a imagem do PC...");
    uiRetArred(8, 8, 76, 22, 11, COR_ESCURO);
    uiTexto(46, 11, 13, COR_TEXTO, CENTRO, 0, "%.1f fps", fps);
    if (zoom > 1.01f) {
        uiRetArred(330, 8, 62, 22, 11, COR_ESCURO);
        uiIcone(IC_LUPA, 344, 19, 12, COR_AZUL_CL, COR_ESCURO);
        uiTexto(372, 11, 13, COR_TEXTO, CENTRO, 0, "%.1fx", zoom);
    }
    uiRetArred(8, 210, 384, 22, 11, COR_ESCURO);                // nome da janela
    uiTexto(18, 213, 13, COR_TEXTO2, ESQ, 364, "%s", janela);

    uiTela(alvoBaixo);
    // abas: Mover (o dedo move a imagem) / Clicar (o dedo vira mouse no PC)
    uiRetArred(10, ABA_Y, 145, ABA_H, 9, !modoClique ? COR_AZUL : COR_CARTAO);
    uiTexto(82, ABA_Y + 8, 14, !modoClique ? COR_BRANCO : COR_MUDO, CENTRO, 0, "Mover");
    uiRetArred(165, ABA_Y, 145, ABA_H, 9, modoClique ? COR_AZUL : COR_CARTAO);
    uiTexto(237, ABA_Y + 8, 14, modoClique ? COR_BRANCO : COR_MUDO, CENTRO, 0, "Clicar");

    uiRetArred(10, ARRASTO_Y0, 300, ARRASTO_Y1 - ARRASTO_Y0, 10, COR_CARTAO);
    if (modoClique) {
        if (temImagem) uiImagemDesenharEscala(im, MINI_X, MINI_Y, MINI_ESC);
        if (g.ativo) uiCirculo(g.x0, g.y0, 7, C2D_Color32(0x85, 0xB7, 0xEB, 0xA0));
        uiTexto(160, 184, 10, COR_MUDO, CENTRO, 0, "Toque: clique   Segurar: direito   Arrastar: rolar");
    } else {
        // minimapa da parte visível
        float mw = 150, mh = 90, mx = 160 - mw / 2, my = 60;
        uiRet(mx, my, mw, mh, COR_CARTAO2);
        float vw = mw / zoom, vh = mh / zoom;
        float vx = mx + centroX * mw - vw / 2, vy = my + centroY * mh - vh / 2;
        if (vx < mx) vx = mx;
        if (vy < my) vy = my;
        if (vx + vw > mx + mw) vx = mx + mw - vw;
        if (vy + vh > my + mh) vy = my + mh - vh;
        uiRet(vx, vy, vw, vh, COR_AZUL);
        uiTexto(160, 160, 11, COR_MUDO, CENTRO, 0, "Arraste aqui para mover   R/L: zoom");
    }

    for (int i = 0; i < 4; i++) {
        const BotaoEsp *b = &botoesEsp[i];
        uiBotao(b->x, BTN_Y, b->w, BTN_H, b->rotulo, b->icone, COR_CARTAO, COR_TEXTO);
    }
}

static void desenharConectando(void) {
    uiQuadro();
    uiTela(alvoCima);
    uiBarraStatus("Espelho", IC_ESPELHO, COR_AZUL_CL);
    uiTexto(200, 96, 20, COR_TEXTO, CENTRO, 0, "Conectando ao PC...");
    uiTexto(200, 128, 13, COR_MUDO, CENTRO, 0, "%s", ipPC);
    uiTela(alvoBaixo);
    uiTexto(160, 108, 14, COR_MUDO, CENTRO, 0, "B ou START: cancelar");
    uiFim();
}

// ---------------------------------------------------------------- módulo

void modEspelho(void) {
    desenharConectando();
    int sock = conectarTCP(PORTA, 8000);
    if (sock == CONEXAO_CANCELADA) return;
    if (sock < 0 || !pedirLista(sock)) {
        if (sock >= 0) fecharRede(sock);
        uiAviso("Não conectou",
                "Não consegui falar com o PC.\n\n"
                "O 3DS Hub está rodando no PC (ícone perto do\n"
                "relógio)? Em Ajustes dá para procurar o PC\n"
                "na rede ou digitar o IP.", IC_X, COR_AZUL);
        return;
    }

    u8 *jpeg = malloc(MAX_JPEG);
    static UiImagem im;
    if (!jpeg || !uiImagemCriar(&im, 400, 240)) {
        free(jpeg);
        fecharRede(sock);
        uiAviso("Espelho", "Sem memória para a imagem.", IC_X, COR_AZUL);
        return;
    }

    int sel = 0;
    inicioLista = 0;
    bool mostrando = false, pedido = false, temImagem = false;   // pedido = um "F" já foi enviado
    const char *aviso = NULL;
    u64 t0 = osGetTime(), tAntes = osGetTime();
    int quadros = 0;
    float fps = 0;
    bool saiu = false, caiu = false;

    modoClique = mostrandoJanela = false;
    downGuardado = 0;
    nFila = 0;
    memset(&g, 0, sizeof(g));
    aoEsperarRede = amostrarEntrada;     // lê a entrada também durante as esperas de rede

    while (aptMainLoop()) {
        amostrarEntrada();
        u32 down = downGuardado, held = hidKeysHeld();
        downGuardado = 0;
        touchPosition tp = toqueDown;    // onde o dedo encostou (vale com KEY_TOUCH em down)
        // mostrando a janela cada quadro demora; segurar o botão também conta
        if (mostrando) down |= held & (KEY_START | KEY_B);
        if (down & KEY_START) {
            saiu = true;
            break;
        }

        if (!mostrando) {
            int abrir = -1;
            if ((down & KEY_DUP) && sel > 0) sel--;
            if ((down & KEY_DDOWN) && sel < nJanelas - 1) sel++;
            if (down & KEY_A) abrir = sel;
            if (down & KEY_TOUCH)
                for (int i = inicioLista; i < nJanelas && i < inicioLista + LINHAS; i++)
                    if (uiDentro(&tp, 10, linhaY(i), 300, LINHA_H)) sel = abrir = i;
            if (down & KEY_X) {
                if (!pedirLista(sock)) { caiu = true; break; }
                if (sel >= nJanelas) sel = 0;
                aviso = NULL;
            }
            // rolagem: mantém a janela escolhida visível
            if (sel < inicioLista) inicioLista = sel;
            if (sel >= inicioLista + LINHAS) inicioLista = sel - LINHAS + 1;

            if (abrir >= 0 && nJanelas > 0) {
                char cmd[16];
                int n = snprintf(cmd, sizeof(cmd), "S %d\n", abrir);
                if (!enviarTudo(sock, cmd, n, 3000)) { caiu = true; break; }
                mostrando = mostrandoJanela = true;
                temImagem = false;
                zoom = 1;                      // janela nova começa inteira
                centroX = centroY = 0.5f;
                tAntes = osGetTime();
                aviso = NULL;
                nFila = 0;
            }
            uiQuadro();
            desenharLista(sel, aviso);
            uiFim();
            continue;
        }

        // abas e botões da tela de baixo
        bool voltar = down & KEY_B;
        if (down & KEY_X) modoClique = !modoClique;
        if (down & KEY_TOUCH) {
            if (uiDentro(&tp, 10, ABA_Y, 145, ABA_H))  modoClique = false;
            if (uiDentro(&tp, 165, ABA_Y, 145, ABA_H)) modoClique = true;
            for (int i = 0; i < 4; i++)
                if (uiDentro(&tp, botoesEsp[i].x, BTN_Y, botoesEsp[i].w, BTN_H)) {
                    if (i == 0) voltar = true;
                    if (i == 1) { zoom = 1; centroX = centroY = 0.5f; }
                    if (i == 2) zoom = zoom / 1.5f < 1 ? 1 : zoom / 1.5f;
                    if (i == 3) zoom = zoom * 1.5f > ZOOM_MAX ? ZOOM_MAX : zoom * 1.5f;
                }
        }

        u32 tam;
        if (voltar) {
            // ainda tem um quadro a caminho: recebe e descarta antes de pedir a lista
            if (pedido && !receberQuadro(sock, jpeg, &tam)) { caiu = true; break; }
            pedido = false;
            mostrando = mostrandoJanela = false;
            nFila = 0;
            if (!pedirLista(sock)) { caiu = true; break; }
            if (sel >= nJanelas) sel = 0;
            continue;
        }

        // zoom e movimento: calculados pelo tempo que passou, então ficam
        // suaves mesmo com poucos quadros por segundo
        u64 agora = osGetTime();
        float dt = (agora - tAntes) / 1000.0f;
        if (dt > 0.25f) dt = 0.25f;
        tAntes = agora;
        controlarZoom(down, held, dt, !modoClique);

        // toques do modo Clicar: manda para o PC (copia antes, porque enviar
        // também lê a entrada e pode acrescentar gestos na fila)
        if (nFila) {
            Gesto envio[8];
            int n = nFila;
            memcpy(envio, fila, sizeof(Gesto) * n);
            nFila = 0;
            for (int i = 0; i < n && !caiu; i++) {
                char cmd[48];
                int k = snprintf(cmd, sizeof(cmd), "%c %d %d %d\n", envio[i].tipo, envio[i].u, envio[i].v, envio[i].n);
                if (!enviarTudo(sock, cmd, k, 3000)) caiu = true;
            }
            if (caiu) break;
        }

        // Pede o próximo quadro ANTES de desenhar o atual: o PC captura e
        // manda enquanto o 3DS decodifica, em vez de um esperar o outro.
        if (!pedido) {
            if (!pedirQuadro(sock)) { caiu = true; break; }
            pedido = true;
        }
        if (!receberQuadro(sock, jpeg, &tam)) { caiu = true; break; }
        pedido = false;

        if (tam == 0) {
            mostrando = mostrandoJanela = false;
            if (!pedirLista(sock)) { caiu = true; break; }
            sel = 0;
            aviso = "A janela fechou ou não pode ser capturada.";
            continue;
        }

        if (!pedirQuadro(sock)) { caiu = true; break; }
        pedido = true;

        int w, h;
        if (jpegDecodificar(jpeg, tam, im.pixels, UI_IMG_LARG, 400, 240, &w, &h)) {
            uiImagemEnviar(&im);
            temImagem = true;
            quadros++;
        }

        if (osGetTime() - t0 >= 1000) {
            fps = quadros * 1000.0f / (osGetTime() - t0);
            quadros = 0;
            t0 = osGetTime();
        }

        uiQuadro();
        desenharEspelho(&im, temImagem, janelas[sel], fps);
        uiFim();
    }

    aoEsperarRede = NULL;
    free(jpeg);
    fecharRede(sock);
    uiImagemLiberar(&im);

    if (caiu && !saiu && aptMainLoop())
        uiAviso("Conexão caiu", "A conexão com o PC caiu.\nAbra o Espelho de novo.", IC_X, COR_AZUL);
}
