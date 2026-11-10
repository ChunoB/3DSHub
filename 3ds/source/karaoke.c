// Karaokê — a música toca no PC, a letra rola aqui e as notas vêm por uma faixa, como no
// Guitar Hero. Cada nota cantada vale Perfeito / Ótimo / Bom / Errou.
//
//   menu         : escolha quem canta (1 a 4 perfis, ou Convidado) e toque em Cantar
//   tela de cima : a letra, pontos, combo e de quem é a vez
//   tela de baixo: a faixa: as 12 notas (Dó embaixo, Si em cima; oitava não importa), as
//                  notas da música vindo até a linha e a sua voz (amarela)
//   A            : Turbo (quando a barra está cheia: pontos em dobro por 10 s)
//   X            : recomeçar a música     R: pular para a próxima
//   SELECT       : encerrar a vez         START: voltar ao menu (no menu: sair)
//
// Na primeira vez que uma música toca, a faixa mostra os trechos com letra e as notas
// aparecem quando soam (o PC as grava). Da segunda vez em diante, as notas gravadas vêm
// vindo antes de tocar. Acabou a vez: o resultado, e o próximo jogador canta a mesma música.
//
// O 3DS descobre a nota que você canta (YIN, uma autocorrelação, no próprio 3DS) e manda
// só ela; o PC (pc/karaoke.py, UDP 5016) julga, guarda perfis e recordes, controla a
// música e manda a letra em pedaços. Se a música pausar no PC, tudo para aqui também.

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

// faixa (igual ao PC)
#define N_JANELA    48
#define ANTES       12
#define SLOT_MS     100
#define CANTO       252
#define NAO_GRAVADO 253
#define SEM_NOTA    255
#define INSTRUMENTAL 100                // nota + 100: fora dos trechos com letra (não vale)

#define MAX_PERFIS  12
#define MAX_JOG     4
#define COR_CONVIDADO 7

static const char *nomesNotas[12] = { "Dó", "Dó#", "Ré", "Ré#", "Mi", "Fá", "Fá#", "Sol", "Sol#", "Lá", "Lá#", "Si" };
static const u32 coresPerfil[8] = { COR_KARAOKE, COR_AZUL, COR_VERDE_AGUA, COR_AMARELO, COR_CORAL, COR_ROXO,
                                    COR_STEAM, COR_MUDO };
static const char *nomesJulg[5] = { "", "PERFEITO!", "ÓTIMO!", "BOM", "ERROU" };
static const u32 coresJulg[5] = { 0, COR_VERDE, COR_AZUL_CL, COR_AMARELO, COR_VERMELHO };

static u32 corPerfil(int c) { return coresPerfil[(c < 0 || c > 7) ? 7 : c]; }

// ---------------------------------------------------------------- estado vindo do PC

static struct {
    bool tocando, gravada;
    u32 posMs, id, pontos;
    int estadoLetra;                    // 0 buscando, 1 achou, 2 sem letra
    int alvo;                           // nota da música agora (-1 = nenhuma)
    int julgSeq, julg;
    int combo, mult, turbo, turboS;
    int rodada;                         // 0 sem rodada, 1 cantando, 2 fim da vez, 3 fim da rodada
    int vez, nJog, acerto, progresso;
    u8 janela[N_JANELA];
    int janelaBase;                     // número da primeira casa da janela (pode ser < 0)
    char titulo[96], artista[64], jogador[24];
    u64 quando;                         // quando chegou (para andar a posição entre respostas)
} jg;

static bool pcAntigo;                   // o PC respondeu no formato antigo (pasta pc desatualizada)
static u64 ultimaResposta;

// letra montada a partir dos pedaços
static char letraBruta[MAX_LETRA + 1];
static int letraTam;
static u32 letraId;
static int partesTotal, proximaParte;
static bool letraPronta;
static struct { u32 ms; const char *texto; } linhas[MAX_LINHAS];
static int nLinhas;
static u64 ultimoPedidoLetra;

// perfis e rodada
typedef struct { int id; char nome[20]; int cor, nivel, pct, partidas; } Perfil;
static Perfil perfis[MAX_PERFIS];
static int nPerfis;
static int escolhidos[MAX_JOG];         // ids na ordem de quem canta (0 = Convidado)
static int dificuldade = 1;             // 0 Fácil, 1 Normal, 2 Difícil (fica até fechar o app)
static const char *nomesDif[3] = { "Fácil", "Normal", "Difícil" };
static const u32 coresDif[3] = { COR_VERDE, COR_AZUL, COR_VERMELHO };
static int nEscolhidos;

static struct {
    u32 pontos;
    int precisao, estrelas, comboMax, cont[5], nivel, xp;
    bool recorde, recordeGeral, subiu, temPerfil;
} res;
static struct { char nome[20]; int cor; u32 pontos; int estrelas; } ranking[MAX_JOG];
static int nRanking;
static char proximoNome[20];
static struct { char nome[20]; u32 pontos; int estrelas; char dif; } tops[5];
static int nTops;
static u32 topsId = 0xFFFFFFFF;

// rede
static int sock;
static struct sockaddr_in dest;
static u8 seqCmd;
static int respSeq = -1;
static char resp[1200];

// voz
static struct { u32 ms; float st; } voz[96];   // rastro da voz (semitom 0-12; < 0 = calado)
static int vozIni, vozN;
static float posSuave;                          // posição da música andando suave entre pacotes
static float stDesenho = -1;                    // semitom do rastro, suavizado (só para desenhar)
static u64 ultimoQuadro;
static int popupJulg, ultimoJulgSeq = -1;
static u64 popupDesde;

// ---------------------------------------------------------------- nota da voz (YIN)

static float amostras[N_AMOSTRAS];
static float difs[TAU_MAX + 1];

static float piso;                      // nível do barulho de fundo (sobe devagar, desce na hora)

// Frequência em Hz (0 = sem nota clara). vol = 0 a 100.
// Testado com vozes simuladas (vibrato, chiado, a música vazando no microfone): o piso de ruído
// para de inventar nota quando você está calado e o "vale mais fundo" acerta mais a voz grave.
static float detectarNota(float *vol) {
    float media = 0;
    for (int i = 0; i < N_AMOSTRAS; i++) media += amostras[i];
    media /= N_AMOSTRAS;
    float soma = 0;
    for (int i = 0; i < N_AMOSTRAS; i++) {
        amostras[i] -= media;                                   // o microfone tem um desvio fixo (DC)
        soma += amostras[i] * amostras[i];
    }
    float rms = sqrtf(soma / N_AMOSTRAS);
    *vol = rms / 32768 * 700;
    if (*vol > 100) *vol = 100;
    if (rms > piso) piso += (rms - piso) * 0.002f;              // a música vazando no microfone sobe o piso
    else piso = rms;                                            // (nas pausas ele volta na hora)
    if (rms < 250 || rms < piso * 1.8f) return 0;               // silêncio, barulho ou só a música

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
    int achou = -1, fundo = TAU_MIN;
    for (int tau = TAU_MIN; tau < TAU_MAX; tau++) {
        if (difs[tau] < difs[fundo]) fundo = tau;
        if (difs[tau] < 0.15f) {
            while (tau + 1 < TAU_MAX && difs[tau + 1] < difs[tau]) tau++;   // desce até o fundo do vale
            achou = tau;
            break;
        }
    }
    if (achou < 0 && difs[fundo] < 0.3f) achou = fundo;        // nenhum vale bem fundo: o melhor, se razoável
    if (achou < 0) return 0;
    float a = difs[achou - 1], b = difs[achou], c = difs[achou + 1];     // ajuste fino (parábola)
    float desloc = (a - 2 * b + c) != 0 ? 0.5f * (a - c) / (a - 2 * b + c) : 0;
    return MIC_TAXA / (achou + desloc);
}

