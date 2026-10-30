// Karaokê — a música toca no PC, a letra rola aqui e o PC dá nota para a sua voz.
//
//   toque uma música no PC (Spotify, YouTube...) e cante no microfone do 3DS
//   tela de cima : a letra, sincronizada com a música
//   tela de baixo: o círculo das 12 notas: a da música (rosa) e a sua (amarela)
//   START        : sair
//
// O 3DS descobre a nota que você canta (YIN, uma autocorrelação, no próprio 3DS) e
// manda só ela; o PC (pc/karaoke.py, UDP 5016) compara com as notas que estão tocando,
// diz em que ponto a música está e manda a letra (do lrclib.net) em pedaços.

#include "comum.h"

#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORTA       5016
#define MIC_BUF     0x30000
#define MIC_TAXA    16360.0f
#define N_AMOSTRAS  1024                // janela de ~62 ms
#define YIN_W       512
#define TAU_MIN     16                  // ~1000 Hz
#define TAU_MAX     205                 // ~80 Hz
#define MAX_LINHAS  220
#define MAX_LETRA   (24 * 1024)

static const char *nomesNotas[12] = { "Dó", "Dó#", "Ré", "Ré#", "Mi", "Fá", "Fá#", "Sol", "Sol#", "Lá", "Lá#", "Si" };

static struct {
    bool tocando;
    u32 posMs, id;
    int notaAlvo, acerto, estadoLetra;  // estadoLetra: 0 buscando, 1 achou, 2 sem letra
    u32 pontos;
    char titulo[96], artista[64];
    u64 quando;                         // quando chegou (para andar a posição entre respostas)
} pc;

// letra montada a partir dos pedaços
static char letraBruta[MAX_LETRA + 1];
static int letraTam;
static u32 letraId;                     // de qual música é a letra (0 = nenhuma)
static int partesTotal, proximaParte;   // pedindo: parte 'proximaParte' de 'partesTotal'
static bool letraPronta;
static struct { u32 ms; const char *texto; } linhas[MAX_LINHAS];
static int nLinhas;

// ---------------------------------------------------------------- nota da voz (YIN)

static float amostras[N_AMOSTRAS];
static float difs[TAU_MAX + 1];

// Frequência em Hz (0 = sem nota clara). vol = 0 a 100.
static float detectarNota(float *vol) {
    float soma = 0;
    for (int i = 0; i < N_AMOSTRAS; i++) soma += amostras[i] * amostras[i];
    float rms = sqrtf(soma / N_AMOSTRAS);
    *vol = rms / 32768 * 700;
    if (*vol > 100) *vol = 100;
    if (rms < 250) return 0;                                    // silêncio / barulho baixo

    // diferença ao quadrado para cada atraso, normalizada pela média acumulada (YIN)
    float acumulado = 0;
    difs[0] = 1;
    for (int tau = 1; tau <= TAU_MAX; tau++) {
        float d = 0;
        for (int j = 0; j < YIN_W; j++) {
            float v = amostras[j] - amostras[j + tau];
            d += v * v;
        }
        acumulado += d;
        difs[tau] = acumulado > 0 ? d * tau / acumulado : 1;
    }
    for (int tau = TAU_MIN; tau < TAU_MAX; tau++) {
        if (difs[tau] < 0.15f) {
            while (tau + 1 < TAU_MAX && difs[tau + 1] < difs[tau]) tau++;   // desce até o fundo do vale
            // ajuste fino entre os vizinhos (parábola)
            float a = difs[tau - 1], b = difs[tau], c = difs[tau + 1];
            float desloc = (a - 2 * b + c) != 0 ? 0.5f * (a - c) / (a - 2 * b + c) : 0;
            return MIC_TAXA / (tau + desloc);
        }
    }
    return 0;
}

static int notaDe(float hz) {
    if (hz <= 0) return -1;
    int midi = (int)lroundf(12 * log2f(hz / 440.0f) + 69);
    return ((midi % 12) + 12) % 12;
}

