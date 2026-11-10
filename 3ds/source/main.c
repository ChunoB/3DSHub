// 3DS Hub — menu que junta Controle, Segunda Tela, Sintetizador, Câmera, Espelho, Steam e Ajustes.
// Do outro lado pode estar o PC (pasta pc) ou o celular (pasta android): o celular
// responde nas mesmas portas, e os módulos que ele ainda não tem avisam isso.
//
// Menu: cruz, circle pad ou toque = escolher (L/R = anterior/próximo), A = abrir,
// Y = Ajustes, SELECT = escolher o PC ou celular, START duas vezes = sair
//
// O menu procura o PC sozinho (descoberta na rede) sempre que o Hub não responde.

#include "comum.h"
#include "updater.h"
#include "tunel.h"

#include <curl/curl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/socket.h>

#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000
#define PORTA_SONDA    5006      // a Segunda Tela responde ao ping: serve para saber se o Hub está no ar

typedef struct {
    const char *nome, *subtitulo, *descricao;
    int icone;
    u32 cor, corClara;
    void (*abrir)(void);
    bool precisaPC;
    bool noCelular;             // já funciona com o app do celular (Android)
    const char *subCelular;     // subtítulo quando o 3DS está ligado ao celular (NULL = o mesmo)
} Item;

static const Item itens[] = {
    { "Controle", "Xbox, teclado e mouse no PC",
      "Use o 3DS como controle de Xbox, teclado ou\ntouchpad. Mira por giroscópio com SELECT+B.",
      IC_CONTROLE, COR_ROXO, COR_ROXO_CL, modControle, true, true, "Botões do 3DS viram toques no celular" },
    { "Segunda Tela", "Gráficos, música e atalhos",
      "CPU, GPU, RAM e rede do PC em tempo real,\ncontrole de música e volume e atalhos.",
      IC_GRAFICO, COR_VERDE_AGUA, COR_AGUA_CL, modTela, true, true, "Música, som e bateria do celular" },
    { "Sintetizador", "Teclado e theremin",
      "Toque na tela de baixo para tocar. Inclinar\no console entorta a nota. Não precisa do PC.",
      IC_PIANO, COR_ROSA, COR_ROSA_CL, modSintetizador, false, true, NULL },
    { "Câmera", "Webcam no OBS, até em 3D",
      "Manda a câmera do 3DS para o OBS do PC.\nTraseira, frontal ou as duas em 3D.",
      IC_CAMERA, COR_CORAL, COR_CORAL_CL, modCamera, true, true, "Câmera do 3DS no celular" },
    { "Espelho", "Uma janela do PC aqui",
      "Mostra uma janela do PC na tela de cima, com\nzoom (R/L). Na aba Clicar, toque para clicar.",
      IC_ESPELHO, COR_AZUL, COR_AZUL_CL, modEspelho, true, true, "A tela do celular aqui" },
    { "Steam", "Seus jogos do PC aqui",
      "A biblioteca da Steam do PC. Escolha um jogo:\nele abre no PC e aparece na tela de cima,\ncom os botões do 3DS como controle.",
      IC_PLAY, COR_STEAM, COR_STEAM_CL, modSteam, true, false, NULL },
    { "Bichinho", "Mora no 3DS, sente o PC",
      "Um bichinho que dança com a música do PC, sua\ncom a CPU quente e dorme com o PC parado.\nDê comida e faça carinho com a caneta.",
      IC_BICHO, COR_BICHO, COR_BICHO_CL, modBicho, false, true, "Mora no 3DS, sente o celular" },
    { "Pescaria", "Peixes na TV, vara no 3DS",
      "Os peixes nadam no monitor do PC. Mire com o\ncircle pad, balance o 3DS para lançar e gire a\ncaneta para puxar. Dá para jogar em vários.",
      IC_PEIXE, COR_PESCA, COR_PESCA_CL, modPescaria, true, true, "Peixes no celular, vara no 3DS" },
    { "Karaokê", "Cante com a música do PC",
      "Toque uma música no PC (Spotify, YouTube...):\na letra rola aqui e o PC dá nota para a sua\nafinação no microfone do 3DS.",
      IC_MICROFONE, COR_KARAOKE, COR_KARAOKE_CL, modKaraoke, true, true, "Cante com a música do celular" },
    { "Ajustes", "PC, atualização e giroscópio",
      "Procurar o PC, atualizar o app pelo Wi-Fi e\najustar o giroscópio e o touchpad.",
      IC_ENGRENAGEM, COR_CARTAO2, COR_TEXTO2, modAjustes, false, true, NULL },
};
#define N_ITENS (int)(sizeof(itens) / sizeof(itens[0]))
#define ITEM_AJUSTES (N_ITENS - 1)