// semitom contínuo dentro da oitava (0 = Dó ... 11,99), ou -1
static float semitom(float hz) {
    if (hz <= 0) return -1;
    float st = fmodf(12 * log2f(hz / 440.0f) + 69, 12);
    return st < 0 ? st + 12 : st;
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

static void novaMusica(u32 id) {
    letraId = id;
    letraPronta = false;
    letraTam = 0;
    partesTotal = proximaParte = 0;
    nLinhas = 0;
    vozN = 0;
}

// ---------------------------------------------------------------- rede

static void lerTexto(char *nome, char *dst1, int n1, char *dst2, int n2, char *dst3, int n3) {
    char *q1 = strchr(nome, '\n');
    if (q1) *q1++ = 0;
    char *q2 = q1 ? strchr(q1, '\n') : NULL;
    if (q2) *q2++ = 0;
    snprintf(dst1, n1, "%s", nome);
    snprintf(dst2, n2, "%s", q1 ? q1 : "");
    if (dst3) snprintf(dst3, n3, "%s", q2 ? q2 : "");
}

static void receberPacotes(void) {
    static u8 buf[1300];
    int n;
    u64 agora = osGetTime();
    while ((n = receberDe(sock, buf, sizeof(buf) - 1)) >= 4) {
        buf[n] = 0;
        if (!memcmp(buf, "3DSG", 4) && n >= 4 + 28 + N_JANELA) {
            u8 *p = buf + 4;
            jg.tocando = p[0];
            memcpy(&jg.posMs, p + 1, 4);
            u32 id;
            memcpy(&id, p + 5, 4);
            jg.estadoLetra = p[9];
            jg.gravada = p[10];
            jg.alvo = p[11] == 255 ? -1 : p[11];
            jg.julgSeq = p[12];
            jg.julg = p[13];
            memcpy(&jg.pontos, p + 14, 4);
            u16 combo;
            memcpy(&combo, p + 18, 2);
            jg.combo = combo;
            jg.mult = p[20];
            jg.turbo = p[21];
            jg.turboS = p[22];
            jg.rodada = p[23];
            jg.vez = p[24];
            jg.nJog = p[25];
            jg.acerto = p[26];
            jg.progresso = p[27];
            memcpy(jg.janela, p + 28, N_JANELA);
            jg.janelaBase = (int)(jg.posMs / SLOT_MS) - ANTES;
            lerTexto((char *)p + 28 + N_JANELA, jg.titulo, sizeof(jg.titulo), jg.artista, sizeof(jg.artista),
                     jg.jogador, sizeof(jg.jogador));
            if (id != jg.id) novaMusica(id);
            jg.id = id;
            jg.quando = agora;
            ultimaResposta = agora;
            pcAntigo = false;
        } else if (!memcmp(buf, "3DSK", 4) && n >= 4 + 16) {     // PC com a pasta pc antiga
            u8 *p = buf + 4;
            jg.tocando = p[0];
            memcpy(&jg.posMs, p + 1, 4);
            u32 id;
            memcpy(&id, p + 5, 4);
            jg.alvo = p[9] == 255 ? -1 : p[9];
            jg.acerto = p[10];
            memcpy(&jg.pontos, p + 11, 4);
            jg.estadoLetra = p[15];
            jg.rodada = 0;
            memset(jg.janela, NAO_GRAVADO, N_JANELA);
            lerTexto((char *)p + 16, jg.titulo, sizeof(jg.titulo), jg.artista, sizeof(jg.artista), NULL, 0);
            jg.jogador[0] = 0;
            if (id != jg.id) novaMusica(id);
            jg.id = id;
            jg.quando = agora;
            ultimaResposta = agora;
            pcAntigo = true;
        } else if (!memcmp(buf, "3DSL", 4) && n >= 12) {
            u32 id;
            u16 parte, totalPartes;
            memcpy(&id, buf + 4, 4);
            memcpy(&parte, buf + 8, 2);
            memcpy(&totalPartes, buf + 10, 2);
            if (id == letraId && parte == proximaParte && !letraPronta) {
                int tam = n - 12;
                if (letraTam + tam > MAX_LETRA) tam = MAX_LETRA - letraTam;
                memcpy(letraBruta + letraTam, buf + 12, tam);
                letraTam += tam;
                partesTotal = totalPartes;
                proximaParte++;
                ultimoPedidoLetra = 0;                       // pede a próxima já
                if (proximaParte >= partesTotal) montarLetra();
            }
        } else if (!memcmp(buf, "3DSP", 4) && n >= 6) {
            int tam = n - 6 < (int)sizeof(resp) - 1 ? n - 6 : (int)sizeof(resp) - 1;
            memcpy(resp, buf + 6, tam);
            resp[tam] = 0;
            respSeq = buf[4];
        }
    }
    // pede a letra (pedaço por pedaço; repete se um pacote se perder)
    if (jg.estadoLetra == 1 && letraId && !letraPronta && agora - ultimoPedidoLetra > 400) {
        ultimoPedidoLetra = agora;
        u8 pedido[10];
        u16 parte = proximaParte;
        memcpy(pedido, "3DSL", 4);
        memcpy(pedido + 4, &letraId, 4);
        memcpy(pedido + 8, &parte, 2);
        enviarPara(sock, pedido, sizeof(pedido), &dest);
    }
}

// Manda um comando de perfis/rodada e espera a resposta (em 'resp'). false = o PC não respondeu.
static bool pedir(char cmd, const void *dados, int n) {
    u8 pac[64];
    seqCmd++;
    memcpy(pac, "3DSP", 4);
    pac[4] = seqCmd;
    pac[5] = (u8)cmd;
    if (n > (int)sizeof(pac) - 6) n = sizeof(pac) - 6;
    if (n > 0) memcpy(pac + 6, dados, n);
    respSeq = -1;
    u64 inicio = osGetTime(), ultimo = 0;
    while (osGetTime() - inicio < 2000 && aptMainLoop()) {
        if (osGetTime() - ultimo >= 300) {
            enviarPara(sock, pac, 6 + n, &dest);
            ultimo = osGetTime();
        }
        receberPacotes();
        if (respSeq == seqCmd) return true;
        svcSleepThread(10 * 1000000LL);
    }
    return false;
}

// separa uma linha por tabs (altera a linha)
static int campos(char *linha, char **f, int max) {
    int n = 0;
    while (linha && n < max) {
        f[n++] = linha;
        char *tab = strchr(linha, '\t');
        if (tab) *tab++ = 0;
        linha = tab;
    }
    return n;
}

static void lerPerfis(void) {
    nPerfis = 0;
    char *salvo;
    for (char *l = strtok_r(resp, "\n", &salvo); l && nPerfis < MAX_PERFIS; l = strtok_r(NULL, "\n", &salvo)) {
        char *f[6];
        if (campos(l, f, 6) < 6) continue;
        Perfil *p = &perfis[nPerfis++];
        p->id = atoi(f[0]);
        snprintf(p->nome, sizeof(p->nome), "%s", f[1]);
        p->cor = atoi(f[2]);
        p->nivel = atoi(f[3]);
        p->pct = atoi(f[4]);
        p->partidas = atoi(f[5]);
    }
    // tira dos escolhidos quem não existe mais
    int k = 0;
    for (int i = 0; i < nEscolhidos; i++) {
        bool existe = escolhidos[i] == 0;
        for (int j = 0; j < nPerfis; j++) existe |= perfis[j].id == escolhidos[i];
        if (existe) escolhidos[k++] = escolhidos[i];
    }
    nEscolhidos = k;
}

static void lerTops(char *texto) {
    nTops = 0;
    char *salvo;
    for (char *l = strtok_r(texto, "\n", &salvo); l; l = strtok_r(NULL, "\n", &salvo)) {
        char *f[5];
        int nf = l[0] == 'T' ? campos(l, f, 5) : 0;
        if (nf < 4 || nTops >= 5) continue;
        snprintf(tops[nTops].nome, sizeof(tops[nTops].nome), "%s", f[1]);
        tops[nTops].pontos = strtoul(f[2], NULL, 10);
        tops[nTops].estrelas = atoi(f[3]);
        tops[nTops].dif = nf >= 5 ? f[4][0] : 'N';
        nTops++;
    }
}

static bool pedirResultado(void) {
    if (!pedir('R', NULL, 0) || !resp[0]) return false;
    static char copia[1200];
    strcpy(copia, resp);
    nRanking = 0;
    proximoNome[0] = 0;
    char *salvo;
    char *l = strtok_r(copia, "\n", &salvo);
    char *f[14];
    if (!l || campos(l, f, 14) < 14) return false;
    res.pontos = strtoul(f[0], NULL, 10);
    res.precisao = atoi(f[1]);
    res.estrelas = atoi(f[2]);
    res.comboMax = atoi(f[3]);
    for (int j = 1; j <= 4; j++) res.cont[j] = atoi(f[3 + j]);
    res.recorde = atoi(f[8]);
    res.recordeGeral = atoi(f[9]);
    res.nivel = atoi(f[10]);
    res.subiu = atoi(f[11]);
    res.xp = atoi(f[12]);
    res.temPerfil = atoi(f[13]);
    static char tudoTops[1200];
    tudoTops[0] = 0;
    for (l = strtok_r(NULL, "\n", &salvo); l; l = strtok_r(NULL, "\n", &salvo)) {
        if (l[0] == 'J' && nRanking < MAX_JOG) {
            char *g[5];
            if (campos(l, g, 5) < 5) continue;
            snprintf(ranking[nRanking].nome, sizeof(ranking[0].nome), "%s", g[1]);
            ranking[nRanking].cor = atoi(g[2]);
            ranking[nRanking].pontos = strtoul(g[3], NULL, 10);
            ranking[nRanking].estrelas = atoi(g[4]);
            nRanking++;
        } else if (l[0] == 'N') {
            char *g[2];
            if (campos(l, g, 2) == 2) snprintf(proximoNome, sizeof(proximoNome), "%s", g[1]);
        } else if (l[0] == 'T') {
            strcat(tudoTops, l);
            strcat(tudoTops, "\n");
        }
    }
    lerTops(tudoTops);
    topsId = jg.id;
    return true;
}

// ---------------------------------------------------------------- desenho: peças

static u32 posicaoAgora(void) {
    if (!jg.tocando) return jg.posMs;
    return jg.posMs + (u32)(osGetTime() - jg.quando);
}

static void estrela(float cx, float cy, float r, u32 cor) {
    float px[10], py[10];
    for (int i = 0; i < 10; i++) {
        float a = -(float)M_PI / 2 + i * (float)M_PI / 5;
        float rr = i % 2 ? r * 0.45f : r;
        px[i] = cx + cosf(a) * rr;
        py[i] = cy + sinf(a) * rr;
    }
    for (int i = 0; i < 10; i++) uiTri(cx, cy, px[i], py[i], px[(i + 1) % 10], py[(i + 1) % 10], cor);
}

static void estrelas(float cx, float cy, float r, int n, float espaco) {
    for (int i = 0; i < 5; i++)
        estrela(cx + (i - 2) * espaco, cy, r, i < n ? COR_AMARELO : COR_CARTAO2);
}

static void chipJogador(float x, float y, const char *nome, int cor, float larguraMax) {
    float w = uiLarguraTextoMax(nome, 13, larguraMax) + 30;
    uiRetArred(x, y, w, 22, 11, corPerfil(cor));
    uiCirculo(x + 11, y + 11, 6, COR_BRANCO);
    uiTexto(x + 22, y + 3, 13, COR_BRANCO, ESQ, larguraMax, "%s", nome);
}

// ---------------------------------------------------------------- tela de baixo: a faixa

#define FX_Y0    28                    // topo da faixa
#define FX_LIN   14                    // altura de cada nota
#define FX_X0    28                    // depois dos nomes das notas
#define FX_LINHA 76                    // a linha de acerto
#define PX_MS    0.075f                // 75 pixels por segundo

static float yNota(int nota) { return FX_Y0 + (11 - nota) * FX_LIN; }

static void desenharFaixa(void) {
    u32 agora = (u32)posSuave;
    bool turbo = jg.turboS > 0;

    // fundo: uma linha por nota (sustenidos mais escuros, como as teclas pretas)
    for (int n = 0; n < 12; n++) {
        bool sust = n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
        uiRet(FX_X0, yNota(n), 320 - FX_X0, FX_LIN, sust ? HEX(0x191C2B) : HEX(0x20253A));
        uiTexto(FX_X0 - 3, yNota(n) + 1, 10, n == jg.alvo ? COR_KARAOKE_CL : COR_MUDO, DIR, 0, "%s", nomesNotas[n]);
    }

    // as casas da janela, juntando as iguais em barras
    int k = 0;
    while (k < N_JANELA) {
        int v = jg.janela[k], fim = k;
        while (fim + 1 < N_JANELA && jg.janela[fim + 1] == v) fim++;
        float x0 = FX_LINHA + ((float)(jg.janelaBase + k) * SLOT_MS - (float)agora) * PX_MS;
        float x1 = FX_LINHA + ((float)(jg.janelaBase + fim + 1) * SLOT_MS - (float)agora) * PX_MS;
        if (x1 > FX_X0 && x0 < 320) {
            if (x0 < FX_X0) x0 = FX_X0;
            if (x1 > 320) x1 = 320;
            if (v < 12) {
                bool naLinha = x0 <= FX_LINHA && x1 >= FX_LINHA;
                u32 cor = naLinha ? (jg.acerto >= 50 ? COR_VERDE : COR_KARAOKE_CL)
                        : x1 < FX_LINHA ? HEX(0x6A2460) : (turbo ? COR_AMARELO : COR_KARAOKE);
                uiRetArred(x0, yNota(v) + 2, x1 - x0, FX_LIN - 4, 4, cor);
            } else if (v >= INSTRUMENTAL && v < INSTRUMENTAL + 12) {   // parte sem letra: não vale
                uiRetArred(x0, yNota(v - INSTRUMENTAL) + 5, x1 - x0, FX_LIN - 10, 2, HEX(0x4A4F66));
            } else if (v == CANTO) {                     // tem canto aqui, nota ainda desconhecida
                uiRetArred(x0, yNota(7) + 3, x1 - x0, FX_LIN * 4 - 6, 6, C2D_Color32(255, 255, 255, 34));
            }
        }
        k = fim + 1;
    }

    // linha de acerto
    uiRet(FX_LINHA - 1, FX_Y0, 3, 12 * FX_LIN, turbo ? COR_AMARELO : C2D_Color32(255, 255, 255, 150));

    // rastro da voz
    float antX = -1, antY = 0, antSt = -1;
    for (int i = 0; i < vozN; i++) {
        int idx = (vozIni + i) % 96;
        float x = FX_LINHA + ((float)voz[idx].ms - (float)agora) * PX_MS;
        float st = voz[idx].st;
        if (x < FX_X0 || x > FX_LINHA + 1 || st < 0) { antX = -1; continue; }
        float y = FX_Y0 + (11.5f - st) * FX_LIN;
        if (antX >= 0 && fabsf(st - antSt) < 2) uiLinha(antX, antY, x, y, 3, COR_AMARELO);
        uiCirculo(x, y, 2, COR_AMARELO);
        antX = x;
        antY = y;
        antSt = st;
    }
    if (vozN) {
        int ult = (vozIni + vozN - 1) % 96;
        if (voz[ult].st >= 0) {
            float y = FX_Y0 + (11.5f - voz[ult].st) * FX_LIN;
            uiCirculo(FX_LINHA, y, 6, jg.acerto >= 100 ? COR_VERDE : jg.acerto >= 50 ? COR_AMARELO : COR_BRANCO);
        }
    }

    // julgamento que acabou de sair
    u64 desde = osGetTime() - popupDesde;
    if (popupJulg >= 1 && popupJulg <= 4 && desde < 750) {
        float y = FX_Y0 + 6 - desde * 0.02f;
        uiTexto(FX_LINHA + 8, y < 2 ? 2 : y, 16, coresJulg[popupJulg], ESQ, 0, "%s", nomesJulg[popupJulg]);
    }
}

static void desenharJogoBaixo(void) {
    uiTela(alvoBaixo);
    if (pcAntigo) {
        uiTexto(160, 4, 11, COR_AMARELO, CENTRO, 312, "Atualize a pasta pc do PC para o jogo completo");
    } else {
        // combo, multiplicador e Turbo
        uiTexto(6, 4, 15, jg.combo ? COR_TEXTO : COR_MUDO, ESQ, 0, "Combo %d", jg.combo);
        uiRetArred(104, 3, 34, 20, 8, jg.mult > 1 ? COR_KARAOKE : COR_CARTAO2);
        uiTexto(121, 5, 14, COR_BRANCO, CENTRO, 0, "x%d", jg.mult * (jg.turboS > 0 ? 2 : 1));
        if (jg.turboS > 0) {
            uiRetArred(146, 4, 168, 18, 8, COR_AMARELO);
            uiTexto(230, 5, 13, COR_ESCURO, CENTRO, 0, "TURBO! %d s", jg.turboS);
        } else {
            uiBarra(146, 6, 120, 14, jg.turbo / 100.0f, jg.turbo >= 100 ? COR_AMARELO : HEX(0x8A6A20), COR_CARTAO2);
            bool pisca = jg.turbo >= 100 && (osGetTime() / 300) % 2;
            uiTexto(272, 6, 12, pisca ? COR_AMARELO : COR_MUDO, ESQ, 0, "%s", jg.turbo >= 100 ? "A: Turbo" : "Turbo");
        }
    }
    desenharFaixa();
    if (!pcAntigo && jg.rodada == 0)
        uiTexto(174, 90, 13, COR_TEXTO, CENTRO, 230, "Sem rodada: START volta ao menu");   // no meio da faixa
    uiDicas(160, 200, CENTRO, "A", "turbo", "X", "recomeçar", "R", "pular música", NULL);
    uiDicas(160, 218, CENTRO, "SELECT", "encerrar a vez", "START", "menu", NULL);
}

// ---------------------------------------------------------------- tela de cima: jogo

// Quebra o texto em até 'max' linhas que cabem em 'largura' (sem cortar palavra; a última
// linha leva o resto). Devolve quantas linhas saíram.
#define TAM_LINHA 168
static int quebrarTexto(const char *texto, float tam, float largura, char saida[][TAM_LINHA], int max) {
    int n = 0;
    const char *p = texto;
    while (*p == ' ') p++;
    while (*p && n < max) {
        if (n == max - 1) {
            snprintf(saida[n++], TAM_LINHA, "%s", p);
            break;
        }
        const char *fim = p, *q = p;
        while (true) {                                   // junta palavras enquanto couber
            const char *esp = strchr(q, ' ');
            const char *prox = esp ? esp : q + strlen(q);
            char tentativa[TAM_LINHA];
            int len = prox - p < TAM_LINHA - 1 ? prox - p : TAM_LINHA - 1;
            memcpy(tentativa, p, len);
            tentativa[len] = 0;
            if (fim != p && uiLarguraTexto(tentativa, tam) > largura) break;
            fim = prox;
            if (!esp) break;
            q = esp + 1;
        }
        int len = fim - p < TAM_LINHA - 1 ? fim - p : TAM_LINHA - 1;
        memcpy(saida[n], p, len);
        saida[n++][len] = 0;
        p = fim;
        while (*p == ' ') p++;
    }
    return n;
}

static const char *textoLinha(int i) { return linhas[i].texto[0] ? linhas[i].texto : "..."; }

static void desenharLetra(float yMeio) {
    if (letraPronta && letraId == jg.id && nLinhas) {
        int a = linhaAtual((u32)posSuave);
        static char atual[3][TAM_LINHA], seguinte[2][TAM_LINHA];
        // a linha atual quebra em até 3 linhas e a caixa cresce (centrada em yMeio)
        int nA = a >= 0 ? quebrarTexto(textoLinha(a), 17, 368, atual, 3) : 0;
        float hCaixa = (nA ? nA : 1) * 20 + 12;
        float topo = yMeio - hCaixa / 2 + 6;
        if (a >= 1 && nA <= 1)                           // a anterior, se sobrar espaço
            uiTexto(200, topo - 24, 13, COR_MUDO, CENTRO, 376, "%s", textoLinha(a - 1));
        if (nA) {
            uiRetArred(8, topo - 6, 384, hCaixa, 10, HEX(0x3A1436));
            for (int i = 0; i < nA; i++) uiTexto(200, topo + i * 20, 17, COR_BRANCO, CENTRO, 376, "%s", atual[i]);
        } else {
            uiTexto(200, topo, 15, COR_MUDO, CENTRO, 0, "(introdução...)");
        }
        if (a + 1 < nLinhas) {                           // a próxima embaixo (até 2 linhas)
            int nB = quebrarTexto(textoLinha(a + 1), 13, 376, seguinte, nA >= 3 ? 1 : 2);
            for (int i = 0; i < nB; i++)
                uiTexto(200, topo - 6 + hCaixa + 6 + i * 16, 13, COR_TEXTO2, CENTRO, 376, "%s", seguinte[i]);
        }
    } else if (jg.estadoLetra == 2) {
        uiTexto(200, yMeio - 8, 15, COR_TEXTO2, CENTRO, 376, "Não achei a letra desta música.");
        uiTexto(200, yMeio + 16, 12, COR_MUDO, CENTRO, 376, "Dá para cantar assim mesmo: siga as notas da faixa.");
    } else {
        uiTexto(200, yMeio, 15, COR_TEXTO2, CENTRO, 0, "Buscando a letra...");
    }
}

static void desenharJogoCima(bool conectado) {
    uiTela(alvoCima);
    uiBarraStatus("Karaokê", IC_MICROFONE, COR_KARAOKE_CL);
    if (!conectado) {
        uiTexto(200, 100, 16, COR_TEXTO, CENTRO, 360, alvoCelular() ? "Procurando o celular..." : "Procurando o PC...");
        return;
    }
    if (!jg.titulo[0]) {
        uiTexto(200, 90, 18, COR_TEXTO, CENTRO, 360, alvoCelular() ? "Toque uma música no celular" : "Toque uma música no PC");
        uiTexto(200, 120, 12, COR_MUDO, CENTRO, 360, "Spotify, YouTube, o que for: a letra aparece aqui");
        return;
    }
    // de quem é a vez e os pontos
    if (jg.jogador[0]) {
        int cor = COR_CONVIDADO;
        for (int i = 0; i < nPerfis; i++)
            if (!strcmp(perfis[i].nome, jg.jogador)) cor = perfis[i].cor;
        chipJogador(10, UI_BARRA + 6, jg.jogador, cor, 150);
        if (jg.nJog > 1) uiTexto(10, UI_BARRA + 31, 11, COR_MUDO, ESQ, 0, "vez %d de %d", jg.vez + 1, jg.nJog);
    }
    uiTexto(390, UI_BARRA + 2, 26, jg.turboS > 0 ? COR_AMARELO : COR_BRANCO, DIR, 0, "%lu", (unsigned long)jg.pontos);
    uiTexto(390, UI_BARRA + 32, 11, COR_MUDO, DIR, 0, "pontos  -  %s", nomesDif[dificuldade]);

    uiTexto(200, UI_BARRA + 50, 13, COR_KARAOKE_CL, CENTRO, 380, "%s", jg.titulo);
    uiTexto(200, UI_BARRA + 66, 11, COR_MUDO, CENTRO, 380, "%s", jg.artista);

    desenharLetra(140);

    // barra da música e o que a faixa está mostrando
    uiBarra(10, 222, 380, 6, jg.progresso / 100.0f, COR_KARAOKE, COR_CARTAO2);
    if (!pcAntigo)
        uiTexto(200, 206, 10, COR_MUDO, CENTRO, 380, "%s", jg.gravada ? "Notas gravadas: elas vêm pela faixa"
                                                          : "1ª vez: gravando as notas (na próxima, elas vêm pela faixa)");

    if (jg.turboS > 0) {
        uiRetArred(130, UI_BARRA + 30, 140, 20, 9, COR_AMARELO);
        uiTexto(200, UI_BARRA + 32, 13, COR_ESCURO, CENTRO, 0, "TURBO  x2");
    }
    if (!jg.tocando) {                                   // pausada no PC: tudo para
        uiRetArred(110, 112, 180, 50, 14, C2D_Color32(10, 10, 16, 230));
        uiIcone(IC_PAUSE, 140, 137, 22, COR_BRANCO, COR_ESCURO);
        uiTexto(160, 126, 18, COR_BRANCO, ESQ, 0, "Pausada");
    }
}

// ---------------------------------------------------------------- menu

#define CHIP_W 100
#define CHIP_H 28
static void chipXY(int i, float *x, float *y) {
    *x = 6 + (i % 3) * (CHIP_W + 4);
    *y = 26 + (i / 3) * (CHIP_H + 5);
}

static int ordemDe(int id) {
    for (int i = 0; i < nEscolhidos; i++)
        if (escolhidos[i] == id) return i + 1;
    return 0;
}

static void desenharMenuCima(bool conectado) {
    uiTela(alvoCima);
    uiBarraStatus("Karaokê", IC_MICROFONE, COR_KARAOKE_CL);
    if (!conectado) {
        uiTexto(200, 100, 16, COR_TEXTO, CENTRO, 360, alvoCelular() ? "Procurando o celular..." : "Procurando o PC...");
        return;
    }
    if (pcAntigo) {
        uiTexto(200, 70, 16, COR_AMARELO, CENTRO, 380, "O programa do PC está desatualizado");
        uiTexto(200, 96, 12, COR_TEXTO2, CENTRO, 380, "Atualize a pasta pc do PC para ter perfis, recordes e rodadas.");
        uiTexto(200, 116, 12, COR_TEXTO2, CENTRO, 380, "Dá para cantar assim mesmo: toque em Cantar.");
        return;
    }
    if (jg.titulo[0]) {
        uiIcone(IC_MUSICA, 22, UI_BARRA + 20, 18, COR_KARAOKE_CL, COR_FUNDO);
        uiTexto(38, UI_BARRA + 8, 15, COR_TEXTO, ESQ, 350, "%s", jg.titulo);
        uiTexto(38, UI_BARRA + 26, 11, COR_MUDO, ESQ, 350, "%s%s", jg.artista, jg.tocando ? "" : "  (pausada)");
    } else {
        uiIcone(IC_MUSICA, 22, UI_BARRA + 20, 18, COR_LINHA, COR_FUNDO);
        uiTexto(38, UI_BARRA + 8, 15, COR_TEXTO2, ESQ, 350, "Nada tocando");
        uiTexto(38, UI_BARRA + 26, 11, COR_MUDO, ESQ, 350, alvoCelular() ? "Toque uma música no celular (YouTube Music...)" : "Toque uma música no PC (Spotify, YouTube...)");
    }

    uiTexto(14, 78, 13, COR_KARAOKE_CL, ESQ, 0, "Recordes desta música");
    if (!nTops) uiTexto(14, 100, 12, COR_MUDO, ESQ, 370, "Ninguém cantou esta ainda. Seja o primeiro!");
    for (int i = 0; i < nTops; i++) {
        float y = 98 + i * 22;
        uiCartaoSel(10, y, 380, 20, 6, i == 0, HEX(0x3A1436));
        uiTexto(20, y + 2, 13, i == 0 ? COR_AMARELO : COR_TEXTO2, ESQ, 0, "%d.", i + 1);
        uiTexto(44, y + 2, 13, COR_TEXTO, ESQ, 170, "%s", tops[i].nome);
        int d = tops[i].dif == 'F' ? 0 : tops[i].dif == 'D' ? 2 : 1;
        uiRetArred(222, y + 3, 16, 14, 4, coresDif[d]);
        uiTexto(230, y + 3, 10, COR_BRANCO, CENTRO, 0, "%c", tops[i].dif);
        for (int e = 0; e < 5; e++) estrela(254 + e * 14, y + 10, 6, e < tops[i].estrelas ? COR_AMARELO : COR_CARTAO2);
        uiTexto(384, y + 2, 13, COR_TEXTO, DIR, 0, "%lu", (unsigned long)tops[i].pontos);
    }

    // a ordem de quem canta
    if (nEscolhidos) {
        char ordem[120] = "";
        for (int i = 0; i < nEscolhidos; i++) {
            const char *nome = "Convidado";
            for (int j = 0; j < nPerfis; j++)
                if (perfis[j].id == escolhidos[i]) nome = perfis[j].nome;
            char parte[32];
            snprintf(parte, sizeof(parte), "%s%d. %s", i ? "   " : "", i + 1, nome);
            strncat(ordem, parte, sizeof(ordem) - strlen(ordem) - 1);
        }
        uiTexto(200, 212, 12, COR_TEXTO, CENTRO, 380, "%s", ordem);
    } else {
        uiTexto(200, 212, 12, COR_MUDO, CENTRO, 380, "Toque nos jogadores lá embaixo (até 4, na ordem de quem canta)");
    }
}

static int nChips(void) { return nPerfis + (nPerfis < MAX_PERFIS ? 1 : 0) + 1; }   // perfis + Novo + Convidado

static void desenharMenuBaixo(bool modoApagar) {
    uiTela(alvoBaixo);
    uiTexto(160, 5, 12, modoApagar ? COR_VERMELHO : COR_TEXTO2, CENTRO, 316, "%s",
            modoApagar ? "Toque no perfil para apagar (B cancela)" : "Quem vai cantar?");
    for (int i = 0; i < nChips(); i++) {
        float x, y;
        chipXY(i, &x, &y);
        if (i < nPerfis) {
            Perfil *p = &perfis[i];
            int ordem = ordemDe(p->id);
            uiCartaoSel(x, y, CHIP_W, CHIP_H, 8, ordem, corPerfil(p->cor));
            if (!ordem) uiRet(x, y + 4, 4, CHIP_H - 8, corPerfil(p->cor));
            uiTexto(x + 9, y + 2, 12, COR_BRANCO, ESQ, CHIP_W - 30, "%s", p->nome);
            uiTexto(x + 9, y + 16, 9, ordem ? COR_BRANCO : COR_MUDO, ESQ, 0, "nível %d", p->nivel);
            uiBarra(x + 50, y + 19, 30, 4, p->pct / 100.0f, ordem ? COR_BRANCO : corPerfil(p->cor), COR_CARTAO2);
            if (ordem) {
                uiCirculo(x + CHIP_W - 11, y + 14, 9, COR_BRANCO);
                uiTexto(x + CHIP_W - 11, y + 6, 12, corPerfil(p->cor), CENTRO, 0, "%d", ordem);
            }
        } else if (i == nPerfis && nPerfis < MAX_PERFIS) {
            uiRetArred(x, y, CHIP_W, CHIP_H, 8, COR_CARTAO2);
            uiTexto(x + CHIP_W / 2, y + 6, 13, COR_KARAOKE_CL, CENTRO, 0, "+ Novo perfil");
        } else {
            int ordem = ordemDe(0);
            uiCartaoSel(x, y, CHIP_W, CHIP_H, 8, ordem, COR_MUDO);
            uiTexto(x + 9, y + 6, 12, COR_TEXTO, ESQ, CHIP_W - 30, "Convidado");
            if (ordem) {
                uiCirculo(x + CHIP_W - 11, y + 14, 9, COR_BRANCO);
                uiTexto(x + CHIP_W - 11, y + 6, 12, COR_MUDO, CENTRO, 0, "%d", ordem);
            }
        }
    }
    uiBotao(6, 196, 70, 38, "Apagar", -1, modoApagar ? COR_VERMELHO : COR_CARTAO2, COR_TEXTO);
    uiRetArred(80, 196, 100, 38, 9, COR_CARTAO2);
    uiTexto(130, 199, 9, COR_MUDO, CENTRO, 0, "nível (L/R)");
    uiTexto(130, 212, 14, coresDif[dificuldade], CENTRO, 96, "%s", nomesDif[dificuldade]);
    uiBotao(184, 196, 130, 38, "Cantar! (A)", IC_MICROFONE, COR_KARAOKE, COR_BRANCO);
}

static void receberPacotes(void);
static bool confirmar(const char *pergunta) {
    return uiConfirmar("Karaokê", IC_MICROFONE, COR_VERMELHO, pergunta, "", "Sim", "Não", receberPacotes);
}

static int escolherCor(const char *nome) {
    int sel = nPerfis % 7;
    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        touchPosition tp;
        hidTouchRead(&tp);
        if (down & KEY_B) return -1;
        if (down & KEY_A) return sel;
        if (down & KEY_DLEFT) sel = (sel + 6) % 7;
        if (down & KEY_DRIGHT) sel = (sel + 1) % 7;
        if (down & KEY_TOUCH) {
            for (int c = 0; c < 7; c++)
                if (uiDentro(&tp, 14 + c * 43, 70, 38, 38)) sel = c;
            if (uiDentro(&tp, 60, 160, 200, 44)) return sel;
        }
        receberPacotes();
        uiQuadro();
        uiTela(alvoCima);
        uiBarraStatus("Karaokê", IC_MICROFONE, COR_KARAOKE_CL);
        uiTexto(200, 60, 14, COR_TEXTO2, CENTRO, 0, "Escolha a cor de");
        chipJogador(200 - (uiLarguraTextoMax(nome, 13, 200) + 30) / 2, 90, nome, sel, 200);
        uiTela(alvoBaixo);
        uiTexto(160, 30, 13, COR_TEXTO2, CENTRO, 0, "Toque numa cor");
        for (int c = 0; c < 7; c++) {
            if (c == sel) uiCirculo(33 + c * 43, 89, 21, COR_BRANCO);
            uiCirculo(33 + c * 43, 89, 17, corPerfil(c));
        }
        uiBotao(60, 160, 200, 44, "Criar perfil (A)", IC_OK, corPerfil(sel), COR_BRANCO);
        uiFim();
    }
    return -1;
}

