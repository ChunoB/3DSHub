// Segunda Tela — gráficos do PC na tela de cima; música, volume e atalhos na de baixo.
//
//   L / R ou as abas       : página MÚSICA / ATALHOS
//   A                      : play/pause
//   esquerda/direita       : música anterior/próxima
//   cima/baixo             : volume
//   START                  : voltar ao menu

#include "comum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#define PORTA       5006
#define HIST        120

enum { CMD_PLAY = 1, CMD_PROXIMA, CMD_ANTERIOR, CMD_VOL_MAIS, CMD_VOL_MENOS, CMD_MUDO };
#define CMD_ATALHO 100

typedef struct {
    char hora[8], data[16], ramu[8], ramt[8], titulo[160], artista[160];   // UTF-8: acento ocupa 2 bytes
    char hcpu[HIST + 1], hgpu[HIST + 1], hram[HIST + 1], hdown[HIST + 1], hup[HIST + 1];
    char atalhos[400];
    int  cpu, gpu, ram, disco, down, up, bat, carg, vol, mudo, tocando, redemax;
} Dados;

static char nomesAtalho[9][48];
static int nAtalhos;
static int pagina;   // 0 = música, 1 = atalhos

// ---------------------------------------------------------------- dados

static void lerDados(char *buf, Dados *d) {
    char *salvo;
    for (char *linha = strtok_r(buf, "\n", &salvo); linha; linha = strtok_r(NULL, "\n", &salvo)) {
        char *igual = strchr(linha, '=');
        if (!igual) continue;
        *igual = 0;
        const char *k = linha, *v = igual + 1;

        #define TEXTO(nome) if (!strcmp(k, #nome)) { snprintf(d->nome, sizeof(d->nome), "%s", v); continue; }
        #define NUMERO(nome) if (!strcmp(k, #nome)) { d->nome = atoi(v); continue; }
        TEXTO(hora) TEXTO(data) TEXTO(ramu) TEXTO(ramt) TEXTO(titulo) TEXTO(artista)
        TEXTO(hcpu) TEXTO(hgpu) TEXTO(hram) TEXTO(hdown) TEXTO(hup) TEXTO(atalhos)
        NUMERO(cpu) NUMERO(gpu) NUMERO(ram) NUMERO(disco) NUMERO(down) NUMERO(up)
        NUMERO(bat) NUMERO(carg) NUMERO(vol) NUMERO(mudo) NUMERO(tocando) NUMERO(redemax)
        #undef TEXTO
        #undef NUMERO
    }
}

// "Nome1|Nome2|..." -> até 9 nomes de atalho
static void montarAtalhos(const char *lista) {
    nAtalhos = 0;
    const char *p = lista;
    while (*p && nAtalhos < 9) {
        const char *fim = strchr(p, '|');
        int tam = fim ? fim - p : (int)strlen(p);
        snprintf(nomesAtalho[nAtalhos++], sizeof(nomesAtalho[0]), "%.*s", tam, p);   // o PC já limita a 10 letras
        if (!fim) break;
        p = fim + 1;
    }
}

// ---------------------------------------------------------------- tela de cima

static void velocidade(char *s, size_t n, int kbs) {
    if (kbs >= 1024) snprintf(s, n, "%.1f MB/s", kbs / 1024.0f);
    else             snprintf(s, n, "%d KB/s", kbs);
}

// Linha do histórico ('A' = 0 ... 's' = máximo) dentro do retângulo dado.
static void grafico(float x, float y, float w, float h, const char *hist, u32 cor) {
    int n = strlen(hist);
    if (n < 2) return;
    float passo = w / (HIST - 1);
    float xAntes = 0, yAntes = 0;
    for (int i = 0; i < n; i++) {
        int v = hist[i] - 'A';
        if (v < 0) v = 0;
        if (v > 50) v = 50;
        float px = x + (HIST - n + i) * passo, py = y + h - v * h / 50.0f;
        if (i) uiLinha(xAntes, yAntes, px, py, 1.8f, cor);
        xAntes = px;
        yAntes = py;
    }
}