// ---------------------------------------------------------------- letra

static void montarLetra(void) {
    nLinhas = 0;
    letraBruta[letraTam] = 0;
    char *salvo;
    for (char *l = strtok_r(letraBruta, "\n", &salvo); l && nLinhas < MAX_LINHAS; l = strtok_r(NULL, "\n", &salvo)) {
        char *tab = strchr(l, '\t');
        if (!tab) continue;
        *tab = 0;
        linhas[nLinhas].ms = strtoul(l, NULL, 10);
        linhas[nLinhas].texto = tab + 1;
        nLinhas++;
    }
    letraPronta = true;
}

static int linhaAtual(u32 ms) {
    int atual = -1;
    for (int i = 0; i < nLinhas; i++) {
        if (linhas[i].ms <= ms) atual = i;
        else break;
    }
    return atual;
}

// ---------------------------------------------------------------- desenho

static u32 posicaoAgora(void) {
    if (!pc.tocando) return pc.posMs;
    return pc.posMs + (u32)(osGetTime() - pc.quando);
}

static void desenharCima(bool conectado) {
    uiTela(alvoCima);
    uiBarraStatus("Karaokê", IC_MICROFONE, COR_KARAOKE_CL);
    if (!conectado) {
        uiTexto(200, 100, 16, COR_TEXTO, CENTRO, 360, "Procurando o PC...");
        return;
    }
    if (!pc.titulo[0]) {
        uiTexto(200, 90, 18, COR_TEXTO, CENTRO, 360, "Toque uma música no PC");
        uiTexto(200, 120, 12, COR_MUDO, CENTRO, 360, "Spotify, YouTube, o que for: a letra aparece aqui");
        return;
    }
    uiTexto(12, UI_BARRA + 4, 13, COR_KARAOKE_CL, ESQ, 300, "%s", pc.titulo);
    uiTexto(12, UI_BARRA + 20, 11, COR_MUDO, ESQ, 300, "%s%s", pc.artista, pc.tocando ? "" : "  (pausada)");
    uiTexto(388, UI_BARRA + 6, 18, COR_AMARELO, DIR, 0, "%lu", (unsigned long)pc.pontos);

    if (letraPronta && letraId == pc.id && nLinhas) {
        int a = linhaAtual(posicaoAgora());
        // a linha atual no meio; a anterior em cima, as próximas embaixo
        for (int k = -1; k <= 2; k++) {
            int i = a + k;
            if (i < 0 || i >= nLinhas) continue;
            float y = 108 + k * 34;
            if (k == 0) {
                uiRetArred(8, y - 6, 384, 32, 10, HEX(0x3A1436));
                uiTexto(200, y, 17, COR_BRANCO, CENTRO, 376, "%s", linhas[i].texto[0] ? linhas[i].texto : "...");
            } else {
                uiTexto(200, y + 2, 13, k < 0 ? COR_MUDO : COR_TEXTO2, CENTRO, 376, "%s",
                        linhas[i].texto[0] ? linhas[i].texto : "...");
            }
        }
        if (a < 0) uiTexto(200, 108, 15, COR_MUDO, CENTRO, 0, "(introdução...)");
    } else if (pc.estadoLetra == 2) {
        uiTexto(200, 100, 15, COR_TEXTO2, CENTRO, 376, "Não achei a letra desta música.");
        uiTexto(200, 124, 12, COR_MUDO, CENTRO, 376, "Dá para cantar assim mesmo: a nota continua valendo.");
    } else {
        uiTexto(200, 108, 15, COR_TEXTO2, CENTRO, 0, "Buscando a letra...");
    }
}