static void novoPerfil(void) {
    SwkbdState kb;
    char nome[40] = "";
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 16);
    swkbdSetHintText(&kb, "Nome do jogador");
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    if (swkbdInputText(&kb, nome, sizeof(nome)) != SWKBD_BUTTON_CONFIRM) return;
    int cor = escolherCor(nome);
    if (cor < 0) return;
    u8 dados[40];
    dados[0] = (u8)cor;
    int n = strlen(nome);
    memcpy(dados + 1, nome, n);
    if (pedir('N', dados, 1 + n)) {
        lerPerfis();
        if (nPerfis && nEscolhidos < MAX_JOG) escolhidos[nEscolhidos++] = perfis[nPerfis - 1].id;
    }
}

static void tocarChip(int i, bool *modoApagar) {
    if (i < nPerfis) {
        int id = perfis[i].id;
        if (*modoApagar) {
            *modoApagar = false;
            char pergunta[80];
            snprintf(pergunta, sizeof(pergunta), "Apagar o perfil de %s?", perfis[i].nome);
            u8 b = (u8)id;
            if (confirmar(pergunta) && pedir('A', &b, 1)) lerPerfis();
            return;
        }
        int o = ordemDe(id);
        if (o) {                                         // tira da lista (e os de depois sobem)
            for (int k = o - 1; k < nEscolhidos - 1; k++) escolhidos[k] = escolhidos[k + 1];
            nEscolhidos--;
        } else if (nEscolhidos < MAX_JOG) {
            escolhidos[nEscolhidos++] = id;
        }
    } else if (i == nPerfis && nPerfis < MAX_PERFIS) {
        *modoApagar = false;
        novoPerfil();
    } else {
        *modoApagar = false;
        int o = ordemDe(0);
        if (o) {
            for (int k = o - 1; k < nEscolhidos - 1; k++) escolhidos[k] = escolhidos[k + 1];
            nEscolhidos--;
        } else if (nEscolhidos < MAX_JOG) {
            escolhidos[nEscolhidos++] = 0;
        }
    }
}

