// Câmera — manda a imagem da câmera do 3DS para o PC (OBS), inclusive em 3D.
//
//   Y     : troca a câmera: traseira / frontal / 3D (as duas traseiras)
//   A     : pausa/retoma o envio
//   X     : LED da câmera traseira apagado (padrão) / aceso
//   B     : virar a imagem (normal / espelhada / invertida / 180°), salvo por câmera
//   START : voltar ao menu
//   (os botões também estão na tela de baixo, por toque)
//
// A tela de cima mostra a prévia (com o slider 3D ligado, em 3D de verdade).

#include "comum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#define PORTA       5007
#define LARG        400
#define ALT         240
#define TAM_IMAGEM  (LARG * ALT * 2)
#define QUALIDADE   70
#define SLIDER_3D   (*(volatile float *)0x1FF81080)

enum { TRASEIRA, FRONTAL, CAM_3D, N_CAMERAS };
static const char *nomesCamera[] = { "Traseira", "Frontal", "3D" };

typedef struct __attribute__((packed)) {
    char magic[4];   // "3DSF"
    u8   olho;       // 0 esquerdo, 1 direito
    u8   total;      // 1 ou 2 olhos neste quadro
    u16  reservado;
    u32  tamanho;
} Cabecalho;

static u32 selecao(int cam) {
    return cam == FRONTAL ? SELECT_IN1 : cam == CAM_3D ? SELECT_OUT1_OUT2 : SELECT_OUT1;
}
static u32 porta(int cam) { return cam == CAM_3D ? PORT_BOTH : PORT_CAM1; }

static u32 bytesTransferencia;

// LED da câmera traseira: registrador 0x2B do microcontrolador (0 = apagado).
// O serviço de câmera acende ele ao ligar/capturar com a câmera traseira,
// então apagamos logo depois de cada vez (rápido demais para ver piscar).
#define REG_LED_CAMERA 0x2B
static bool mcuOk, ledApagado = true;

static void apagarLed(void) {
    if (!mcuOk || !ledApagado) return;
    u8 desligado = 0;
    MCUHWC_WriteRegister(REG_LED_CAMERA, &desligado, 1);
}

static Result erroConfig;   // primeiro erro ao configurar a câmera (0 = tudo certo)

static void anotar(Result r) {
    if (R_FAILED(r) && !erroConfig) erroConfig = r;
}

// Configura só as câmeras escolhidas (como o exemplo oficial do devkitPro faz) e
// nos dois "contextos" da câmera: se o tamanho 400x240 não pegar, a câmera manda
// 640x480 e cada linha "escorre" para a próxima (imagem torta, com faixas).
// Como a imagem sai de cada câmera. O padrão de um dos "contextos" da câmera vem
// virado, então sempre configuramos explicitamente (e dá para trocar no botão Virar).
// (arquivo novo: as escolhas salvas com o padrão antigo não valem mais)
#define ARQ_GIRO "sdmc:/3ds/3dshub_camera2.cfg"
// A câmera entrega a imagem espelhada; o "Desespelhar" (FLIP_HORIZONTAL) é o certo
// e vem primeiro, como padrão. Os outros ficam no botão Virar, se precisar.
static const CAMU_Flip giros[4] = { FLIP_HORIZONTAL, FLIP_NONE, FLIP_REVERSE, FLIP_VERTICAL };
static const char *nomesGiro[4] = { "normal", "espelhada", "invertida", "invertida e espelhada" };
static int giro[3];        // escolha de cada câmera (traseira, frontal, 3D); 0 = padrão

static void lerGiros(void) {
    giro[0] = giro[1] = giro[2] = 0;
    FILE *f = fopen(ARQ_GIRO, "r");
    if (!f) return;
    if (fscanf(f, "%d %d %d", &giro[0], &giro[1], &giro[2]) != 3) giro[0] = giro[1] = giro[2] = 0;
    fclose(f);
    for (int i = 0; i < 3; i++) if (giro[i] < 0 || giro[i] > 3) giro[i] = 0;
}

