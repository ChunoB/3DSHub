// Sintetizador — a tela de toque vira um teclado ou um theremin
//
// Controles:
//   toque            : tocar
//   X                : alterna TECLADO / THEREMIN
//   Y                : troca a forma de onda
//   cima/baixo       : oitava
//   L                : liga/desliga a inclinação (entortar nota e brilho)
//   R                : liga/desliga o eco
//   START            : voltar ao menu
//
// O som sai pelo NDSP, que precisa do arquivo sdmc:/3ds/dspfirm.cdc
// (gerado uma vez pelo homebrew DSP1).

#include "comum.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------- áudio
#define TAXA        32000
#define AMOSTRAS    256          // por buffer (8 ms)
#define N_BUFFERS   4
#define TAB_SENO    1024
#define ECO_AMOSTRAS (TAXA * 3 / 10)   // 300 ms

enum { SENO, QUADRADA, SERRA, TRIANGULO, N_ONDAS };

// Parâmetros que a thread principal escreve e a de áudio lê.
static volatile struct {
    float freq, amp;     // alvos (o áudio desliza até eles)
    float glide;         // quão rápido a frequência chega no alvo
    float corte;         // filtro passa-baixa: 1 = aberto, perto de 0 = abafado
    int   onda;
    bool  eco;
} P = { 440, 0, 0.02f, 1, QUADRADA, false };

static float tabSeno[TAB_SENO];
static float ecoBuf[ECO_AMOSTRAS];
static ndspWaveBuf wbuf[N_BUFFERS];
static s16 *audioMem;
static LightEvent eventoAudio;
static volatile bool rodando = true;

static void preencher(s16 *saida, int n) {
    static float freq = 440, amp = 0, fase = 0, filtro = 0;
    static int posEco = 0;
    float alvoF = P.freq, alvoA = P.amp, glide = P.glide, corte = P.corte;
    int onda = P.onda;
    bool eco = P.eco;

    for (int i = 0; i < n; i++) {
        freq += (alvoF - freq) * glide;
        amp  += (alvoA - amp) * 0.004f;   // ataque/soltura suaves, sem estalo
        fase += freq * (1.0f / TAXA);     // multiplicação: mais barata que dividir
        if (fase >= 1) fase -= 1;

        float v;
        switch (onda) {
            case SENO:      v = tabSeno[(int)(fase * TAB_SENO) & (TAB_SENO - 1)]; break;
            case QUADRADA:  v = fase < 0.5f ? 0.6f : -0.6f; break;
            case SERRA:     v = (2 * fase - 1) * 0.8f; break;
            default:        v = 4 * fabsf(fase - 0.5f) - 1; break;
        }
        filtro += corte * (v - filtro);
        float s = filtro * amp;

        float atrasado = ecoBuf[posEco];
        ecoBuf[posEco] = eco ? s + atrasado * 0.45f : 0;
        if (++posEco >= ECO_AMOSTRAS) posEco = 0;
        if (eco) s += atrasado * 0.5f;

        int x = (int)(s * 0.35f * 32767);
        saida[i] = x > 32767 ? 32767 : x < -32768 ? -32768 : x;
    }
}

static void callbackNdsp(void *dados) {
    (void)dados;
    LightEvent_Signal(&eventoAudio);
}

static void threadAudio(void *arg) {
    (void)arg;
    while (rodando) {
        for (int i = 0; i < N_BUFFERS; i++) {
            if (wbuf[i].status != NDSP_WBUF_DONE && wbuf[i].status != NDSP_WBUF_FREE)
                continue;
            preencher(wbuf[i].data_pcm16, AMOSTRAS);
            DSP_FlushDataCache(wbuf[i].data_pcm16, AMOSTRAS * sizeof(s16));
            ndspChnWaveBufAdd(0, &wbuf[i]);
        }
        LightEvent_Wait(&eventoAudio);
    }
}

static void encerrarAudio(Thread t);