// Tela de baixo: os módulos numa grade de 3 colunas e, embaixo, a barra dos Ajustes
// (sempre o último item).
#define COLUNAS  3
#define N_GRADE  (N_ITENS - 1)
#define TILE_W   96
#define TILE_H   58
#define TILE_VAO 7
#define BARRA_Y  203
#define BARRA_H  30
static float tileX(int i) { return i == ITEM_AJUSTES ? 8 : 9 + (i % COLUNAS) * (TILE_W + TILE_VAO); }
static float tileY(int i) { return i == ITEM_AJUSTES ? BARRA_Y : 7 + (i / COLUNAS) * (TILE_H + TILE_VAO); }
static float tileW(int i) { return i == ITEM_AJUSTES ? 304 : TILE_W; }
static float tileH(int i) { return i == ITEM_AJUSTES ? BARRA_H : TILE_H; }

static int tileNoToque(const touchPosition *tp) {
    for (int i = 0; i < N_ITENS; i++)
        if (uiDentro(tp, tileX(i), tileY(i), tileW(i), tileH(i))) return i;
    return -1;
}

// Anda na grade. Dos lados segue a ordem dos módulos (do fim de uma linha para o começo
// da outra); para cima e para baixo dá a volta passando pelos Ajustes.
static int colunaLembrada;
static int mover(int sel, int dx, int dy) {
    if (sel != ITEM_AJUSTES) colunaLembrada = sel % COLUNAS;
    if (dx) return (sel + dx + N_ITENS) % N_ITENS;
    if (sel == ITEM_AJUSTES) {
        int linhas = (N_GRADE + COLUNAS - 1) / COLUNAS;
        int alvo = (dy < 0 ? linhas - 1 : 0) * COLUNAS + colunaLembrada;
        return alvo < N_GRADE ? alvo : N_GRADE - 1;
    }
    int alvo = sel + dy * COLUNAS;
    return alvo < 0 || alvo >= N_GRADE ? ITEM_AJUSTES : alvo;
}

// O último módulo aberto fica guardado: o menu abre nele da próxima vez.
#define ARQ_MENU "sdmc:/3ds/3dshub_menu.cfg"
static int moduloGravado = -1;
static int lerUltimoModulo(void) {
    char nome[32] = "";
    FILE *f = fopen(ARQ_MENU, "r");
    if (!f) return 0;
    if (!fgets(nome, sizeof(nome), f)) nome[0] = 0;
    fclose(f);
    nome[strcspn(nome, "\r\n")] = 0;
    for (int i = 0; i < N_GRADE; i++)
        if (!strcmp(nome, itens[i].nome)) return moduloGravado = i;
    return 0;
}
static void gravarUltimoModulo(int i) {
    if (i == moduloGravado || i == ITEM_AJUSTES) return;
    FILE *f = fopen(ARQ_MENU, "w");
    if (!f) return;
    fprintf(f, "%s\n", itens[i].nome);
    fclose(f);
    moduloGravado = i;
}

// ---------------------------------------------------------------- sonda do Hub
// Manda um "ping" (3DSP) para a Segunda Tela do PC: se responder, o Hub está no ar.
// (É um ping separado do "oi" da Segunda Tela, para o PC não achar que ela está aberta.)

