// Segunda Tela — gráficos do PC na tela de cima; música, volume e atalhos na de baixo.
//
//   L / R ou as abas       : página MÚSICA / ATALHOS
//   A                      : play/pause
//   esquerda/direita       : música anterior/próxima
//   cima/baixo             : volume
//   X                      : o som do PC (ou do celular) tocando aqui no 3DS
//   Y                      : (celular) buscar uma música e tocar no YouTube Music
//   START                  : voltar ao menu
//
// Com o celular no lugar do PC (app 3DS Hub do Android), a tela de cima troca CPU e
// GPU, que o Android não deixa ler, pela música tocando, bateria e volume, e o som
// do celular já começa a tocar aqui.

#include "comum.h"
#include "som.h"

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
    int  celular, somCel;      // do celular: é um celular / a captura do som está liberada lá
    int  bloqueado;            // o app tocando no celular proíbe a captura do som (ex.: Spotify)
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
        if (!strcmp(k, "celular")) { d->celular = atoi(v); continue; }
        if (!strcmp(k, "som")) { d->somCel = atoi(v); continue; }
        if (!strcmp(k, "bloqueado")) { d->bloqueado = atoi(v); continue; }
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

static void textoVolume(const Dados *d, char *vol, size_t n) {
    if (d->mudo)          snprintf(vol, n, "mudo");
    else if (d->vol >= 0) snprintf(vol, n, "%d%%", d->vol);
    else                  snprintf(vol, n, "-");
}

static void rodape(bool conectado, const char *quem) {
    uiCirculo(18, 222, 5, conectado ? COR_AGUA_CL : COR_AMARELO);
    uiTexto(30, 214, 13, conectado ? COR_TEXTO2 : COR_AMARELO, ESQ, 0, "%s",
            conectado ? "Conectado" : (quem[0] == 'c' ? "Procurando o celular..." : "Procurando o PC..."));
}

// Celular: a música em destaque (é o que mais se usa), bateria, volume e o som.
static void desenharCimaCelular(const Dados *d, bool conectado) {
    uiTela(alvoCima);
    uiBarraStatus("Segunda Tela", IC_GRAFICO, COR_AGUA_CL);

    uiRetArred(10, 28, 380, 104, 12, COR_CARTAO);
    uiRetArred(22, 40, 80, 80, 14, HEX(0x0F6E56));
    uiIcone(d->tocando ? IC_MUSICA : IC_PAUSE, 62, 80, 40, HEX(0x9FE1CB), HEX(0x0F6E56));
    const char *estado = d->titulo[0] ? (d->tocando ? "TOCANDO NO CELULAR" : "PAUSADO") : "NADA TOCANDO";
    uiTexto(114, 42, 12, COR_AGUA_CL, ESQ, 266, "%s", estado);
    if (d->titulo[0]) {
        uiTexto(114, 60, 20, COR_TEXTO, ESQ, 266, "%s", d->titulo);
        uiTexto(114, 90, 14, COR_TEXTO2, ESQ, 266, "%s", d->artista);
    } else {
        uiTexto(114, 62, 14, COR_TEXTO2, ESQ, 266, "Toque uma música no celular");
        uiTexto(114, 82, 12, COR_MUDO, ESQ, 266, "Spotify, YouTube Music, Deezer...");
    }

    // som aqui no 3DS
    uiRetArred(10, 138, 185, 66, 12, COR_CARTAO);
    uiTexto(20, 144, 12, COR_MUDO, ESQ, 0, "Som no 3DS");
    if (!somLigado()) {
        uiTexto(20, 162, 16, COR_TEXTO2, ESQ, 165, "Desligado");
        uiTexto(20, 184, 11, COR_MUDO, ESQ, 165, "X liga");
    } else if (conectado && d->bloqueado) {
        uiTexto(20, 160, 13, COR_AMARELO, ESQ, 165, "Este app não deixa");
        uiTexto(20, 178, 11, COR_MUDO, ESQ, 165, "mandar o som dele.");
        uiTexto(20, 191, 11, COR_MUDO, ESQ, 165, "Ele toca só no celular.");
    } else if (somChegando()) {
        uiTexto(20, 162, 16, COR_AGUA_CL, ESQ, 165, "Tocando aqui");
        uiTexto(20, 184, 11, COR_MUDO, ESQ, 165, "X desliga");
    } else if (conectado && !d->somCel) {
        uiTexto(20, 162, 14, COR_AMARELO, ESQ, 165, "Falta liberar");
        uiTexto(20, 182, 11, COR_MUDO, ESQ, 165, "No celular: Ligar o som");
    } else {
        uiTexto(20, 162, 16, COR_TEXTO2, ESQ, 165, "Esperando...");
        uiTexto(20, 184, 11, COR_MUDO, ESQ, 165, "Alguns apps não deixam");
    }

    // bateria e volume do celular
    uiRetArred(205, 138, 185, 66, 12, COR_CARTAO);
    uiTexto(215, 144, 12, COR_MUDO, ESQ, 0, "Celular");
    if (d->bat >= 0) {
        uiTexto(215, 160, 16, d->bat <= 15 && !d->carg ? COR_VERMELHO : COR_TEXTO, ESQ, 0, "%d%%", d->bat);
        uiTexto(262, 163, 12, COR_MUDO, ESQ, 120, "%s", d->carg ? "carregando" : "bateria");
    }
    char vol[16];
    textoVolume(d, vol, sizeof(vol));
    uiTexto(215, 184, 12, COR_TEXTO2, ESQ, 165, "Volume %s   ·   %s", vol, d->hora);

    rodape(conectado, "celular");
    uiTexto(390, 214, 13, COR_MUDO, DIR, 0, "%s", ipPC);
}