static bool iniciarAudio(Thread *t) {
    if (R_FAILED(ndspInit())) return false;

    for (int i = 0; i < TAB_SENO; i++) tabSeno[i] = sinf(2 * M_PI * i / TAB_SENO);

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, TAXA);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
    float mix[12] = { 1.0f, 1.0f };
    ndspChnSetMix(0, mix);

    audioMem = linearAlloc(N_BUFFERS * AMOSTRAS * sizeof(s16));
    if (!audioMem) {
        ndspExit();
        return false;
    }
    memset(wbuf, 0, sizeof(wbuf));
    for (int i = 0; i < N_BUFFERS; i++) {
        wbuf[i].data_vaddr = audioMem + i * AMOSTRAS;
        wbuf[i].nsamples   = AMOSTRAS;
        wbuf[i].status     = NDSP_WBUF_DONE;
    }

    LightEvent_Init(&eventoAudio, RESET_ONESHOT);
    ndspSetCallback(callbackNdsp, NULL);

    s32 prio;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    *t = threadCreate(threadAudio, NULL, 32 * 1024, prio - 1, -2, false);
    if (!*t) {
        encerrarAudio(NULL);
        return false;
    }
    return true;
}

static void encerrarAudio(Thread t) {
    rodando = false;
    LightEvent_Signal(&eventoAudio);
    if (t) {
        threadJoin(t, U64_MAX);
        threadFree(t);
    }
    ndspSetCallback(NULL, NULL);
    ndspChnWaveBufClear(0);
    ndspExit();
    if (audioMem) linearFree(audioMem);
    audioMem = NULL;
}

// ---------------------------------------------------------------- tela de baixo

// Teclado: 2 oitavas = 14 teclas brancas de 22 px, centralizadas.
#define LARG_BRANCA 22
#define X0_TECLADO  ((320 - 14 * LARG_BRANCA) / 2)
#define LARG_PRETA  14
#define ALT_PRETA   140

static const int semiBranca[7] = { 0, 2, 4, 5, 7, 9, 11 };
static const bool temSustenido[7] = { true, true, false, true, true, true, false };

static int semitomBranca(int i) { return (i / 7) * 12 + semiBranca[i % 7]; }
static int centroPreta(int i)   { return X0_TECLADO + (i + 1) * LARG_BRANCA; }

// Retorna o semitom (0..23) sob o ponto, ou -1.
static int teclaEm(int x, int y) {
    if (y < ALT_PRETA) {
        for (int i = 0; i < 13; i++) {
            if (!temSustenido[i % 7]) continue;
            int c = centroPreta(i);
            if (x >= c - LARG_PRETA / 2 && x < c + LARG_PRETA / 2)
                return semitomBranca(i) + 1;
        }
    }
    int i = (x - X0_TECLADO) / LARG_BRANCA;
    if (x < X0_TECLADO || i > 13) return -1;
    return semitomBranca(i);
}

static void desenharTeclado(int tocada, int oitava) {
    uiTela(alvoBaixo);
    for (int i = 0; i < 14; i++) {
        float x = X0_TECLADO + i * LARG_BRANCA;
        bool apertada = semitomBranca(i) == tocada;
        // começa acima da tela para só os cantos de baixo aparecerem arredondados
        uiRetArred(x + 1, -6, LARG_BRANCA - 2, 242, 5, apertada ? COR_ROSA_CL : COR_BRANCO);
        if (i % 7 == 0)   // marca os Dós com o nome da nota
            uiTexto(x + LARG_BRANCA / 2, 216, 10, apertada ? COR_ESCURO : COR_MUDO, CENTRO, 0,
                    "C%d", oitava + i / 7);
    }
    for (int i = 0; i < 13; i++) {
        if (!temSustenido[i % 7]) continue;
        float c = centroPreta(i);
        bool apertada = semitomBranca(i) + 1 == tocada;
        uiRetArred(c - LARG_PRETA / 2, -6, LARG_PRETA, ALT_PRETA + 6, 4, apertada ? COR_ROSA : HEX(0x22263A));
    }
}

static void desenharTheremin(bool tocando, int tx, int ty, int oitava) {
    uiTela(alvoBaixo);
    // degradê: em cima = volume mais alto
    u32 topo = HEX(0x4B1528);
    C2D_DrawRectangle(0, 0, 0.5f, 320, 240, topo, topo, COR_FUNDO, COR_FUNDO);
    // uma linha por semitom; os Dós mais fortes e com nome
    for (int s = 0; s <= 24; s++) {
        float x = s * 320 / 24.0f;
        bool doo = s % 12 == 0;
        uiRet(x - (doo ? 1 : 0.5f), 0, doo ? 2 : 1, 240, doo ? HEX(0x72243E) : HEX(0x2E2236));
        if (doo && s < 24) uiTexto(x + 4, 4, 11, COR_ROSA_CL, ESQ, 0, "C%d", oitava + s / 12);
    }
    if (tocando) {
        uiLinha(tx, 0, tx, 240, 2, COR_ROSA);
        uiLinha(0, ty, 320, ty, 2, COR_ROSA);
        uiCirculo(tx, ty, 9, COR_ROSA_CL);
    }
    uiTexto(316, 222, 11, COR_MUDO, DIR, 0, "lados: nota   cima: volume");
}