static int sonda = -1;
static struct sockaddr_in destinoSonda;
static u64 ultimaRespostaHub;
static int descoberta = -1;
static char aviso[64];          // "PC encontrado: ..." por alguns segundos
static u64 avisoAte;
static bool outroNaRede;        // o aparelho escolhido não responde, mas há outro na rede
static bool hubRespondendo(void);

static void abrirSonda(void) {
    if (sonda < 0 && (ipPC[0] || modoRemoto)) sonda = abrirUDP(PORTA_SONDA, &destinoSonda);
}

static void fecharSondaDeVez(void) {
    if (sonda >= 0) fecharRede(sonda);
    sonda = -1;
}

// Antes de abrir um módulo: na rede de casa os módulos usam as mesmas portas, então
// a sonda sai do caminho. Pela internet cada conexão é separada, e manter a sonda
// evita abrir o túnel de novo (alguns segundos) toda vez que volta ao menu.
static void fecharSonda(void) {
    if (!modoRemoto) fecharSondaDeVez();
}

static void rodarSonda(u32 quadro) {
    if (sonda < 0) return;
    if (quadro % 90 == 0 && enviarPara(sonda, "3DSP", 4, &destinoSonda) < 0 && modoRemoto) {
        fecharSondaDeVez();          // o túnel caiu (ex.: ficou parado muito tempo): reabre depois
        return;
    }
    static char buf[2048];
    while (receberDe(sonda, buf, sizeof(buf)) >= 4)
        if (!memcmp(buf, "3DSP", 4)) ultimaRespostaHub = osGetTime();
}

// ---------------------------------------------------------------- casa ou internet
// Na mesma rede: conexão direta (mais rápida). Se o PC não responde na rede e o 3DS
// foi pareado para o acesso pela internet, passa a usar o túnel sozinho. Quando o PC
// aparece de novo na rede, volta para a conexão direta.

static u64 inicioApp, proximaTentativaRemota;

static void mensagemRapida(const char *titulo, const char *texto) {
    uiQuadro();
    uiTela(alvoCima);
    uiBarraStatus("3DS Hub", IC_CONTROLE, COR_TEXTO);
    uiCartao(12, 70, 376, 84, 14);
    uiTexto(200, 86, 18, COR_TEXTO, CENTRO, 350, "%s", titulo);
    uiTexto(200, 116, 13, COR_TEXTO2, CENTRO, 350, "%s", texto);
    uiTela(alvoBaixo);
    uiFim();
}

static void rodarRemoto(bool temIP) {
    if (!temIP || !tunelPareado()) return;
    u64 agora = osGetTime();
    if (!modoRemoto) {
        // espera a rede de casa ter uma chance antes de ir pela internet
        if (agora - inicioApp < 6000 || hubRespondendo() || agora < proximaTentativaRemota) return;
        proximaTentativaRemota = agora + 60000;
        mensagemRapida("Procurando o PC pela internet...", "O PC não respondeu nesta rede. START = sair");
        if (!tunelBuscarEndereco()) return;
        fecharSondaDeVez();
        modoRemoto = true;
        snprintf(nomePC, sizeof(nomePC), "%s", tunelNomePC(tunelAtual()));
        ultimaRespostaHub = 0;
        abrirSonda();
        if (sonda < 0) {             // não deu: continua na rede de casa
            modoRemoto = false;
            abrirSonda();
            return;
        }
        snprintf(aviso, sizeof(aviso), "Conectado ao PC pela internet");
        avisoAte = osGetTime() + 4000;
    } else if (sonda < 0 && agora >= proximaTentativaRemota) {
        proximaTentativaRemota = agora + 20000;
        mensagemRapida("Reconectando pela internet...", "START = sair");
        abrirSonda();
    }
}

static bool hubRespondendo(void) {
    return ultimaRespostaHub && osGetTime() - ultimaRespostaHub < 4000;
}

// ---------------------------------------------------------------- descoberta