static void cartaoMetrica(float x, float y, const char *rotulo, int valor, const char *hist, u32 cor) {
    uiRetArred(x, y, 120, 76, 10, COR_CARTAO);
    uiTexto(x + 10, y + 6, 12, COR_MUDO, ESQ, 0, "%s", rotulo);
    if (valor < 0) uiTexto(x + 10, y + 20, 22, COR_MUDO, ESQ, 0, "--");
    else           uiTexto(x + 10, y + 20, 22, cor, ESQ, 0, "%d%%", valor);
    uiRet(x + 8, y + 70, 104, 1, COR_LINHA);
    grafico(x + 8, y + 48, 104, 22, hist, cor);
}

static void desenharCima(const Dados *d, bool conectado) {
    uiTela(alvoCima);
    uiBarraStatus("Segunda Tela", IC_GRAFICO, COR_AGUA_CL);

    cartaoMetrica(10, 28, "CPU", d->cpu, d->hcpu, COR_AZUL_CL);
    cartaoMetrica(140, 28, "GPU", d->gpu, d->hgpu, COR_ROSA_CL);
    cartaoMetrica(270, 28, "RAM", d->ram, d->hram, COR_VERDE);

    // rede
    char down[24], up[24];
    velocidade(down, sizeof(down), d->down);
    velocidade(up, sizeof(up), d->up);
    uiRetArred(10, 110, 185, 58, 10, COR_CARTAO);
    uiTexto(20, 116, 12, COR_MUDO, ESQ, 0, "Rede");
    uiIcone(IC_SETA_BAIXO, 66, 124, 11, COR_AZUL_CL, COR_CARTAO);
    uiTexto(74, 116, 12, COR_AZUL_CL, ESQ, 50, "%s", down);
    uiIcone(IC_SETA_CIMA, 132, 124, 11, COR_AMARELO, COR_CARTAO);
    uiTexto(140, 116, 12, COR_AMARELO, ESQ, 50, "%s", up);
    grafico(18, 136, 169, 26, d->hup, COR_AMARELO);
    grafico(18, 136, 169, 26, d->hdown, COR_AZUL_CL);

    // música
    uiRetArred(205, 110, 185, 58, 10, COR_CARTAO);
    uiRetArred(213, 121, 36, 36, 8, HEX(0x0F6E56));
    uiIcone(IC_MUSICA, 231, 139, 20, HEX(0x9FE1CB), HEX(0x0F6E56));
    const char *estado = d->titulo[0] ? (d->tocando ? "Tocando agora" : "Pausado") : "Nada tocando";
    uiTexto(257, 117, 13, COR_TEXTO, ESQ, 128, "%s", estado);
    uiTexto(257, 134, 12, COR_TEXTO2, ESQ, 128, "%s", d->titulo);
    uiTexto(257, 150, 11, COR_MUDO, ESQ, 128, "%s", d->artista);

    // bateria, disco e volume do PC
    uiRetArred(10, 174, 380, 30, 10, COR_CARTAO);
    char vol[24];
    if (d->mudo)          snprintf(vol, sizeof(vol), "mudo");
    else if (d->vol >= 0) snprintf(vol, sizeof(vol), "%d%%", d->vol);
    else                  snprintf(vol, sizeof(vol), "-");
    if (d->bat >= 0)
        uiTexto(20, 181, 13, COR_TEXTO2, ESQ, 360, "Bateria %d%%%s   ·   Disco %d%%   ·   Volume %s",
                d->bat, d->carg ? " (na tomada)" : "", d->disco, vol);
    else
        uiTexto(20, 181, 13, COR_TEXTO2, ESQ, 360, "Disco %d%%   ·   Volume %s", d->disco, vol);

    uiCirculo(18, 222, 5, conectado ? COR_AGUA_CL : COR_AMARELO);
    uiTexto(30, 214, 13, conectado ? COR_TEXTO2 : COR_AMARELO, ESQ, 0, "%s",
            conectado ? "Conectado" : "Procurando o PC...");
    uiTexto(390, 214, 13, COR_MUDO, DIR, 0, "PC %s", ipPC);
}