// ---------------------------------------------------------------- tela de cima
static const char *nomesNota[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
static const char *nomesOndaBonitos[] = { "Seno", "Quadrada", "Serra", "Triângulo" };

// desenho da forma de onda: 2 períodos dentro do retângulo
static void desenharOnda(int onda, float x, float y, float w, float h, u32 cor) {
    float xAntes = 0, yAntes = 0;
    for (int i = 0; i <= 48; i++) {
        float f = fmodf(i / 24.0f, 1.0f), v;
        switch (onda) {
            case SENO:      v = sinf(2 * (float)M_PI * f); break;
            case QUADRADA:  v = f < 0.5f ? 1 : -1; break;
            case SERRA:     v = 2 * f - 1; break;
            default:        v = 4 * fabsf(f - 0.5f) - 1; break;
        }
        float px = x + i * w / 48, py = y + h / 2 - v * h / 2;
        if (i) uiLinha(xAntes, yAntes, px, py, 2, cor);
        xAntes = px;
        yAntes = py;
    }
}

static void pilula(float x, float y, float w, const char *rotulo, bool ligado) {
    uiRetArred(x, y, w, 24, 12, ligado ? COR_ROSA : COR_CARTAO2);
    uiTexto(x + w / 2, y + 4, 13, ligado ? COR_BRANCO : COR_MUDO, CENTRO, 0, "%s", rotulo);
}

static void desenharCima(bool theremin, int oitava, bool inclinacao, bool eco,
                         float midi, bool tocando, float entortar, float brilho) {
    uiTela(alvoCima);
    uiBarraStatus("Sintetizador", IC_PIANO, COR_ROSA_CL);

    // nota atual, grande
    uiRetArred(16, 30, 150, 110, 12, COR_CARTAO);
    uiTexto(28, 38, 12, COR_MUDO, ESQ, 0, "Nota");
    if (tocando) {
        int m = (int)roundf(midi);
        uiTexto(91, 58, 40, COR_ROSA_CL, CENTRO, 140, "%s%d", nomesNota[((m % 12) + 12) % 12], m / 12 - 1);
        uiTexto(91, 108, 13, COR_TEXTO2, CENTRO, 140, "%.1f Hz", 440.0f * powf(2, (midi - 69) / 12));
    } else {
        uiTexto(91, 66, 28, COR_LINHA, CENTRO, 0, "-");
    }

    // forma de onda
    uiRetArred(176, 30, 208, 52, 12, COR_CARTAO);
    uiTexto(188, 36, 12, COR_MUDO, ESQ, 0, "Onda (Y)");
    uiTexto(188, 54, 16, COR_TEXTO, ESQ, 90, "%s", nomesOndaBonitos[P.onda]);
    desenharOnda(P.onda, 284, 40, 90, 32, COR_ROSA_CL);

    // modo e oitava
    uiRetArred(176, 88, 100, 52, 12, COR_CARTAO);
    uiTexto(188, 94, 12, COR_MUDO, ESQ, 0, "Modo (X)");
    uiTexto(188, 112, 16, COR_TEXTO, ESQ, 84, "%s", theremin ? "Theremin" : "Teclado");
    uiRetArred(284, 88, 100, 52, 12, COR_CARTAO);
    uiTexto(296, 94, 12, COR_MUDO, ESQ, 0, "Oitava");
    uiTexto(296, 112, 16, COR_TEXTO, ESQ, 0, "%d", oitava);
    uiTexto(372, 114, 11, COR_MUDO, DIR, 0, "cima/baixo");

    // eco, inclinação e os medidores da inclinação
    uiRetArred(16, 146, 368, 58, 12, COR_CARTAO);
    pilula(28, 163, 70, "Eco (R)", eco);
    pilula(106, 163, 108, "Inclinação (L)", inclinacao);
    if (inclinacao) {
        // entortar: -2 a +2 semitons, marcador a partir do meio da barra
        uiTexto(226, 152, 11, COR_MUDO, ESQ, 0, "Entortar %+.1f", entortar);
        uiRetArred(226, 168, 146, 6, 3, COR_CARTAO2);
        uiRet(298.5f, 165, 1, 12, COR_LINHA);
        uiCirculo(299 + entortar / 2 * 70, 171, 5, COR_ROSA_CL);
        uiTexto(226, 180, 11, COR_MUDO, ESQ, 0, "Brilho %d%%", (int)(brilho * 100));
        uiBarra(226, 195, 146, 6, brilho, COR_ROSA_CL, COR_CARTAO2);
    } else {
        uiTexto(298, 168, 12, COR_MUDO, CENTRO, 0, "Ligue para entortar a nota");
    }

    uiTexto(16, 214, 13, COR_MUDO, ESQ, 0, "START: voltar ao menu");
}

// ---------------------------------------------------------------- módulo
static float limitar(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void modSintetizador(void) {
    rodando = true;
    P.amp = 0;
    memset(ecoBuf, 0, sizeof(ecoBuf));

    Thread t = NULL;
    if (!iniciarAudio(&t)) {
        uiAviso("Sem som",
                "Falta o arquivo sdmc:/3ds/dspfirm.cdc.\n\n"
                "Para criar: abra o menu Rosalina\n"
                "(L + baixo + SELECT) > Miscellaneous options\n"
                "> Dump DSP firmware. Depois abra de novo.", IC_MUDO, COR_ROSA);
        return;
    }

    HIDUSER_EnableAccelerometer();

    // Com o NDSP ligado, deixar o libctru suspender o app (HOME) ou dormir (tampa) pode
    // travar o console esperando o DSP. Então o HOME é tratado aqui: desliga o som,
    // vai para o HOME e religa na volta. E sem dormir enquanto o sintetizador está aberto.
    aptSetHomeAllowed(false);
    bool podiaDormir = aptIsSleepAllowed();
    aptSetSleepAllowed(false);

    bool theremin = false, inclinacao = false;
    int oitava = 4;

    while (aptMainLoop()) {
        if (aptCheckHomePressRejected()) {
            encerrarAudio(t);
            t = NULL;
            aptJumpToHomeMenu();
            if (aptShouldClose()) break;          // fechou pelo HOME: sai sem religar o som
            rodando = true;
            P.amp = 0;
            if (!iniciarAudio(&t)) t = NULL;
            continue;
        }
        hidScanInput();
        u32 down = hidKeysDown(), held = hidKeysHeld();

        if (down & KEY_START) break;
        if (down & KEY_X) theremin = !theremin;
        if (down & KEY_Y) P.onda = (P.onda + 1) % N_ONDAS;
        if ((down & KEY_DUP)   && oitava < 7) oitava++;
        if ((down & KEY_DDOWN) && oitava > 1) oitava--;
        if (down & KEY_L) inclinacao = !inclinacao;
        if (down & KEY_R) P.eco = !P.eco;

        // inclinação: lados = entortar a nota (±2 semitons), frente/trás = brilho
        float entortar = 0, brilho = 1;
        if (inclinacao) {
            accelVector a;
            hidAccelRead(&a);
            float lado = limitar(a.x / 250.0f, -1, 1);
            if (fabsf(lado) < 0.1f) lado = 0;
            entortar = lado * 2;
            brilho = limitar((a.y / 250.0f + 1) / 2, 0, 1);
        }
        P.corte = 0.03f + 0.97f * brilho * brilho;

        touchPosition tp;
        hidTouchRead(&tp);
        bool tocando = held & KEY_TOUCH;
        float base = (oitava + 1) * 12;   // C da oitava escolhida (C4 = 60)
        float midi = 0;
        int tecla = -1;

        if (theremin) {
            if (tocando) {
                midi = base + tp.px * 24.0f / 320;
                P.amp = 1.0f - tp.py / 260.0f;
            } else {
                P.amp = 0;
            }
            P.glide = 0.0015f;
        } else {
            tecla = tocando ? teclaEm(tp.px, tp.py) : -1;
            if (tecla >= 0) {
                midi = base + tecla;
                P.amp = 1;
            } else {
                P.amp = 0;
                tocando = false;
            }
            P.glide = 0.02f;
        }

        if (tocando)
            P.freq = 440.0f * powf(2, (midi + entortar - 69) / 12);

        uiQuadro();
        desenharCima(theremin, oitava, inclinacao, P.eco, midi + entortar, tocando, entortar, brilho);
        if (theremin) desenharTheremin(tocando, tp.px, tp.py, oitava);
        else          desenharTeclado(tecla, oitava);
        uiFim();
    }

    HIDUSER_DisableAccelerometer();
    if (t || audioMem) encerrarAudio(t);
    aptSetSleepAllowed(podiaDormir);
    aptSetHomeAllowed(true);
}