// Pergunta de vez em quando quem é o 3DS Hub da rede. O 3DS fica com o aparelho
// escolhido (PC ou celular): só pega um sozinho quando ainda não tem nenhum, e só
// segue o mesmo aparelho se o IP dele mudou. Trocar de aparelho é com SELECT.
static void rodarDescoberta(u32 quadro) {
    if (descoberta < 0) return;
    bool hubOk = hubRespondendo();
    if (hubOk) outroNaRede = false;
    if (quadro % 120 == 30 && (!hubOk || modoRemoto || !ipPC[0] || !nomePC[0])) descobertaPerguntar(descoberta);

    char ip[32], nome[32];
    while (descobertaResposta(descoberta, ip, sizeof(ip), nome, sizeof(nome))) {
        if (modoRemoto) {
            // outro PC desta rede não tira o 3DS do PC escolhido pela internet
            if (strcmp(nome, tunelNomePC(tunelAtual()))) continue;
            // o mesmo PC apareceu nesta rede: volta para a conexão direta
            fecharSondaDeVez();
            modoRemoto = false;
            definirIP(ip);
            snprintf(nomePC, sizeof(nomePC), "%s", nome);
            ultimaRespostaHub = 0;
            abrirSonda();
            snprintf(aviso, sizeof(aviso), "PC na mesma rede: conexão direta");
            avisoAte = osGetTime() + 4000;
        } else if (!strcmp(ip, ipPC)) {
            snprintf(nomePC, sizeof(nomePC), "%s", nome);   // o mesmo aparelho: só aprende o nome
        } else if (!ipPC[0] || (!hubOk && (!nomePC[0] || !strcmp(nome, nomePC)))) {
            // primeiro aparelho, IP digitado que não responde, ou o escolhido com IP novo
            fecharSonda();
            definirIP(ip);
            snprintf(nomePC, sizeof(nomePC), "%s", nome);
            ultimaRespostaHub = 0;
            abrirSonda();
            snprintf(aviso, sizeof(aviso), "Encontrado: %s", nome);
            avisoAte = osGetTime() + 4000;
        } else if (!hubOk) {
            outroNaRede = true;      // não troca sozinho: o menu sugere o SELECT
        }
    }
}

// ---------------------------------------------------------------- desenho

static u64 confirmarSaidaAte;   // primeiro START: pede o segundo para sair

// Movimento do menu: tudo chega aos poucos no lugar (uiSuave), nada pula.
static struct {
    float destaque[N_ITENS];    // 0 = tile normal, 1 = escolhido
    float aperto[N_ITENS];      // 1 = com o dedo em cima
    float salto;                // o ícone grande "salta" ao trocar de módulo
    float entrada;              // 1 -> 0: a cor do módulo fechado some por cima do menu
    u32   corEntrada;
    u32   cor, corClara;        // cor do módulo escolhido, misturada aos poucos
    int   selAntes;
} anim = { .selAntes = -1 };

static void animar(int sel, int apertado) {
    if (anim.selAntes < 0) {                    // primeiro quadro: já começa no lugar
        anim.cor = itens[sel].cor;
        anim.corClara = itens[sel].corClara;
        anim.destaque[sel] = 1;
        anim.entrada = 1;
        anim.corEntrada = COR_ESCURO;
    } else if (sel != anim.selAntes) {
        anim.salto = 1;
    }
    anim.selAntes = sel;
    for (int i = 0; i < N_ITENS; i++) {
        anim.destaque[i] = uiSuave(anim.destaque[i], i == sel, 0.35f);
        anim.aperto[i] = uiSuave(anim.aperto[i], i == apertado, 0.5f);
    }
    anim.salto = uiSuave(anim.salto, 0, 0.22f);
    anim.entrada = uiSuave(anim.entrada, 0, 0.2f);
    anim.cor = uiMistura(anim.cor, itens[sel].cor, 0.3f);
    anim.corClara = uiMistura(anim.corClara, itens[sel].corClara, 0.3f);
}