// ---------------------------------------------------------------- resultado e ranking

static void desenharResultado(void) {
    uiTela(alvoCima);
    uiBarraStatus("Karaokê", IC_MICROFONE, COR_KARAOKE_CL);
    int cor = COR_CONVIDADO;
    for (int i = 0; i < nPerfis; i++)
        if (!strcmp(perfis[i].nome, jg.jogador)) cor = perfis[i].cor;
    chipJogador(10, UI_BARRA + 6, jg.jogador, cor, 200);
    uiTexto(390, UI_BARRA + 8, 12, COR_MUDO, DIR, 220, "%s", jg.titulo);

    estrelas(200, 80, 17, res.estrelas, 40);
    uiTexto(200, 102, 34, COR_BRANCO, CENTRO, 0, "%lu", (unsigned long)res.pontos);
    uiTexto(200, 140, 12, COR_MUDO, CENTRO, 390, "pontos  -  %d%% de afinação  -  maior combo %d  -  %s",
            res.precisao, res.comboMax, nomesDif[dificuldade]);

    if (res.recordeGeral) {
        bool pisca = (osGetTime() / 400) % 2;
        uiRetArred(110, 160, 180, 26, 10, pisca ? COR_AMARELO : COR_CORAL);
        uiTexto(200, 164, 16, COR_ESCURO, CENTRO, 0, "NOVO RECORDE!");
    } else if (res.recorde) {
        uiRetArred(120, 160, 160, 26, 10, COR_KARAOKE);
        uiTexto(200, 164, 14, COR_BRANCO, CENTRO, 0, "Recorde pessoal!");
    }
    if (res.temPerfil) {
        uiTexto(200, 194, 12, res.subiu ? COR_AMARELO : COR_TEXTO2, CENTRO, 380, "+%d XP  -  %s %d%s", res.xp,
                res.subiu ? "subiu para o nível" : "nível", res.nivel, res.subiu ? "!" : "");
    } else {
        uiTexto(200, 194, 12, COR_MUDO, CENTRO, 380, "Convidado não salva recordes: crie um perfil no menu");
    }

    uiTela(alvoBaixo);
    for (int j = 1; j <= 4; j++) {
        float x = 8 + (j - 1) * 77;
        uiCartao(x, 8, 73, 50, 8);
        uiTexto(x + 36, 13, 11, coresJulg[j], CENTRO, 0, "%s", j == 1 ? "Perfeito" : j == 2 ? "Ótimo" : j == 3 ? "Bom" : "Errou");
        uiTexto(x + 36, 30, 18, COR_TEXTO, CENTRO, 0, "%d", res.cont[j]);
    }
    // ranking da rodada até agora
    for (int i = 0; i < nRanking; i++) {
        float y = 66 + i * 22;
        uiCartao(8, y, 304, 20, 6);
        uiCirculo(20, y + 10, 6, corPerfil(ranking[i].cor));
        uiTexto(32, y + 2, 13, COR_TEXTO, ESQ, 150, "%d. %s", i + 1, ranking[i].nome);
        uiTexto(304, y + 2, 13, COR_TEXTO, DIR, 0, "%lu", (unsigned long)ranking[i].pontos);
    }
    if (jg.rodada == 2) {
        char rot[48];
        snprintf(rot, sizeof(rot), "Próximo: %s (A)", proximoNome[0] ? proximoNome : "jogador");
        uiBotao(8, 172, 304, 56, rot, IC_MICROFONE, COR_KARAOKE, COR_BRANCO);
    } else if (jg.nJog > 1) {
        uiBotao(8, 172, 304, 56, "Ver o ranking (A)", IC_LISTA, COR_KARAOKE, COR_BRANCO);
    } else {
        uiBotao(8, 172, 196, 56, "Cantar de novo (A)", IC_MICROFONE, COR_KARAOKE, COR_BRANCO);
        uiBotao(212, 172, 100, 56, "Menu (B)", -1, COR_CARTAO2, COR_TEXTO);
    }
}