static void desenharBaixo(int minhaNota, float vol, bool cantando) {
    uiTela(alvoBaixo);
    const float cx = 112, cy = 112, r = 84;
    uiAnel(cx, cy, r + 14, 2, COR_LINHA, COR_FUNDO);
    for (int n = 0; n < 12; n++) {
        float a = n * (float)M_PI / 6 - (float)M_PI / 2;
        float x = cx + cosf(a) * r, y = cy + sinf(a) * r;
        bool alvo = n == pc.notaAlvo, minha = n == minhaNota;
        u32 fundo = alvo && minha ? COR_VERDE : alvo ? COR_KARAOKE : minha ? COR_AMARELO : COR_CARTAO;
        uiCirculo(x, y, alvo || minha ? 15 : 12, fundo);
        uiTexto(x, y - 7, 11, alvo || minha ? COR_BRANCO : COR_MUDO, CENTRO, 0, "%s", nomesNotas[n]);
    }
    if (minhaNota >= 0) {                                       // ponteiro da sua voz
        float a = minhaNota * (float)M_PI / 6 - (float)M_PI / 2;
        uiLinha(cx, cy, cx + cosf(a) * (r - 18), cy + sinf(a) * (r - 18), 3, COR_AMARELO);
    }
    uiCirculo(cx, cy, 22, COR_CARTAO2);
    uiTexto(cx, cy - 9, 15, minhaNota >= 0 && minhaNota == pc.notaAlvo ? COR_VERDE : COR_TEXTO, CENTRO, 0,
            "%s", minhaNota >= 0 ? nomesNotas[minhaNota] : "-");

    // à direita: acerto, volume e legenda
    uiTexto(222, 14, 12, COR_TEXTO2, ESQ, 0, "Afinação");
    uiBarra(222, 32, 88, 12, pc.acerto / 100.0f, pc.acerto >= 70 ? COR_VERDE : pc.acerto >= 40 ? COR_AMARELO : COR_VERMELHO,
            COR_CARTAO2);
    uiTexto(222, 54, 12, COR_TEXTO2, ESQ, 0, "Sua voz");
    uiBarra(222, 72, 88, 12, vol / 100, COR_KARAOKE_CL, COR_CARTAO2);
    uiCirculo(228, 106, 6, COR_KARAOKE);
    uiTexto(240, 99, 11, COR_MUDO, ESQ, 0, "música");
    uiCirculo(228, 126, 6, COR_AMARELO);
    uiTexto(240, 119, 11, COR_MUDO, ESQ, 0, "você");
    const char *frase = !cantando ? "Cante!" : pc.acerto >= 70 ? "Perfeito!" : pc.acerto >= 40 ? "Quase!" : "Desafinou...";
    uiTexto(266, 150, 15, COR_TEXTO, CENTRO, 0, "%s", frase);
    uiTexto(160, 222, 10, COR_MUDO, CENTRO, 0, "Oitava não importa - START: sair");
}

// ---------------------------------------------------------------- módulo