// Faixa de aviso da tela de cima (uma linha).
static void faixa(u32 fundo, u32 cor, int icone, const char *texto) {
    uiRetArred(12, 166, 376, 24, 12, fundo);
    if (icone >= 0) {
        float w = uiLarguraTexto(texto, 13) + 20;
        uiIcone(icone, 200 - w / 2 + 6, 178, 13, cor, fundo);
        uiTexto(200 + 10, 171, 13, cor, CENTRO, 330, "%s", texto);
    } else {
        uiTexto(200, 171, 13, cor, CENTRO, 356, "%s", texto);
    }
}

static void desenharTile(int i, bool cel) {
    const Item *it = &itens[i];
    float d = anim.destaque[i], ap = anim.aperto[i];
    float cresce = 2 * d - 2 * ap;              // o escolhido cresce; o apertado afunda
    float x = tileX(i) - cresce, y = tileY(i) - cresce, w = tileW(i) + 2 * cresce, h = tileH(i) + 2 * cresce;
    bool apagado = cel && !it->noCelular;       // módulo que o celular não tem

    u32 fundo = uiMistura(COR_CARTAO, it->cor, d);
    if (ap > 0) fundo = uiMistura(fundo, COR_ESCURO, 0.3f * ap);
    u32 borda = uiMistura(COR_BORDA, it->corClara, d * 0.8f);
    u32 corIcone = uiMistura(apagado ? COR_LINHA : it->corClara, COR_BRANCO, d);
    u32 corTexto = uiMistura(apagado ? COR_MUDO : COR_TEXTO2, COR_BRANCO, d);
    uiCartaoCor(x, y, w, h, 12, fundo, borda);

    if (i == ITEM_AJUSTES) {                    // barra: ícone e nome à esquerda, versão à direita
        float cy = y + h / 2;
        uiIcone(it->icone, x + 20, cy, 16, corIcone, fundo);
        uiTexto(x + 36, cy - 7, 13, corTexto, ESQ, 0, "%s", it->nome);
        float wv = uiTexto(x + w - 12, cy - 6, 11, uiMistura(COR_MUDO, COR_TEXTO, d), DIR, 0, "v" APP_VERSION);
        uiDica(x + w - 12 - wv - 26, cy - UI_DICA / 2.0f, "Y", NULL);
        return;
    }
    float cx = x + w / 2;
    uiIcone(it->icone, cx, y + h / 2 - 8, 26, corIcone, fundo);
    float tam = uiLarguraTexto(it->nome, 12) > TILE_W - 8 ? 11 : 12;
    uiTexto(cx, y + h - 20, tam, corTexto, CENTRO, w - 6, "%s", it->nome);
}