// ---------------------------------------------------------------- tela de baixo
// Posições dos botões (as mesmas para desenhar e para o toque).

#define BTN_W 93.3f
static float colX(int c) { return 10 + c * (BTN_W + 10); }

static void retBotao(int i, float *x, float *y, float *w, float *h) {
    *w = BTN_W;
    *x = colX(i % 3);
    if (pagina == 0) { *h = 62; *y = 44 + (i / 3) * 68; }
    else             { *h = 52; *y = 44 + (i / 3) * 58; }
}

static int botoesNaPagina(void) { return pagina == 0 ? 6 : nAtalhos; }

static int comandoDoBotao(int i) {
    static const int musica[6] = { CMD_ANTERIOR, CMD_PLAY, CMD_PROXIMA, CMD_VOL_MENOS, CMD_MUDO, CMD_VOL_MAIS };
    return pagina == 0 ? musica[i] : CMD_ATALHO + i;
}

static void desenharBaixo(const Dados *d, int aceso) {
    uiTela(alvoBaixo);

    // abas
    uiRetArred(10, 8, 145, 28, 9, pagina == 0 ? COR_VERDE_AGUA : COR_CARTAO);
    uiTexto(82, 14, 14, pagina == 0 ? COR_AGUA_ESC : COR_MUDO, CENTRO, 0, "Música");
    uiRetArred(165, 8, 145, 28, 9, pagina == 1 ? COR_VERDE_AGUA : COR_CARTAO);
    uiTexto(237, 14, 14, pagina == 1 ? COR_AGUA_ESC : COR_MUDO, CENTRO, 0, "Atalhos");

    for (int i = 0; i < botoesNaPagina(); i++) {
        float x, y, w, h;
        retBotao(i, &x, &y, &w, &h);
        u32 fundo = i == aceso ? COR_AGUA_CL : COR_CARTAO;
        u32 conteudo = i == aceso ? COR_AGUA_ESC : COR_TEXTO;
        if (pagina == 0) {
            static const int icones[6] = { IC_ANTERIOR, IC_PLAY, IC_PROXIMA, IC_VOL_MENOS, IC_MUDO, IC_VOL_MAIS };
            int icone = icones[i];
            if (i == 1) {                                   // play/pause em destaque
                if (d->tocando) icone = IC_PAUSE;
                if (i != aceso) { fundo = COR_VERDE_AGUA; conteudo = COR_AGUA_ESC; }
            }
            if (i == 4 && d->mudo && i != aceso) { fundo = COR_VERMELHO; conteudo = COR_BRANCO; }
            uiBotao(x, y, w, h, NULL, icone, fundo, conteudo);
        } else {
            uiBotao(x, y, w, h, nomesAtalho[i], -1, fundo, conteudo);
        }
    }

    if (pagina == 0) {
        float f = d->vol < 0 ? 0 : d->vol / 100.0f;
        char vol[16];
        if (d->mudo)          snprintf(vol, sizeof(vol), "mudo");
        else if (d->vol >= 0) snprintf(vol, sizeof(vol), "%d%%", d->vol);
        else                  snprintf(vol, sizeof(vol), "-");
        uiTexto(10, 182, 12, COR_MUDO, ESQ, 0, "Volume");
        uiTexto(310, 182, 12, d->mudo ? COR_VERMELHO : COR_TEXTO2, DIR, 0, "%s", vol);
        uiBarra(10, 199, 300, 8, d->mudo ? 0 : f, COR_AGUA_CL, COR_CARTAO);
        // (sem setas unicode: a fonte do 3DS pode não ter esses símbolos)
        uiTexto(160, 218, 11, COR_MUDO, CENTRO, 0, "A play   lados: música   cima/baixo: volume");
    } else if (nAtalhos == 0) {
        uiTexto(160, 110, 13, COR_MUDO, CENTRO, 0, "Esperando a lista de atalhos do PC...");
    } else {
        uiTexto(160, 220, 11, COR_MUDO, CENTRO, 0, "Edite os atalhos no config.json do 3DS Hub");
    }
}

