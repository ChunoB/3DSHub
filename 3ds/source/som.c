// Som — o áudio do PC tocando no 3DS (usado pela Steam).
//
// Enquanto ligado, manda "3DSU" para o PC (UDP 5013) a cada segundo e recebe o som
// em pacotes de 512 amostras IMA ADPCM (22050 Hz, mono; ver pc/som.py). Os pacotes
// viram PCM e entram num anel; uma thread de áudio (como a do Sintetizador) tira do
// anel para os buffers do NDSP. Antes de começar a tocar o anel junta ~200 ms, para
// o Wi-Fi poder atrasar um pouco sem picotar; passando de ~500 ms, joga fora o
// excesso, para o atraso não crescer.
//
// Precisa do sdmc:/3ds/dspfirm.cdc, como o Sintetizador.

#include "comum.h"
#include "som.h"

#include <string.h>

#define PORTA        5013
#define TAXA         22050
#define AMOSTRAS_BUF 512                // por buffer do NDSP (23 ms)
#define N_BUFFERS    6
#define ANEL         (TAXA)             // 1 s
#define ALVO         (TAXA / 5)         // junta 200 ms antes de tocar
#define MAXIMO       (TAXA / 2)         // mais que 500 ms guardados: descarta

static const s8 tabIndice[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const s16 tabPasso[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

static s16 anel[ANEL];
static u32 inicio, quantas;             // posição de leitura e amostras guardadas
static bool juntando = true;            // esperando o anel encher antes de tocar
static LightLock trava;

static ndspWaveBuf wbuf[N_BUFFERS];
static s16 *memAudio;
static LightEvent evento;
static Thread thread;
static volatile bool rodando;

static int sock = -1;
static struct sockaddr_in destino;
static u64 ultimoOi, ultimoPacote;

// ---------------------------------------------------------------- ADPCM
// Igual ao audioop.adpcm2lin do Python (conferido amostra por amostra).
static int decodificar(const u8 *dados, int n, int previsao, int indice, s16 *saida) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        for (int metade = 0; metade < 2; metade++) {
            int nib = metade ? dados[i] & 15 : dados[i] >> 4;
            int passo = tabPasso[indice];
            int dif = passo >> 3;
            if (nib & 4) dif += passo;
            if (nib & 2) dif += passo >> 1;
            if (nib & 1) dif += passo >> 2;
            previsao += nib & 8 ? -dif : dif;
            if (previsao > 32767) previsao = 32767;
            if (previsao < -32768) previsao = -32768;
            indice += tabIndice[nib];
            if (indice < 0) indice = 0;
            if (indice > 88) indice = 88;
            saida[k++] = previsao;
        }
    }
    return k;
}

static void guardar(const s16 *amostras, int n) {
    LightLock_Lock(&trava);
    for (int i = 0; i < n; i++) {
        if (quantas == ANEL) {          // cheio: perde a mais antiga
            inicio = (inicio + 1) % ANEL;
            quantas--;
        }
        anel[(inicio + quantas) % ANEL] = amostras[i];
        quantas++;
    }
    if (quantas > MAXIMO) {             // atrasou demais: pula para perto do agora
        inicio = (inicio + quantas - ALVO) % ANEL;
        quantas = ALVO;
    }
    LightLock_Unlock(&trava);
}

// ---------------------------------------------------------------- thread de áudio

static void preencher(s16 *saida) {
    LightLock_Lock(&trava);
    if (juntando && quantas >= ALVO) juntando = false;
    if (!juntando && quantas >= AMOSTRAS_BUF) {
        for (int i = 0; i < AMOSTRAS_BUF; i++) saida[i] = anel[(inicio + i) % ANEL];
        inicio = (inicio + AMOSTRAS_BUF) % ANEL;
        quantas -= AMOSTRAS_BUF;
    } else {
        memset(saida, 0, AMOSTRAS_BUF * sizeof(s16));   // faltou som: silêncio e junta de novo
        juntando = true;
    }
    LightLock_Unlock(&trava);
}

static void aoTerminarBuffer(void *dados) {
    (void)dados;
    LightEvent_Signal(&evento);
}

static void threadAudio(void *arg) {
    (void)arg;
    while (rodando) {
        for (int i = 0; i < N_BUFFERS; i++) {
            if (wbuf[i].status != NDSP_WBUF_DONE && wbuf[i].status != NDSP_WBUF_FREE) continue;
            preencher(wbuf[i].data_pcm16);
            DSP_FlushDataCache(wbuf[i].data_pcm16, AMOSTRAS_BUF * sizeof(s16));
            ndspChnWaveBufAdd(0, &wbuf[i]);
        }
        LightEvent_Wait(&evento);
    }
}

// ---------------------------------------------------------------- módulo

bool somIniciar(void) {
    if (rodando) return true;
    if (R_FAILED(ndspInit())) return false;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, TAXA);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
    float mix[12] = { 1.0f, 1.0f };
    ndspChnSetMix(0, mix);

    memAudio = linearAlloc(N_BUFFERS * AMOSTRAS_BUF * sizeof(s16));
    if (!memAudio) {
        ndspExit();
        return false;
    }
    memset(memAudio, 0, N_BUFFERS * AMOSTRAS_BUF * sizeof(s16));
    memset(wbuf, 0, sizeof(wbuf));
    for (int i = 0; i < N_BUFFERS; i++) {
        wbuf[i].data_vaddr = memAudio + i * AMOSTRAS_BUF;
        wbuf[i].nsamples   = AMOSTRAS_BUF;
        wbuf[i].status     = NDSP_WBUF_DONE;
    }
    LightLock_Init(&trava);
    inicio = quantas = 0;
    juntando = true;

    sock = abrirUDP(PORTA, &destino);
    ultimoOi = ultimoPacote = 0;

    LightEvent_Init(&evento, RESET_ONESHOT);
    ndspSetCallback(aoTerminarBuffer, NULL);
    rodando = true;
    s32 prio;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    thread = threadCreate(threadAudio, NULL, 16 * 1024, prio - 1, -2, false);
    if (!thread) {
        somEncerrar();
        return false;
    }
    return true;
}

void somEncerrar(void) {
    if (!memAudio) return;
    rodando = false;
    LightEvent_Signal(&evento);
    if (thread) {
        threadJoin(thread, U64_MAX);
        threadFree(thread);
        thread = NULL;
    }
    ndspSetCallback(NULL, NULL);
    ndspChnWaveBufClear(0);
    ndspExit();
    linearFree(memAudio);
    memAudio = NULL;
    if (sock >= 0) fecharRede(sock);
    sock = -1;
}

bool somLigado(void) { return rodando; }

bool somChegando(void) { return rodando && ultimoPacote && osGetTime() - ultimoPacote < 1000; }

void somAtualizar(void) {
    if (!rodando || sock < 0) return;
    u64 agora = osGetTime();
    if (agora - ultimoOi >= 1000) {     // "ainda quero som"
        ultimoOi = agora;
        enviarPara(sock, "3DSU", 4, &destino);
    }
    static u8 pacote[1024];
    static s16 pcm[1024];
    int n;
    while ((n = receberDe(sock, pacote, sizeof(pacote))) >= 12) {
        if (memcmp(pacote, "3DSS", 4)) continue;
        s16 previsao;
        memcpy(&previsao, pacote + 8, 2);
        int indice = pacote[10] > 88 ? 88 : pacote[10];
        int bytes = n - 12 > 512 ? 512 : n - 12;
        guardar(pcm, decodificar(pacote + 12, bytes, previsao, indice, pcm));
        ultimoPacote = agora;
    }
}
