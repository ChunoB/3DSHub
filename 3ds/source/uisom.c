// Sons da interface: o "tique" ao passar pelos ícones, abrir, voltar e erro, no estilo
// dos sons do sistema do 3DS. São sintetizados aqui (sem arquivos) e tocados pelo NDSP,
// que precisa do sdmc:/3ds/dspfirm.cdc, como o Sintetizador; sem ele, o app fica mudo
// e nada mais muda.
//
// Só tocam no menu e nos Ajustes (uiSomPermitir). Os módulos com som próprio (Segunda
// Tela, Steam, Sintetizador) ligam e desligam o NDSP por conta deles, então o som da
// interface é desligado de vez antes de qualquer módulo abrir.
//
// Com o NDSP ligado, deixar o libctru suspender o app (HOME) ou dormir (tampa) pode
// travar o console (ver som.h). Então, enquanto o som da interface está vivo:
//   - o HOME é tratado aqui (uiSomQuadro, chamado a cada quadro pelo uiQuadro): desliga
//     o som, vai para o HOME e religa no próximo som;
//   - o 3DS não dorme; e depois de um tempo sem som nenhum o NDSP é desligado e o
//     3DS volta a poder dormir (se a tampa já estava fechada, dorme na hora).

#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TAXA      22050
#define CANAL     8                     // longe do canal 0 (som.c e o Sintetizador)
#define OCIOSO_MS 45000                 // sem som por esse tempo: desliga o NDSP
#define ARQ_SOM   "sdmc:/3ds/3dshub_som.cfg"

// Cada som é um "tac": um estalo de ruído bem curto (a batida) e um tom que desliza de
// f0 a f1 e some em poucos milésimos (o corpo). Nada de apito comprido.
typedef struct {
    const char *nome;   // sdmc:/3ds/3dshub_sons/<nome>.wav troca o som por um arquivo seu
    float f0, f1;       // frequência do corpo no começo e no fim (Hz)
    float duracao;      // segundos
    float queda;        // quão rápido o volume cai (maior = mais seco)
    float volume;       // 0 a 1
    float harmonico;    // um pouco da oitava de cima
    float ruido;        // força do estalo
    int   pulsos;       // 1 = um tac; 2 = "ta-tac"
} Receita;

static const Receita receitas[N_SONS] = {
    [SOM_MOVER]  = { "mover",   900,  640, 0.030f, 170, 0.55f, 0.25f, 0.55f, 1 },
    [SOM_TIQUE]  = { "tique",  1250,  950, 0.022f, 230, 0.42f, 0.20f, 0.45f, 1 },
    [SOM_ABRIR]  = { "abrir",   820, 1240, 0.085f, 105, 0.55f, 0.25f, 0.45f, 2 },
    [SOM_VOLTAR] = { "voltar",  680,  400, 0.050f, 110, 0.55f, 0.25f, 0.45f, 1 },
    [SOM_ERRO]   = { "erro",    300,  250, 0.150f,  42, 0.55f, 0.50f, 0.25f, 2 },
};
#define PASTA_SONS "sdmc:/3ds/3dshub_sons"
#define MAX_WAV    (TAXA_MAX * 2 * 2)   // até 2 s em estéreo
#define TAXA_MAX   48000

static s16 *dados[N_SONS];              // cada som na sua memória linear
static u32 taxaDe[N_SONS];
static bool estereo[N_SONS];
static bool gerado;
static ndspWaveBuf ondas[N_SONS];
static bool permitido;                  // estamos no menu ou nos Ajustes
static bool vivo;                       // o NDSP está ligado por nós
static bool semDsp;                     // o NDSP não ligou (falta o dspfirm.cdc): não insiste
static bool querSom = true;             // a opção dos Ajustes
static bool leuOpcao, podiaDormir;
static u64 ultimoUso;

static void lerOpcao(void) {
    if (leuOpcao) return;
    leuOpcao = true;
    FILE *f = fopen(ARQ_SOM, "r");
    if (!f) return;
    int v = 1;
    if (fscanf(f, "%d", &v) == 1) querSom = v != 0;
    fclose(f);
}

// Um .wav seu no lugar do som sintetizado (PCM de 16 bits, mono ou estéreo, até 2 s).
// É assim que dá para usar os sons originais do sistema: extraia do seu console e
// ponha em PASTA_SONS com os nomes mover.wav, tique.wav, abrir.wav, voltar.wav, erro.wav.
static bool carregarWav(int i) {
    char caminho[80];
    snprintf(caminho, sizeof(caminho), PASTA_SONS "/%s.wav", receitas[i].nome);
    FILE *f = fopen(caminho, "rb");
    if (!f) return false;
    u8 cab[12];
    bool ok = fread(cab, 1, 12, f) == 12 && !memcmp(cab, "RIFF", 4) && !memcmp(cab + 8, "WAVE", 4);
    u16 canais = 0, bits = 0, formato = 0;
    u32 taxa = 0;
    while (ok) {
        u8 bloco[8];
        u32 tam;
        if (fread(bloco, 1, 8, f) != 8) { ok = false; break; }
        memcpy(&tam, bloco + 4, 4);
        if (!memcmp(bloco, "fmt ", 4)) {
            u8 fmt[16];
            if (tam < 16 || fread(fmt, 1, 16, f) != 16) { ok = false; break; }
            memcpy(&formato, fmt, 2);
            memcpy(&canais, fmt + 2, 2);
            memcpy(&taxa, fmt + 4, 4);
            memcpy(&bits, fmt + 14, 2);
            fseek(f, (tam - 16) + (tam & 1), SEEK_CUR);
        } else if (!memcmp(bloco, "data", 4)) {
            ok = formato == 1 && bits == 16 && (canais == 1 || canais == 2) && taxa >= 8000 && taxa <= TAXA_MAX;
            if (!ok) break;
            if (tam > MAX_WAV * sizeof(s16)) tam = MAX_WAV * sizeof(s16);
            tam -= tam % (canais * 2);
            dados[i] = linearAlloc(tam);
            ok = dados[i] && tam && fread(dados[i], 1, tam, f) == tam;
            if (ok) {
                taxaDe[i] = taxa;
                estereo[i] = canais == 2;
                ondas[i].data_vaddr = dados[i];
                ondas[i].nsamples = tam / (canais * 2);
                DSP_FlushDataCache(dados[i], tam);
            } else if (dados[i]) {
                linearFree(dados[i]);
                dados[i] = NULL;
            }
            break;
        } else {
            fseek(f, tam + (tam & 1), SEEK_CUR);
        }
    }
    fclose(f);
    return ok && dados[i];
}