static void desenharRanking(void) {
    uiTela(alvoCima);
    uiBarraStatus("Karaokê", IC_MICROFONE, COR_KARAOKE_CL);
    uiTexto(200, UI_BARRA + 6, 16, COR_KARAOKE_CL, CENTRO, 0, "Ranking da rodada");
    uiTexto(200, UI_BARRA + 28, 11, COR_MUDO, CENTRO, 380, "%s", jg.titulo);
    for (int i = 0; i < nRanking; i++) {
        float y = 76 + i * 36;
        uiCartaoSel(30, y, 340, 32, 10, i == 0, HEX(0x3A1436));
        uiTexto(48, y + 6, 18, i == 0 ? COR_AMARELO : COR_TEXTO2, ESQ, 0, "%d", i + 1);
        uiCirculo(80, y + 16, 8, corPerfil(ranking[i].cor));
        uiTexto(94, y + 7, 15, COR_TEXTO, ESQ, 130, "%s", ranking[i].nome);
        for (int e = 0; e < 5; e++) estrela(240 + e * 14, y + 16, 6, e < ranking[i].estrelas ? COR_AMARELO : COR_CARTAO2);
        uiTexto(360, y + 7, 15, COR_TEXTO, DIR, 0, "%lu", (unsigned long)ranking[i].pontos);
    }
    if (nRanking) {
        uiTexto(200, 222, 12, COR_AMARELO, CENTRO, 380, "%s venceu!", ranking[0].nome);
    }

    uiTela(alvoBaixo);
    uiTexto(160, 10, 12, COR_TEXTO2, CENTRO, 0, "Recordes desta música");
    for (int i = 0; i < nTops; i++) {
        float y = 30 + i * 22;
        uiTexto(20, y, 12, i == 0 ? COR_AMARELO : COR_MUDO, ESQ, 0, "%d.", i + 1);
        uiTexto(40, y, 12, COR_TEXTO, ESQ, 170, "%s", tops[i].nome);
        uiTexto(300, y, 12, COR_TEXTO, DIR, 0, "%lu", (unsigned long)tops[i].pontos);
    }
    uiBotao(8, 172, 196, 56, "Cantar de novo (A)", IC_MICROFONE, COR_KARAOKE, COR_BRANCO);
    uiBotao(212, 172, 100, 56, "Menu (B)", -1, COR_CARTAO2, COR_TEXTO);
}

