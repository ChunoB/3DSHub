// 3DS Hub — menu que junta Controle, Segunda Tela, Sintetizador, Câmera, Espelho e Ajustes.
//
// Menu: cruz ou toque = escolher, A = abrir, Y = Ajustes, START = sair
//
// O menu procura o PC sozinho (descoberta na rede) sempre que o Hub não responde.

#include "comum.h"
#include "updater.h"
#include "tunel.h"

#include <curl/curl.h>
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
} Item;

static const Item itens[] = {
    { "Controle", "Xbox, teclado e mouse no PC",
      "Use o 3DS como controle de Xbox, teclado ou\ntouchpad. Mira por giroscópio com SELECT+B.",
      IC_CONTROLE, COR_ROXO, COR_ROXO_CL, modControle, true },
    { "Segunda Tela", "Gráficos, música e atalhos",
      "CPU, GPU, RAM e rede do PC em tempo real,\ncontrole de música e volume e atalhos.",
      IC_GRAFICO, COR_VERDE_AGUA, COR_AGUA_CL, modTela, true },
    { "Sintetizador", "Teclado e theremin",
      "Toque na tela de baixo para tocar. Inclinar\no console entorta a nota. Não precisa do PC.",
      IC_PIANO, COR_ROSA, COR_ROSA_CL, modSintetizador, false },
    { "Câmera", "Webcam no OBS, até em 3D",
      "Manda a câmera do 3DS para o OBS do PC.\nTraseira, frontal ou as duas em 3D.",
      IC_CAMERA, COR_CORAL, COR_CORAL_CL, modCamera, true },
    { "Espelho", "Uma janela do PC aqui",
      "Mostra uma janela do PC na tela de cima, com\nzoom (R/L). Na aba Clicar, toque para clicar.",
      IC_ESPELHO, COR_AZUL, COR_AZUL_CL, modEspelho, true },
    { "Ajustes", "PC, atualização e giroscópio",
      "Procurar o PC, atualizar o app pelo Wi-Fi e\najustar o giroscópio e o touchpad.",
      IC_ENGRENAGEM, COR_CARTAO2, COR_TEXTO2, modAjustes, false },
};
#define N_ITENS (int)(sizeof(itens) / sizeof(itens[0]))
#define ITEM_AJUSTES (N_ITENS - 1)

// grade 2x3 na tela de baixo
#define TILE_W 145
#define TILE_H 70
static float tileX(int i) { return 10 + (i % 2) * (TILE_W + 10); }
static float tileY(int i) { return 8 + (i / 2) * (TILE_H + 7); }

// ---------------------------------------------------------------- sonda do Hub
// Manda um "ping" (3DSP) para a Segunda Tela do PC: se responder, o Hub está no ar.
// (É um ping separado do "oi" da Segunda Tela, para o PC não achar que ela está aberta.)

static int sonda = -1;
static struct sockaddr_in destinoSonda;
static u64 ultimaRespostaHub;
static int descoberta = -1;
static char aviso[64];          // "PC encontrado: ..." por alguns segundos
static u64 avisoAte;
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
    uiTexto(200, 90, 18, COR_TEXTO, CENTRO, 360, "%s", titulo);
    uiTexto(200, 122, 13, COR_TEXTO2, CENTRO, 360, "%s", texto);
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


// Pergunta de vez em quando quem é o 3DS Hub da rede. Só troca de PC sozinho
// quando o PC atual não responde (ou ainda não tem IP): assim, com dois PCs
// na mesma rede, ele não fica pulando de um para o outro.
static void rodarDescoberta(u32 quadro) {
    if (descoberta < 0) return;
    bool hubOk = hubRespondendo();
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
            snprintf(nomePC, sizeof(nomePC), "%s", nome);   // o mesmo PC: só aprende o nome
        } else if (!ipPC[0] || !hubOk) {
            fecharSonda();
            definirIP(ip);
            snprintf(nomePC, sizeof(nomePC), "%s", nome);
            ultimaRespostaHub = 0;
            abrirSonda();
            snprintf(aviso, sizeof(aviso), "PC encontrado: %s", nome);
            avisoAte = osGetTime() + 4000;
        }
    }
}

// ---------------------------------------------------------------- desenho