void modKaraoke(void) {
    struct sockaddr_in dest;
    int sock = abrirUDP(PORTA, &dest);
    if (sock < 0) return;

    u8 *micBuf = memalign(0x1000, MIC_BUF);
    bool mic = micBuf && R_SUCCEEDED(micInit(micBuf, MIC_BUF));
    u32 micTam = 0;
    if (mic) {
        micTam = micGetSampleDataSize();
        mic = R_SUCCEEDED(MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_16360, 0, micTam, true));
    }
    if (!mic) {
        if (micBuf) { micExit(); free(micBuf); }
        fecharRede(sock);
        uiAviso("Karaokê", "Não consegui ligar o microfone do 3DS.", IC_MICROFONE, COR_KARAOKE);
        return;
    }

    memset(&pc, 0, sizeof(pc));
    pc.notaAlvo = 255;
    letraId = 0;
    letraPronta = false;
    nLinhas = 0;
    u64 ultimoEnvio = 0, ultimoPedidoLetra = 0, ultimaResposta = 0;
    int minhaNota = -1;
    float vol = 0, hz = 0;

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        u64 agora = osGetTime();

        // as últimas N amostras do microfone (o buffer dá a volta)
        u32 fim = micGetLastSampleOffset() / 2;
        u32 total = micTam / 2;
        const s16 *dados = (const s16 *)micBuf;
        for (int i = 0; i < N_AMOSTRAS; i++)
            amostras[i] = dados[(fim + total - N_AMOSTRAS + i) % total];

        if (agora - ultimoEnvio >= 50) {
            ultimoEnvio = agora;
            hz = detectarNota(&vol);
            minhaNota = notaDe(hz);
            u8 pacote[7];
            u16 dhz = (u16)(hz * 10 > 65535 ? 0 : hz * 10);
            memcpy(pacote, "3DSK", 4);
            memcpy(pacote + 4, &dhz, 2);
            pacote[6] = (u8)vol;
            enviarPara(sock, pacote, sizeof(pacote), &dest);
        }

        // respostas: estado da música e pedaços da letra
        static u8 resp[1200];
        int n;
        while ((n = receberDe(sock, resp, sizeof(resp) - 1)) >= 4) {
            if (!memcmp(resp, "3DSK", 4) && n >= 4 + 16) {
                u8 *p = resp + 4;
                pc.tocando = p[0];
                memcpy(&pc.posMs, p + 1, 4);
                memcpy(&pc.id, p + 5, 4);
                pc.notaAlvo = p[9] == 255 ? -1 : p[9];
                pc.acerto = p[10];
                memcpy(&pc.pontos, p + 11, 4);
                pc.estadoLetra = p[15];
                resp[n] = 0;
                char *nome = (char *)p + 16, *quebra = strchr(nome, '\n');
                if (quebra) *quebra = 0;
                snprintf(pc.titulo, sizeof(pc.titulo), "%.95s", nome);
                snprintf(pc.artista, sizeof(pc.artista), "%.63s", quebra ? quebra + 1 : "");
                pc.quando = agora;
                ultimaResposta = agora;
                if (pc.id != letraId) {                          // música nova: letra nova
                    letraId = pc.id;
                    letraPronta = false;
                    letraTam = 0;
                    partesTotal = proximaParte = 0;
                    nLinhas = 0;
                }
            } else if (!memcmp(resp, "3DSL", 4) && n >= 12) {
                u32 id;
                u16 parte, totalPartes;
                memcpy(&id, resp + 4, 4);
                memcpy(&parte, resp + 8, 2);
                memcpy(&totalPartes, resp + 10, 2);
                if (id == letraId && parte == proximaParte && !letraPronta) {
                    int tam = n - 12;
                    if (letraTam + tam > MAX_LETRA) tam = MAX_LETRA - letraTam;
                    memcpy(letraBruta + letraTam, resp + 12, tam);
                    letraTam += tam;
                    partesTotal = totalPartes;
                    proximaParte++;
                    ultimoPedidoLetra = 0;                       // pede a próxima já
                    if (proximaParte >= partesTotal) montarLetra();
                }
            }
        }
        // pede a letra (pedaço por pedaço; repete se um pacote se perder)
        if (pc.estadoLetra == 1 && letraId && !letraPronta && agora - ultimoPedidoLetra > 400) {
            ultimoPedidoLetra = agora;
            u8 pedido[10];
            u16 parte = proximaParte;
            memcpy(pedido, "3DSL", 4);
            memcpy(pedido + 4, &letraId, 4);
            memcpy(pedido + 8, &parte, 2);
            enviarPara(sock, pedido, sizeof(pedido), &dest);
        }

        bool conectado = ultimaResposta && agora - ultimaResposta < 2000;
        uiQuadro();
        desenharCima(conectado);
        desenharBaixo(minhaNota, vol, hz > 0);
        uiFim();
    }

    MICU_StopSampling();
    micExit();
    free(micBuf);
    fecharRede(sock);
}