static void salvarGiros(void) {
    FILE *f = fopen(ARQ_GIRO, "w");
    if (!f) return;
    fprintf(f, "%d %d %d\n", giro[0], giro[1], giro[2]);
    fclose(f);
}

static void configurar(u32 sel, int giroEscolhido) {
    anotar(CAMU_FlipImage(sel, giros[giroEscolhido], CONTEXT_BOTH));
    anotar(CAMU_SetSize(sel, SIZE_CTR_TOP_LCD, CONTEXT_BOTH));
    anotar(CAMU_SetOutputFormat(sel, OUTPUT_RGB_565, CONTEXT_BOTH));
    anotar(CAMU_SetFrameRate(sel, FRAME_RATE_15));
    anotar(CAMU_SetNoiseFilter(sel, true));
    anotar(CAMU_SetAutoExposure(sel, true));
    anotar(CAMU_SetAutoWhiteBalance(sel, true));
    anotar(CAMU_SetTrimming(PORT_CAM1, false));
    anotar(CAMU_SetTrimming(PORT_CAM2, false));
}

static void ativar(int cam) {
    CAMU_Activate(SELECT_NONE);
    configurar(selecao(cam), giro[cam]);
    CAMU_GetMaxBytes(&bytesTransferencia, LARG, ALT);
    CAMU_SetTransferBytes(PORT_BOTH, bytesTransferencia, LARG, ALT);
    CAMU_Activate(selecao(cam));
    apagarLed();
    if (cam == CAM_3D) CAMU_SynchronizeVsyncTiming(SELECT_OUT1, SELECT_OUT2);
}

// HOME, tampa fechada ou fechar o app: a câmera é desligada na hora (ligada durante a
// suspensão, ela deixava o "fechando software" demorando muito) e religada na volta.
static volatile bool camDesligada, religarCam;

static void aoMudarApt(APT_HookType tipo, void *param) {
    if (tipo == APTHOOK_ONSUSPEND || tipo == APTHOOK_ONSLEEP || tipo == APTHOOK_ONEXIT) {
        if (!camDesligada) {
            CAMU_StopCapture(PORT_BOTH);
            CAMU_Activate(SELECT_NONE);
            camDesligada = true;
        }
    } else if (tipo == APTHOOK_ONRESTORE || tipo == APTHOOK_ONWAKEUP) {
        religarCam = true;
    }
}

// A captura demora; os botões apertados nesse meio-tempo ficam guardados aqui
// para o laço principal não perder nenhum toque.
static u32 downGuardado;
static touchPosition toqueGuardado;

static bool querSair(void) {
    hidScanInput();
    u32 down = hidKeysDown();
    if (down & KEY_TOUCH) hidTouchRead(&toqueGuardado);
    downGuardado |= down;
    return (down | hidKeysHeld()) & KEY_START;
}

// Tira uma foto (ou duas, no 3D). Retorna false se demorar demais.
// Espera em pedaços de 50 ms olhando o START, para nunca deixar de responder.
static bool capturar(int cam, u8 *buf, bool *sair) {
    Handle ev[2] = {0, 0};
    u32 p = porta(cam);
    bool ok = true;

    CAMU_ClearBuffer(p);
    CAMU_StartCapture(p);
    apagarLed();
    CAMU_SetReceiving(&ev[0], buf, PORT_CAM1, TAM_IMAGEM, (s16)bytesTransferencia);
    if (cam == CAM_3D)
        CAMU_SetReceiving(&ev[1], buf + TAM_IMAGEM, PORT_CAM2, TAM_IMAGEM, (s16)bytesTransferencia);

    for (int i = 0; i < 2; i++) {
        if (!ev[i]) continue;
        bool chegou = false;
        for (int t = 0; t < 10 && !chegou && !*sair; t++) {       // até 0,5 s
            // 0 = sinalizado; o "tempo esgotado" (0x09401BFE) não conta como erro no 3DS
            chegou = svcWaitSynchronization(ev[i], 50000000ULL) == 0;
            if (!chegou && querSair()) *sair = true;
        }
        if (!chegou) ok = false;
        svcCloseHandle(ev[i]);
    }
    CAMU_StopCapture(p);
    return ok;
}