static void desenharCima(const Dados *d, bool conectado) {
    if (d->celular) { desenharCimaCelular(d, conectado); return; }
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

    rodape(conectado, "pc");
    uiTexto(390, 214, 13, COR_MUDO, DIR, 0, "%s %s", somLigado() ? "Som aqui ·" : "PC", ipPC);
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

    if (pagina == 0 && d->celular) {
        // celular: no lugar da dica, o botão de buscar música
        float f = d->vol < 0 ? 0 : d->vol / 100.0f;
        char vol[16];
        textoVolume(d, vol, sizeof(vol));
        uiTexto(10, 180, 12, COR_MUDO, ESQ, 0, "Volume");
        uiTexto(310, 180, 12, d->mudo ? COR_VERMELHO : COR_TEXTO2, DIR, 0, "%s", vol);
        uiBarra(10, 196, 300, 6, d->mudo ? 0 : f, COR_AGUA_CL, COR_CARTAO);
        uiBotao(10, 208, 300, 26, "Buscar música (Y)", IC_LUPA, COR_CARTAO2, COR_TEXTO);
    } else if (pagina == 0) {
        float f = d->vol < 0 ? 0 : d->vol / 100.0f;
        char vol[16];
        if (d->mudo)          snprintf(vol, sizeof(vol), "mudo");
        else if (d->vol >= 0) snprintf(vol, sizeof(vol), "%d%%", d->vol);
        else                  snprintf(vol, sizeof(vol), "-");
        uiTexto(10, 182, 12, COR_MUDO, ESQ, 0, "Volume");
        uiTexto(310, 182, 12, d->mudo ? COR_VERMELHO : COR_TEXTO2, DIR, 0, "%s", vol);
        uiBarra(10, 199, 300, 8, d->mudo ? 0 : f, COR_AGUA_CL, COR_CARTAO);
        // (sem setas unicode: a fonte do 3DS pode não ter esses símbolos)
        uiTexto(160, 218, 11, COR_MUDO, CENTRO, 0, "A play   lados: música   cima/baixo: volume   X: som");
    } else if (nAtalhos == 0) {
        uiTexto(160, 110, 13, COR_MUDO, CENTRO, 0, "Esperando a lista de atalhos...");
    } else {
        uiTexto(160, 220, 11, COR_MUDO, CENTRO, 0, "%s", d->celular ? "Atalhos do celular"
                                                             : "Edite os atalhos no config.json do 3DS Hub");
    }
}