// ---------------------------------------------------------------- módulo

enum { TELA_MENU, TELA_JOGO, TELA_RESULTADO, TELA_RANKING };

static void aviso(const char *texto) { uiAviso("Karaokê", texto, IC_MICROFONE, COR_KARAOKE); }

void modKaraoke(void) {
    sock = abrirUDP(PORTA, &dest);
    if (sock < 0) {
        avisoSemConexao("Karaokê", IC_MICROFONE, COR_KARAOKE);
        return;
    }

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
        aviso("Não consegui ligar o microfone do 3DS.");
        return;
    }

    memset(&jg, 0, sizeof(jg));
    jg.alvo = -1;
    memset(jg.janela, NAO_GRAVADO, N_JANELA);
    novaMusica(0);
    pcAntigo = false;
    ultimaResposta = 0;
    topsId = 0xFFFFFFFF;
    nTops = 0;
    popupJulg = 0;
    ultimoJulgSeq = -1;
    u64 ultimoEnvio = 0, turboAte = 0, ultimaLista = 0;
    float vol = 0, hz = 0;
    int tela = TELA_MENU;
    bool modoApagar = false, listou = false;
    posSuave = 0;
    ultimoQuadro = osGetTime();

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        touchPosition tp;
        hidTouchRead(&tp);
        u64 agora = osGetTime();
        bool conectado = ultimaResposta && agora - ultimaResposta < 2000;

        // ---- microfone e voz (sempre: o PC precisa da voz e de saber que o 3DS está aqui)
        if (agora - ultimoEnvio >= 50) {
            ultimoEnvio = agora;
            // as últimas N_AMOSTRAS do microfone (o buffer dele é um anel)
            u32 total = micTam / 2;
            u32 pos = (micGetLastSampleOffset() / 2 + total - N_AMOSTRAS) % total;
            const s16 *dados = (const s16 *)micBuf;
            for (int i = 0; i < N_AMOSTRAS; i++) {
                amostras[i] = dados[pos];
                if (++pos == total) pos = 0;
            }
            hz = detectarNota(&vol);
            u8 pacote[8];
            u16 dhz = (u16)(hz * 10 > 65535 ? 0 : hz * 10);
            memcpy(pacote, "3DSK", 4);
            memcpy(pacote + 4, &dhz, 2);
            pacote[6] = (u8)vol;
            pacote[7] = 2 | (agora < turboAte ? 1 : 0);
            enviarPara(sock, pacote, sizeof(pacote), &dest);
            if (jg.tocando) {                            // rastro da voz (parado se a música pausar)
                int idx = (vozIni + vozN) % 96;
                voz[idx].ms = posicaoAgora();
                float st = (hz > 0 && vol >= 8) ? semitom(hz) : -1;
                if (st < 0) stDesenho = -1;
                else if (stDesenho < 0) stDesenho = st;
                else {                                   // anda metade do caminho (dando a volta na oitava)
                    float d = st - stDesenho;
                    if (d > 6) d -= 12;
                    if (d < -6) d += 12;
                    stDesenho = fmodf(stDesenho + d * 0.5f + 12, 12);
                }
                voz[idx].st = stDesenho;
                if (vozN < 96) vozN++;
                else vozIni = (vozIni + 1) % 96;
            }
        }
        receberPacotes();

        // posição suave: anda com o relógio e corrige devagar para a do PC
        float dt = (float)(agora - ultimoQuadro);
        ultimoQuadro = agora;
        float alvoPos = (float)posicaoAgora();
        if (jg.tocando) posSuave += dt;
        if (fabsf(alvoPos - posSuave) > 400) posSuave = alvoPos;
        else posSuave += (alvoPos - posSuave) * 0.1f;

        if (jg.julgSeq != ultimoJulgSeq) {
            if (ultimoJulgSeq >= 0 && jg.julg) {
                popupJulg = jg.julg;
                popupDesde = agora;
            }
            ultimoJulgSeq = jg.julgSeq;
        }

        // ---- cada tela
        if (tela == TELA_MENU) {
            if (conectado && !pcAntigo && (!listou || agora - ultimaLista > 15000)) {
                listou = true;
                ultimaLista = agora;
                if (pedir('L', NULL, 0)) lerPerfis();
            }
            if (conectado && !pcAntigo && jg.id != topsId) {
                topsId = jg.id;
                if (pedir('K', NULL, 0)) lerTops(resp);
            }
            if (down & KEY_START) break;
            if ((down & KEY_B) && modoApagar) modoApagar = false;
            bool cantar = (down & KEY_A) != 0;
            if (down & KEY_R) dificuldade = (dificuldade + 1) % 3;
            if (down & KEY_L) dificuldade = (dificuldade + 2) % 3;
            if (down & KEY_TOUCH) {
                for (int i = 0; i < nChips(); i++) {
                    float x, y;
                    chipXY(i, &x, &y);
                    if (uiDentro(&tp, x, y, CHIP_W, CHIP_H)) {
                        tocarChip(i, &modoApagar);
                        break;                           // a lista pode ter mudado
                    }
                }
                if (uiDentro(&tp, 6, 196, 70, 38)) modoApagar = !modoApagar && nPerfis > 0;
                if (uiDentro(&tp, 80, 196, 100, 38)) dificuldade = (dificuldade + 1) % 3;
                if (uiDentro(&tp, 184, 196, 130, 38)) cantar = true;
            }
            if (cantar && conectado) {
                if (pcAntigo) {
                    tela = TELA_JOGO;
                } else {
                    u8 pedido[2 + MAX_JOG];
                    int n = nEscolhidos ? nEscolhidos : 1;
                    pedido[0] = (u8)n;
                    for (int i = 0; i < n; i++) pedido[1 + i] = nEscolhidos ? (u8)escolhidos[i] : 0;
                    pedido[1 + n] = (u8)dificuldade;
                    if (pedir('S', pedido, 2 + n)) {
                        tela = TELA_JOGO;
                        vozN = 0;
                        ultimoJulgSeq = -1;
                    } else {
                        aviso(alvoCelular() ? "O celular não respondeu. Confira se o 3DS Hub está ligado nele."
                                              : "O PC não respondeu. Confira se o 3DS Hub está aberto no PC.");
                    }
                }
            }
        } else if (tela == TELA_JOGO) {
            if (down & KEY_START) {
                if (!pcAntigo) pedir('X', NULL, 0);
                tela = TELA_MENU;
                listou = false;
                topsId = 0xFFFFFFFF;
            } else if (!pcAntigo) {
                if ((down & KEY_A) && jg.turbo >= 100 && jg.turboS == 0) turboAte = agora + 400;
                if (down & KEY_X) { pedir('C', NULL, 0); vozN = 0; }
                if (down & KEY_R) pedir('Z', NULL, 0);
                if ((down & KEY_SELECT) && confirmar("Encerrar a sua vez agora?")) pedir('F', NULL, 0);
                if ((jg.rodada == 2 || jg.rodada == 3) && pedirResultado()) tela = TELA_RESULTADO;
            }
        } else if (tela == TELA_RESULTADO) {
            bool a = (down & KEY_A) || ((down & KEY_TOUCH) && uiDentro(&tp, 8, 172, jg.rodada == 2 || jg.nJog > 1 ? 304 : 196, 56));
            bool b = (down & (KEY_B | KEY_START)) || ((down & KEY_TOUCH) && jg.rodada == 3 && jg.nJog <= 1 &&
                                                      uiDentro(&tp, 212, 172, 100, 56));
            if (jg.rodada == 2 && a) {
                if (pedir('P', NULL, 0)) { tela = TELA_JOGO; vozN = 0; ultimoJulgSeq = -1; }
            } else if (jg.rodada == 3 && a && jg.nJog > 1) {
                tela = TELA_RANKING;
            } else if (jg.rodada == 3 && a) {
                if (pedir('J', NULL, 0)) { tela = TELA_JOGO; vozN = 0; ultimoJulgSeq = -1; }
            } else if (b && (jg.rodada == 3 || (down & KEY_START))) {
                pedir('X', NULL, 0);
                tela = TELA_MENU;
                listou = false;
                topsId = 0xFFFFFFFF;
            } else if (jg.rodada == 1) {
                tela = TELA_JOGO;                        // a vez recomeçou por outro caminho
            }
        } else if (tela == TELA_RANKING) {
            bool a = (down & KEY_A) || ((down & KEY_TOUCH) && uiDentro(&tp, 8, 172, 196, 56));
            bool b = (down & (KEY_B | KEY_START)) || ((down & KEY_TOUCH) && uiDentro(&tp, 212, 172, 100, 56));
            if (a) {
                if (pedir('J', NULL, 0)) { tela = TELA_JOGO; vozN = 0; ultimoJulgSeq = -1; }
            } else if (b) {
                pedir('X', NULL, 0);
                tela = TELA_MENU;
                listou = false;
                topsId = 0xFFFFFFFF;
            }
        }
        if (!aptMainLoop()) break;

        // ---- desenho
        uiQuadro();
        if (tela == TELA_MENU) {
            desenharMenuCima(conectado);
            desenharMenuBaixo(modoApagar);
        } else if (tela == TELA_JOGO) {
            desenharJogoCima(conectado);
            desenharJogoBaixo();
        } else if (tela == TELA_RESULTADO) {
            desenharResultado();
        } else {
            desenharRanking();
        }
        uiFim();
    }

    if (!pcAntigo && ultimaResposta) pedir('X', NULL, 0);
    MICU_StopSampling();
    micExit();
    free(micBuf);
    fecharRede(sock);
}