static bool enviarOlho(int sock, const u16 *img, int olho, int total) {
    u8 *jpeg = NULL;
    unsigned long tam = 0;
    if (!jpegCodificar(img, LARG, ALT, QUALIDADE, &jpeg, &tam)) return true;   // pula o quadro
    Cabecalho c = { {'3', 'D', 'S', 'F'}, olho, total, 0, tam };
    bool ok = enviarTudo(sock, &c, sizeof(c), 3000) && enviarTudo(sock, jpeg, tam, 3000);
    free(jpeg);
    return ok;
}

// copia a foto (400 de largura) para a imagem da tela (linhas de 512)
static void paraPrevia(UiImagem *im, const u8 *foto) {
    for (int y = 0; y < ALT; y++)
        memcpy(im->pixels + y * UI_IMG_LARG, foto + y * LARG * 2, LARG * 2);
}

// ---------------------------------------------------------------- desenho
// posições dos botões da tela de baixo (as mesmas para desenhar e para o toque)
#define SEG_Y   72
#define SEG_H   36
#define SEG_W   93.3f
#define BTN_Y   118
#define BTN_H   44
static float segX(int i) { return 10 + i * (SEG_W + 10); }

typedef struct {
    int cam;
    bool conectado, pausado, temFoto, tresD;
    float fps;
} Estado;

static void desenharCima(const Estado *e, UiImagem *esq, UiImagem *dir) {
    uiTela(alvoCima);
    if (e->temFoto) uiImagemDesenhar(esq, 0, 0);
    else            uiTexto(200, 110, 14, COR_MUDO, CENTRO, 0, "Ligando a câmera...");

    // selo no canto: ao vivo / pausado / conectando
    const char *selo = !e->conectado ? "Conectando" : e->pausado ? "Pausado" : "Ao vivo";
    u32 cor = !e->conectado ? COR_AMARELO : e->pausado ? COR_MUDO : COR_VERMELHO;
    uiRetArred(8, 8, 104, 22, 11, COR_ESCURO);
    uiCirculo(21, 19, 5, cor);
    uiTexto(32, 11, 13, COR_TEXTO, ESQ, 76, "%s", selo);
    if (e->conectado && !e->pausado) {
        uiRetArred(316, 8, 76, 22, 11, COR_ESCURO);
        uiTexto(354, 11, 13, COR_TEXTO, CENTRO, 0, "%.1f fps", e->fps);
    }

    if (e->tresD) {                 // olho direito, para o 3D de verdade
        uiTela(alvoCimaDir);
        if (e->temFoto) uiImagemDesenhar(dir, 0, 0);
    }
}