static bool sintetizar(int i) {
    const Receita *r = &receitas[i];
    int n = (int)(r->duracao * TAXA);
    dados[i] = linearAlloc(n * sizeof(s16));
    if (!dados[i]) return false;
    float fase = 0, pulso = r->duracao / r->pulsos;
    u32 semente = 12345 + i * 977;
    for (int k = 0; k < n; k++) {
        float t = (float)k / TAXA;
        float tp = fmodf(t, pulso);                            // tempo dentro do tac
        fase += (r->f0 + (r->f1 - r->f0) * (t / r->duracao)) / TAXA;
        float ataque = tp < 0.001f ? tp / 0.001f : 1;          // 1 ms para entrar
        float fim = (n - k) < 44 ? (n - k) / 44.0f : 1;        // e 2 ms para sair, sem estalo extra
        float corpo = sinf(2 * (float)M_PI * fase) + r->harmonico * sinf(4 * (float)M_PI * fase);
        semente = semente * 1664525u + 1013904223u;
        float chiado = ((semente >> 16) & 0x7FFF) / 16384.0f - 1;
        float v = corpo / (1 + r->harmonico) * expf(-tp * r->queda) + chiado * r->ruido * expf(-tp * 900);
        v *= r->volume * ataque * fim;
        dados[i][k] = (s16)((v > 1 ? 1 : v < -1 ? -1 : v) * 30000);
    }
    taxaDe[i] = TAXA;
    estereo[i] = false;
    ondas[i].data_vaddr = dados[i];
    ondas[i].nsamples = n;
    DSP_FlushDataCache(dados[i], n * sizeof(s16));
    return true;
}

// Prepara os sons uma vez (o .wav do cartão, se existir; senão o sintetizado).
static bool gerar(void) {
    if (gerado) return true;
    memset(ondas, 0, sizeof(ondas));
    for (int i = 0; i < N_SONS; i++)
        if (!carregarWav(i) && !sintetizar(i)) return false;
    gerado = true;
    return true;
}

static bool ligar(void) {
    if (vivo) return true;
    if (semDsp) return false;
    if (R_FAILED(ndspInit())) {
        semDsp = true;
        return false;
    }
    if (!gerar()) {
        ndspExit();
        semDsp = true;
        return false;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(CANAL);
    ndspChnSetInterp(CANAL, NDSP_INTERP_LINEAR);
    float mix[12] = { 0.7f, 0.7f };
    ndspChnSetMix(CANAL, mix);

    aptSetHomeAllowed(false);           // o HOME passa por uiSomQuadro
    podiaDormir = aptIsSleepAllowed();
    aptSetSleepAllowed(false);
    vivo = true;
    ultimoUso = osGetTime();
    return true;
}

static void desligar(void) {
    if (!vivo) return;
    ndspChnWaveBufClear(CANAL);
    ndspExit();
    vivo = false;
    aptSetHomeAllowed(true);
    aptSetSleepAllowed(podiaDormir);    // tampa fechada nesse meio-tempo: dorme agora
}

void uiSomPermitir(bool sim) {
    lerOpcao();
    permitido = sim;
    if (!sim) desligar();
    else if (querSom) ligar();          // já liga: a espera do NDSP fica na entrada do menu
}

void uiSomPausar(void) { desligar(); }

void uiSomFim(void) {
    permitido = false;
    desligar();
    for (int i = 0; i < N_SONS; i++) {
        if (dados[i]) linearFree(dados[i]);
        dados[i] = NULL;
    }
    gerado = false;
}

void uiSomTocar(int som) {
    if (!permitido || !querSom || som < 0 || som >= N_SONS || !ligar()) return;
    ndspChnWaveBufClear(CANAL);         // um som por vez: o novo corta o anterior
    ndspChnSetRate(CANAL, taxaDe[som]);
    ndspChnSetFormat(CANAL, estereo[som] ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    ondas[som].status = NDSP_WBUF_FREE;
    ndspChnWaveBufAdd(CANAL, &ondas[som]);
    ultimoUso = osGetTime();
}

void uiSomQuadro(void) {
    if (!vivo) return;
    if (aptCheckHomePressRejected()) {
        desligar();
        aptJumpToHomeMenu();            // volta daqui quando o app é retomado (ou está fechando)
        return;
    }
    if (osGetTime() - ultimoUso > OCIOSO_MS) desligar();
}

bool uiSomQuer(void) {
    lerOpcao();
    return querSom;
}

bool uiSomFunciona(void) { return !semDsp; }

void uiSomDefinir(bool ligado) {
    lerOpcao();
    querSom = ligado;
    semDsp = false;                     // ligou de novo: tenta o NDSP outra vez
    FILE *f = fopen(ARQ_SOM, "w");
    if (f) {
        fprintf(f, "%d\n", ligado ? 1 : 0);
        fclose(f);
    }
    if (!ligado) desligar();
    else if (permitido) ligar();
}