static void desenharMenu(int sel, int apertado, bool temIP) {
    const Item *it = &itens[sel];
    bool cel = alvoCelular();
    animar(sel, apertado);

    // ---- tela de cima: o módulo escolhido em destaque, na cor dele
    uiTelaGrad(alvoCima, uiMistura(COR_FUNDO, anim.cor, 0.22f), COR_FUNDO);
    uiBarraStatus("3DS Hub", IC_CONTROLE, COR_TEXTO);

    u32 corCartao = uiMistura(COR_CARTAO, anim.cor, 0.12f);
    uiCartaoCor(12, 30, 376, 130, 14, corCartao, uiMistura(COR_BORDA, anim.cor, 0.45f));
    uiIcone(it->icone, 322, 95, 112, uiMistura(corCartao, anim.cor, 0.2f), corCartao);    // marca d'água

    float t = 60 + 8 * anim.salto;              // o quadrado do ícone salta e volta
    uiRetArred(56 - t / 2, 72 - t / 2, t, t, 16, anim.cor);
    uiIcone(it->icone, 56, 72, 34 + 5 * anim.salto, COR_BRANCO, anim.cor);
    uiTexto(100, 44, 22, COR_TEXTO, ESQ, 276, "%s", it->nome);
    uiTexto(100, 72, 13, anim.corClara, ESQ, 276, "%s", cel && it->subCelular ? it->subCelular : it->subtitulo);
    if (cel && !it->noCelular) {
        uiRetArred(100, 90, 84, 15, 7.5f, COR_CARTAO2);
        uiTexto(142, 92, 10, COR_TEXTO2, CENTRO, 0, "Só com o PC");
    }
    uiTexto(26, 112, 13, COR_TEXTO2, ESQ, 0, "%s", it->descricao);

    // ---- avisos (ou as dicas dos botões)
    if (osGetTime() < confirmarSaidaAte)
        faixa(COR_CARTAO2, COR_TEXTO, -1, "Aperte START de novo para sair do 3DS Hub");
    else if (osGetTime() < avisoAte)
        faixa(HEX(0x085041), COR_AGUA_CL, IC_OK, aviso);
    else if (outroNaRede && temIP)
        faixa(HEX(0x4A3410), COR_AMARELO, -1, "Não responde. SELECT escolhe outro aparelho");
    else
        uiDicas(200, 171, CENTRO, "A", "abrir", "Y", "ajustes", "SELECT", "trocar de PC ou celular", NULL);

    // ---- rodapé: qual aparelho e se o Hub respondeu
    bool hubOk = hubRespondendo();
    u32 cor = !temIP ? COR_VERMELHO : hubOk ? COR_AGUA_CL : COR_AMARELO;
    const char *estado = !temIP ? "3DS sem Wi-Fi" : hubOk ? (modoRemoto ? "Conectado pela internet"
                                                                          : cel ? "Celular encontrado" : "Hub encontrado")
                                                          : (modoRemoto ? "Conectando pela internet..." : "Procurando o Hub...");
    uiCartao(12, 198, 376, 32, 12);
    float w = uiTexto(378, 207, 13, cor, DIR, 0, "%s", estado);
    // procurando: a bolinha pulsa; achou: fica acesa
    float pulso = hubOk || !temIP ? 1 : 0.45f + 0.55f * fabsf(sinf(osGetTime() / 380.0f));
    uiCirculo(378 - w - 10, 214, 4, uiMistura(COR_CARTAO, cor, pulso));
    float wNome = 378 - w - 20 - 44 - 8;
    uiIcone(IC_PC, 30, 214, 15, COR_MUDO, COR_CARTAO);   // (o mesmo ícone para PC e celular)
    if (modoRemoto)     uiTexto(44, 207, 13, COR_TEXTO2, ESQ, wNome, "%s (pela internet)", nomePC[0] ? nomePC : "PC");
    else if (!ipPC[0])  uiTexto(44, 207, 13, COR_MUDO, ESQ, wNome, "Procurando...");
    else if (nomePC[0]) uiTexto(44, 207, 13, COR_TEXTO2, ESQ, wNome, "%s (%s)", nomePC, ipPC);
    else                uiTexto(44, 207, 13, COR_TEXTO2, ESQ, wNome, "PC %s", ipPC);

    if (anim.entrada > 0.01f) uiRet(0, 0, 400, 240, uiAlfa(anim.corEntrada, anim.entrada));

    // ---- tela de baixo: a grade. O escolhido é desenhado por último (fica por cima).
    uiTela(alvoBaixo);
    for (int i = 0; i < N_ITENS; i++)
        if (i != sel) desenharTile(i, cel);
    desenharTile(sel, cel);
    if (anim.entrada > 0.01f) uiRet(0, 0, 320, 240, uiAlfa(anim.corEntrada, anim.entrada));
}