static void desenharMenu(int sel, bool temIP) {
    const Item *it = &itens[sel];

    uiTela(alvoCima);
    uiBarraStatus("3DS Hub", IC_CONTROLE, COR_TEXTO);

    uiRetArred(16, 36, 64, 64, 16, it->cor);
    uiIcone(it->icone, 48, 68, 38, COR_BRANCO, it->cor);
    uiTexto(94, 42, 22, COR_TEXTO, ESQ, 290, "%s", it->nome);
    uiTexto(94, 72, 13, it->corClara, ESQ, 290, "%s", it->subtitulo);
    uiTexto(16, 114, 13, COR_TEXTO2, ESQ, 0, "%s", it->descricao);

    if (osGetTime() < avisoAte) {
        uiRetArred(16, 166, 368, 28, 10, HEX(0x085041));
        uiIcone(IC_OK, 32, 180, 14, COR_AGUA_CL, HEX(0x085041));
        uiTexto(46, 172, 13, COR_AGUA_CL, ESQ, 330, "%s", aviso);
    }

    // rodapé: qual PC e se o Hub respondeu
    uiRet(16, 202, 368, 1, COR_LINHA);
    uiIcone(IC_PC, 26, 220, 14, COR_MUDO, COR_FUNDO);
    if (modoRemoto)     uiTexto(38, 212, 13, COR_MUDO, ESQ, 190, "%s (pela internet)", nomePC[0] ? nomePC : "PC");
    else if (!ipPC[0])  uiTexto(38, 212, 13, COR_MUDO, ESQ, 190, "PC: procurando...");
    else if (nomePC[0]) uiTexto(38, 212, 13, COR_MUDO, ESQ, 190, "%s (%s)", nomePC, ipPC);
    else                uiTexto(38, 212, 13, COR_MUDO, ESQ, 190, "PC %s", ipPC);
    bool hubOk = hubRespondendo();
    u32 cor = !temIP ? COR_VERMELHO : hubOk ? COR_AGUA_CL : COR_AMARELO;
    const char *estado = !temIP ? "3DS sem Wi-Fi" : hubOk ? (modoRemoto ? "Conectado pela internet" : "Hub encontrado")
                                                          : (modoRemoto ? "Conectando pela internet..." : "Procurando o Hub...");
    float w = uiTexto(384, 212, 13, cor, DIR, 0, "%s", estado);
    uiIcone(hubOk && temIP ? IC_OK : IC_X, 384 - w - 10, 220, 12, cor, COR_FUNDO);

    uiTela(alvoBaixo);
    for (int i = 0; i < N_ITENS; i++) {
        bool s = i == sel;
        uiBotao(tileX(i), tileY(i), TILE_W, TILE_H, itens[i].nome, itens[i].icone,
                s ? itens[i].cor : COR_CARTAO, s ? COR_BRANCO : itens[i].corClara);
    }
}

int main(int argc, char **argv) {
    // de onde o .3dsx foi aberto (o Homebrew Launcher passa o caminho): a atualização
    // pelo Wi-Fi grava a versão nova por cima desse arquivo
    if (argc > 0 && argv[0] && !strncmp(argv[0], "sdmc:/", 6))
        snprintf(caminhoApp, sizeof(caminhoApp), "%s", argv[0]);

    updater_init("ChunoB/3DSHub", APP_VERSION, "3dshub.3dsx", caminhoApp);
    romfsInit();
    gfxInitDefault();
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
    int sel = 0;
    u32 quadro = 0;
    bool temIP = false;

    while (aptMainLoop() && !sairDoApp) {
        hidScanInput();
        u32 down = hidKeysDown();
        int abrir = -1;

        // apertouStart: START durante uma conexão pela internet (que segura o menu)
        if ((down & KEY_START) || apertouStart) break;
        if (down & KEY_DLEFT)  sel = sel % 2 ? sel - 1 : sel;
        if (down & KEY_DRIGHT) sel = sel % 2 ? sel : sel + 1;
        if (down & KEY_DUP)    sel = sel >= 2 ? sel - 2 : sel;
        if (down & KEY_DDOWN)  sel = sel + 2 < N_ITENS ? sel + 2 : sel;
        if (down & KEY_A) abrir = sel;
        if (down & KEY_Y) abrir = ITEM_AJUSTES;

        if (down & KEY_TOUCH) {
            touchPosition tp;
            hidTouchRead(&tp);
            for (int i = 0; i < N_ITENS; i++)
                if (uiDentro(&tp, tileX(i), tileY(i), TILE_W, TILE_H)) sel = abrir = i;
        }

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
            if (it->precisaPC && !rede) {
                uiAviso("Rede do 3DS", "Não consegui iniciar a rede do 3DS.\nReinicie o app.", IC_X, COR_VERMELHO);
            } else if (it->precisaPC && !modoRemoto && !ipPC[0] && !pedirIP()) {
                // ainda sem PC e cancelou o IP: continua no menu (a descoberta segue tentando)
            } else {
                fecharSonda();
                it->abrir();
                if (aptShouldClose()) break;   // fechado pelo HOME dentro do módulo: sai já
                apertouStart = false;       // o START dentro do módulo era para sair dele
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
        desenharMenu(sel, temIP);
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