static bool tocouBuscar(const touchPosition *tp) {
    return pagina == 0 && uiDentro(tp, 10, 208, 300, 26);
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

// Teclado do 3DS -> o celular manda o YouTube Music tocar ("3DSB" + texto)
static void buscarMusica(int sock, struct sockaddr_in *dest) {
    SwkbdState kb;
    char busca[96];
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 60);
    swkbdSetHintText(&kb, "Música, artista ou álbum");
    swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, "Tocar", true);
    if (swkbdInputText(&kb, busca, sizeof(busca)) != SWKBD_BUTTON_CONFIRM || !busca[0]) return;
    char pkt[100];
    memcpy(pkt, "3DSB", 4);
    int n = snprintf(pkt + 4, sizeof(pkt) - 4, "%s", busca);
    enviarPara(sock, pkt, 4 + n, dest);
}

static void enviar(int sock, struct sockaddr_in *dest, const char *magic, int cmd) {
    char pkt[5];
    memcpy(pkt, magic, 4);
    pkt[4] = (char)cmd;
    enviarPara(sock, pkt, cmd ? 5 : 4, dest);
}

// ---------------------------------------------------------------- módulo

static void avisoSemSom(void) {
    uiAviso("Sem som",
            "Falta o arquivo sdmc:/3ds/dspfirm.cdc.\n\n"
            "Para criar: abra o menu Rosalina\n"
            "(L + baixo + SELECT) > Miscellaneous options\n"
            "> Dump DSP firmware. Depois abra de novo.", IC_MUDO, COR_VERDE_AGUA);
}

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
    bool somDecidido = false;           // no celular, o som liga sozinho uma vez

    // Com o som (NDSP) ligado, o HOME e a tampa são tratados aqui, como na Steam:
    // deixar o libctru suspender o app com o DSP tocando pode travar o console.
    aptSetHomeAllowed(false);
    bool podiaDormir = aptIsSleepAllowed();
    aptSetSleepAllowed(false);
    u32 frame = 0;
    static char buf[2048];

    while (aptMainLoop()) {
        if (aptCheckHomePressRejected()) {
            bool tinhaSom = somLigado();
            somEncerrar();
            aptJumpToHomeMenu();
            if (aptShouldClose()) break;
            if (tinhaSom) somIniciar();
            continue;
        }
        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_START) break;

        if (down & KEY_X) {
            somDecidido = true;
            if (somLigado()) somEncerrar();
            else if (!somIniciar()) avisoSemSom();
        }
        if ((down & KEY_Y) && d.celular) buscarMusica(sock, &dest);
        if (somLigado()) somAtualizar();

        if (down & (KEY_L | KEY_R)) {
            pagina = !pagina;
            aceso = -1;
        }

        int cmd = 0, idx = -1;
        if (down & KEY_TOUCH) {
            touchPosition tp;
            int aba;
            hidTouchRead(&tp);
            if (d.celular && tocouBuscar(&tp)) buscarMusica(sock, &dest);
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
        while ((n = receberDe(sock, buf, sizeof(buf) - 1)) > 0) {
            buf[n] = 0;
            if (n >= 5 && !memcmp(buf, "3DST\n", 5)) {
                lerDados(buf + 5, &d);
                montarAtalhos(d.atalhos);
                ultimaResposta = osGetTime();
                if (d.celular && !somDecidido) {
                    somDecidido = true;
                    if (!somIniciar()) avisoSemSom();
                }
            }
        }
        bool conectado = ultimaResposta && osGetTime() - ultimaResposta < 2000;

        uiQuadro();
        desenharCima(&d, conectado);
        desenharBaixo(&d, aceso);
        uiFim();
        frame++;
    }
    somEncerrar();
    aptSetHomeAllowed(true);
    aptSetSleepAllowed(podiaDormir);
    fecharRede(sock);
}