// Abrir um módulo: a cor dele cresce a partir do tile e toma as duas telas.
#define QUADROS_ABRIR 9
static void transicaoAbrir(int i, bool temIP) {
    const Item *it = &itens[i];
    float x0 = tileX(i), y0 = tileY(i), w0 = tileW(i), h0 = tileH(i);
    for (int q = 1; q <= QUADROS_ABRIR && aptMainLoop(); q++) {
        float f = (float)q / QUADROS_ABRIR;
        f = 1 - (1 - f) * (1 - f);              // rápido no começo, suave no fim
        uiQuadro();
        desenharMenu(i, -1, temIP);
        C2D_SceneBegin(alvoCima);
        uiRet(0, 0, 400, 240, uiAlfa(it->cor, f));
        C2D_SceneBegin(alvoBaixo);
        float x = x0 * (1 - f), y = y0 * (1 - f), w = w0 + (320 - w0) * f, h = h0 + (240 - h0) * f;
        uiRetArred(x, y, w, h, 12 * (1 - f), it->cor);
        uiIcone(it->icone, x + w / 2, y + h / 2, 26 + 38 * f, COR_BRANCO, it->cor);
        uiFim();
    }
    // na volta, a cor some por cima do menu
    anim.entrada = 1;
    anim.corEntrada = it->cor;
}