static void desenharBaixo(const Estado *e) {
    uiTela(alvoBaixo);

    // estado da conexão
    uiRetArred(10, 8, 300, 40, 10, COR_CARTAO);
    uiIcone(IC_CAMERA, 30, 28, 20, COR_CORAL_CL, COR_CARTAO);
    if (!e->conectado)    uiTexto(48, 19, 14, COR_AMARELO, ESQ, 140, "Conectando ao PC...");
    else if (e->pausado)  uiTexto(48, 19, 14, COR_TEXTO2, ESQ, 140, "Pausado");
    else                  uiTexto(48, 19, 14, COR_AGUA_CL, ESQ, 140, "Enviando ao PC");
    uiTexto(300, 21, 12, COR_MUDO, DIR, 110, "PC %s", ipPC);

    // escolha da câmera (e como a imagem dela está virada)
    uiTexto(10, 54, 12, COR_MUDO, ESQ, 0, "Câmera (Y)");
    uiTexto(310, 54, 12, COR_MUDO, DIR, 0, "imagem: %s (B)", nomesGiro[giro[e->cam]]);
    for (int i = 0; i < N_CAMERAS; i++) {
        bool s = i == e->cam;
        uiBotao(segX(i), SEG_Y, SEG_W, SEG_H, nomesCamera[i], -1,
                s ? COR_CORAL : COR_CARTAO, s ? COR_BRANCO : COR_TEXTO2);
    }

    // pausar, LED e virar
    uiBotao(segX(0), BTN_Y, SEG_W, BTN_H, e->pausado ? "Retomar" : "Pausar",
            e->pausado ? IC_PLAY : IC_PAUSE, COR_CARTAO, COR_TEXTO);
    const char *led = !mcuOk ? "LED travado" : ledApagado ? "LED off" : "LED on";
    uiBotao(segX(1), BTN_Y, SEG_W, BTN_H, led, IC_LED, COR_CARTAO, ledApagado || !mcuOk ? COR_MUDO : COR_AMARELO);
    uiBotao(segX(2), BTN_Y, SEG_W, BTN_H, "Virar", IC_TELA_CHEIA, COR_CARTAO, COR_TEXTO);

    // como usar no OBS (ou o erro da câmera, se o 3DS recusou alguma configuração)
    uiRetArred(10, 172, 300, 44, 10, erroConfig ? HEX(0x501313) : COR_CARTAO);
    if (erroConfig) {
        uiTexto(20, 177, 12, COR_VERMELHO, ESQ, 280, "A câmera recusou a configuração");
        uiTexto(20, 194, 13, COR_TEXTO2, ESQ, 280, "erro 0x%08lX (me mande esse código)", erroConfig);
    } else {
        uiTexto(20, 177, 12, COR_MUDO, ESQ, 0, "No OBS: Fonte > Navegador");
        uiTexto(20, 194, 13, COR_TEXTO2, ESQ, 280, "http://127.0.0.1:8090/obs");
    }
    uiTexto(10, 222, 11, COR_MUDO, ESQ, 0, "A pausar  X LED  B virar");
    uiTexto(310, 222, 11, COR_MUDO, DIR, 0, "START: voltar ao menu");
}

// ---------------------------------------------------------------- módulo