static int botaoTocado(const touchPosition *tp, int *aba) {
    *aba = -1;
    if (tp->py < 40) {                                      // linha das abas
        *aba = tp->px < 160 ? 0 : 1;
        return -1;
    }
    for (int i = 0; i < botoesNaPagina(); i++) {
        float x, y, w, h;
        retBotao(i, &x, &y, &w, &h);
        if (uiDentro(tp, x, y, w, h)) return i;
    }
    return -1;
}

static void enviar(int sock, struct sockaddr_in *dest, const char *magic, int cmd) {
    char pkt[5];
    memcpy(pkt, magic, 4);
    pkt[4] = (char)cmd;
    sendto(sock, pkt, cmd ? 5 : 4, 0, (struct sockaddr *)dest, sizeof(*dest));
}

// ---------------------------------------------------------------- módulo

void modTela(void) {
    struct sockaddr_in dest;
    int sock = abrirUDP(PORTA, &dest);
    if (sock < 0) return;

    static Dados d;
    memset(&d, 0, sizeof(d));
    d.bat = d.vol = d.gpu = -1;
    nAtalhos = 0;
    pagina = 0;

    u64 ultimaResposta = 0;
    int aceso = -1, framesAceso = 0;
    u32 frame = 0;
    static char buf[2048];

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_START) break;

        if (down & (KEY_L | KEY_R)) {
            pagina = !pagina;
            aceso = -1;
        }

        int cmd = 0, idx = -1;
        if (down & KEY_TOUCH) {
            touchPosition tp;
            int aba;
            hidTouchRead(&tp);
            idx = botaoTocado(&tp, &aba);
            if (aba >= 0 && aba != pagina) {
                pagina = aba;
                aceso = -1;
            }
            if (idx >= 0) cmd = comandoDoBotao(idx);
        }
        else if (down & KEY_A)      cmd = CMD_PLAY;
        else if (down & KEY_DLEFT)  cmd = CMD_ANTERIOR;
        else if (down & KEY_DRIGHT) cmd = CMD_PROXIMA;
        else if (down & KEY_DUP)    cmd = CMD_VOL_MAIS;
        else if (down & KEY_DDOWN)  cmd = CMD_VOL_MENOS;

        if (cmd) {
            enviar(sock, &dest, "3DSC", cmd);
            if (idx < 0)        // botão físico: acende o botão equivalente, se estiver na tela
                for (int i = 0; i < botoesNaPagina(); i++) if (comandoDoBotao(i) == cmd) idx = i;
            aceso = idx;
            framesAceso = 8;
        } else if (frame % 20 == 0) {
            enviar(sock, &dest, "3DSH", 0);   // "oi", ~3x por segundo
        }
        if (aceso >= 0 && --framesAceso <= 0) aceso = -1;

        int n;
        while ((n = recvfrom(sock, buf, sizeof(buf) - 1, 0, NULL, NULL)) > 0) {
            buf[n] = 0;
            if (n >= 5 && !memcmp(buf, "3DST\n", 5)) {
                lerDados(buf + 5, &d);
                montarAtalhos(d.atalhos);
                ultimaResposta = osGetTime();
            }
        }
        bool conectado = ultimaResposta && osGetTime() - ultimaResposta < 2000;

        uiQuadro();
        desenharCima(&d, conectado);
        desenharBaixo(&d, aceso);
        uiFim();
        frame++;
    }
    close(sock);
}