int main(int argc, char **argv) {
    // de onde o .3dsx foi aberto (o Homebrew Launcher passa o caminho): a atualização
    // pelo Wi-Fi grava a versão nova por cima desse arquivo
    if (argc > 0 && argv[0] && !strncmp(argv[0], "sdmc:/", 6))
        snprintf(caminhoApp, sizeof(caminhoApp), "%s", argv[0]);

    updater_init("ChunoB/3DSHub", APP_VERSION, "3dshub.3dsx", caminhoApp);
    romfsInit();
    gfxInitDefault();
    osSetSpeedupEnable(true);   // New 3DS: 804 MHz e cache L2 (no Old não faz nada)
    uiIniciar();

    u32 *socBuf = memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    bool rede = socBuf && R_SUCCEEDED(socInit(socBuf, SOC_BUFFERSIZE));
    curl_global_init(CURL_GLOBAL_DEFAULT);
    bool procurouVersao = false, perguntouVersao = false;
    inicioApp = osGetTime();
    srand((unsigned)svcGetSystemTick());

    carregarIP();
    if (rede) {
        abrirSonda();
        descoberta = descobertaAbrir();
    }
    hidSetRepeatParameters(22, 7);      // segurar a cruz ou o circle pad anda sozinho
    int sel = lerUltimoModulo();
    uiSomPermitir(true);
    int apertado = -1;                  // tile com o dedo em cima (abre ao soltar)
    touchPosition toque = { 0 };
    u32 quadro = 0;
    bool temIP = false;
    bool esperarSoltarStart = false;    // o START que fechou um módulo ainda está apertado

    while (aptMainLoop() && !sairDoApp) {
        hidScanInput();
        u32 down = hidKeysDown();
        int abrir = -1, selAntes = sel;

        if (esperarSoltarStart) {
            if (hidKeysHeld() & KEY_START) down &= ~KEY_START;
            else esperarSoltarStart = false;
        }
        // sair pede dois toques no START. apertouStart: START durante uma espera da
        // conexão pela internet (que segura o menu) conta como o primeiro toque.
        if ((down & KEY_START) || apertouStart) {
            apertouStart = false;
            if (osGetTime() < confirmarSaidaAte) break;
            confirmarSaidaAte = osGetTime() + 2500;
            uiSomTocar(SOM_VOLTAR);
        }
        if ((down & KEY_SELECT) && rede) {
            uiSomTocar(SOM_ABRIR);
            escolherAparelho();
            if (aptShouldClose()) break;
            fecharSondaDeVez();
            ultimaRespostaHub = 0;
            outroNaRede = false;
            redeMudou = false;
            if (modoRemoto) mensagemRapida("Conectando pela internet...", nomePC);
            abrirSonda();
            esperarSoltarStart = true;
        }
        u32 rep = hidKeysDownRepeat();
        if (rep & KEY_LEFT)  sel = mover(sel, -1, 0);
        if (rep & KEY_RIGHT) sel = mover(sel, 1, 0);
        if (rep & KEY_UP)    sel = mover(sel, 0, -1);
        if (rep & KEY_DOWN)  sel = mover(sel, 0, 1);
        if (rep & KEY_L)     sel = mover(sel, -1, 0);
        if (rep & KEY_R)     sel = mover(sel, 1, 0);
        if (down & KEY_A) abrir = sel;
        if (down & KEY_Y) abrir = ITEM_AJUSTES;

        // toque: o tile afunda com o dedo e abre ao soltar em cima dele (arrastar
        // para fora desiste)
        if (hidKeysHeld() & KEY_TOUCH) {
            hidTouchRead(&toque);
            int t = tileNoToque(&toque);
            if (down & KEY_TOUCH) apertado = t;
            else if (t != apertado) apertado = -1;
            if (apertado >= 0) sel = apertado;
        } else if (apertado >= 0) {
            if (hidKeysUp() & KEY_TOUCH) abrir = apertado;
            apertado = -1;
        }
        if (abrir >= 0) sel = abrir;
        else if (sel != selAntes) uiSomTocar(SOM_MOVER);

        // versão nova na internet: procura sozinho alguns segundos depois de abrir
        // e pergunta uma vez se quer atualizar
        if (rede && temIP && !procurouVersao && osGetTime() - inicioApp > 3000) {
            procurouVersao = true;
            updater_check();
        }
        if (!perguntouVersao && updater_state() == UPD_AVAILABLE) {
            perguntouVersao = true;
            atualizarPelaInternet(true);
            if (sairDoApp) break;
        }

        if (quadro % 60 == 0) temIP = rede && gethostid() != 0;
        if (rede) {
            rodarSonda(quadro);
            rodarDescoberta(quadro);
            rodarRemoto(temIP);
        }
        quadro++;

        if (abrir >= 0) {
            const Item *it = &itens[abrir];
            if (alvoCelular() && !it->noCelular) {
                uiSomTocar(SOM_ERRO);
                uiAviso(it->nome, "Este módulo só funciona com o PC.", it->icone, it->cor);
            } else if (it->precisaPC && !rede) {
                uiSomTocar(SOM_ERRO);
                uiAviso("Rede do 3DS", "Não consegui iniciar a rede do 3DS.\nReinicie o app.", IC_X, COR_VERMELHO);
            } else if (it->precisaPC && !modoRemoto && !ipPC[0] && !pedirIP()) {
                // ainda sem PC e cancelou o IP: continua no menu (a descoberta segue tentando)
            } else {
                gravarUltimoModulo(abrir);
                uiSomTocar(SOM_ABRIR);
                transicaoAbrir(abrir, temIP);
                fecharSonda();
                // os módulos cuidam do próprio som (NDSP): o da interface sai de cena.
                // Só os Ajustes continuam com ele.
                if (abrir != ITEM_AJUSTES) uiSomPermitir(false);
                it->abrir();
                uiSomPermitir(true);
                if (aptShouldClose()) break;   // fechado pelo HOME dentro do módulo: sai já
                apertouStart = false;       // o START dentro do módulo era para sair dele
                esperarSoltarStart = true;
                hidSetRepeatParameters(22, 7);      // (a Steam e os Ajustes usam outro ritmo)
                ultimaRespostaHub = 0;
                if (redeMudou) {            // nos Ajustes escolheu outro PC (ou casa/internet)
                    redeMudou = false;
                    fecharSondaDeVez();
                    if (modoRemoto) mensagemRapida("Conectando pela internet...", nomePC);
                }
                if (rede) abrirSonda();     // o IP pode ter mudado nos Ajustes
            }
        }

        uiQuadro();
        desenharMenu(sel, apertado, temIP);
        uiFim();
    }

    updater_exit();
    fecharSondaDeVez();             // pela internet a sonda é um túnel: fecha também
    if (descoberta >= 0) close(descoberta);
    curl_global_cleanup();
    // Só socExit(), como nos outros apps. O SOCU_ShutdownSockets() que havia aqui é o mesmo
    // "finalizar" que o socExit() já faz: finalizar duas vezes travava o fechamento pelo
    // HOME no console de verdade (a tela "fechando" não saía nunca).
    if (rede) socExit();
    uiEncerrar();
    gfxExit();
    romfsExit();
    return 0;
}