void modCamera(void) {
    u8 *buf = malloc(TAM_IMAGEM * 2);
    static UiImagem esq, dir;
    bool imagensOk = uiImagemCriar(&esq, LARG, ALT) && uiImagemCriar(&dir, LARG, ALT);
    if (!buf || !imagensOk || R_FAILED(camInit())) {
        uiImagemLiberar(&esq);
        uiImagemLiberar(&dir);
        free(buf);
        uiAviso("Câmera", "Não consegui abrir a câmera do 3DS.", IC_CAMERA, COR_CORAL);
        return;
    }

    erroConfig = 0;   // a configuração de cada câmera acontece ao ligá-la (ativar)
    lerGiros();

    // acesso ao microcontrolador, para apagar o LED traseiro
    mcuOk = R_SUCCEEDED(mcuHwcInit());

    Estado e;
    memset(&e, 0, sizeof(e));
    e.cam = TRASEIRA;
    ativar(e.cam);
    camDesligada = religarCam = false;
    aptHookCookie gancho;
    aptHook(&gancho, aoMudarApt, NULL);

    int sock = -1;
    u64 proximaTentativa = 0, t0 = osGetTime();
    int quadros = 0;
    downGuardado = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 agora = hidKeysDown();
        touchPosition tp;
        if (agora & KEY_TOUCH) hidTouchRead(&tp);
        else                   tp = toqueGuardado;
        u32 down = agora | downGuardado;           // inclui o que foi apertado durante a captura
        downGuardado = 0;
        if ((down | hidKeysHeld()) & KEY_START) break;
        if (religarCam) {                    // voltou do HOME ou a tampa abriu
            religarCam = camDesligada = false;
            ativar(e.cam);
        }

        int novaCam = -1;
        bool trocarPausa = down & KEY_A, trocarLed = down & KEY_X, virar = down & KEY_B;
        if (down & KEY_Y) novaCam = (e.cam + 1) % N_CAMERAS;
        if (down & KEY_TOUCH) {
            for (int i = 0; i < N_CAMERAS; i++)
                if (uiDentro(&tp, segX(i), SEG_Y, SEG_W, SEG_H)) novaCam = i;
            if (uiDentro(&tp, segX(0), BTN_Y, SEG_W, BTN_H)) trocarPausa = true;
            if (uiDentro(&tp, segX(1), BTN_Y, SEG_W, BTN_H)) trocarLed = true;
            if (uiDentro(&tp, segX(2), BTN_Y, SEG_W, BTN_H)) virar = true;
        }
        if (novaCam >= 0 && novaCam != e.cam) {
            e.cam = novaCam;
            ativar(e.cam);
        }
        if (virar) {                         // próximo jeito de virar, salvo para esta câmera
            giro[e.cam] = (giro[e.cam] + 1) % 4;
            salvarGiros();
            ativar(e.cam);
        }
        if (trocarPausa) e.pausado = !e.pausado;
        if (trocarLed && mcuOk) {
            ledApagado = !ledApagado;
            if (ledApagado) {
                apagarLed();
            } else if (e.cam != FRONTAL) {        // devolve o LED aceso, como o 3DS faz
                u8 aceso = 2;
                MCUHWC_WriteRegister(REG_LED_CAMERA, &aceso, 1);
            }
        }

        // conecta (e reconecta) ao Hub; antes, mostra "conectando" na tela
        if (sock < 0 && osGetTime() >= proximaTentativa) {
            uiQuadro();
            desenharCima(&e, &esq, &dir);
            desenharBaixo(&e);
            uiFim();
            sock = conectarTCP(PORTA, 1500);
            if (sock == CONEXAO_CANCELADA) break;    // apertou START durante a conexão
            if (sock < 0) proximaTentativa = osGetTime() + 2000;
        }
        e.conectado = sock >= 0;

        if (camDesligada) {                   // suspenso/fechando: não captura
            uiQuadro();
            desenharCima(&e, &esq, &dir);
            desenharBaixo(&e);
            uiFim();
            continue;
        }
        bool sair = false;
        bool fotoOk = capturar(e.cam, buf, &sair);
        if (sair) break;

        if (fotoOk) {
            // prévia
            e.tresD = e.cam == CAM_3D && SLIDER_3D > 0.0f;
            paraPrevia(&esq, buf);
            uiImagemEnviar(&esq);
            if (e.tresD) {
                paraPrevia(&dir, buf + TAM_IMAGEM);
                uiImagemEnviar(&dir);
            }
            e.temFoto = true;

            // envio
            if (sock >= 0 && !e.pausado) {
                int total = e.cam == CAM_3D ? 2 : 1;
                bool ok = enviarOlho(sock, (u16 *)buf, 0, total);
                if (ok && total == 2) ok = enviarOlho(sock, (u16 *)(buf + TAM_IMAGEM), 1, total);
                if (!ok) {
                    fecharRede(sock);
                    sock = -1;
                    e.conectado = false;
                    proximaTentativa = osGetTime() + 2000;
                } else {
                    quadros++;
                }
            }
        }

        if (osGetTime() - t0 >= 1000) {
            e.fps = quadros * 1000.0f / (osGetTime() - t0);
            quadros = 0;
            t0 = osGetTime();
        }

        gfxSet3D(e.tresD);
        uiQuadro();
        desenharCima(&e, &esq, &dir);
        desenharBaixo(&e);
        uiFim();
    }

    aptUnhook(&gancho);
    gfxSet3D(false);
    if (sock >= 0) fecharRede(sock);
    if (!camDesligada) {
        CAMU_StopCapture(PORT_BOTH);
        CAMU_Activate(SELECT_NONE);   // desligar a câmera já apaga o LED
    }
    camExit();
    if (mcuOk) mcuHwcExit();
    uiImagemLiberar(&esq);
    uiImagemLiberar(&dir);
    free(buf);
}
