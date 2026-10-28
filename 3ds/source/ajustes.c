// Ajustes — procurar o PC, digitar o IP, atualizar o app pelo Wi-Fi e mudar as
// configurações do giroscópio e do touchpad (gravadas no config.json do PC).
//
//   cima/baixo ou toque : escolher
//   esquerda/direita, - e + : mudar o valor
//   A : fazer a ação / ligar e desligar
//   START ou B : voltar ao menu

#include "comum.h"
#include "updater.h"
#include "tunel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#define PORTA       5010
#define ARQ_CRC_CIA "sdmc:/3ds/3dshub_cia.crc"   // versão do CIA instalado pela atualização

enum { PROCURAR, DIGITAR_IP, ATUALIZAR, ATUALIZAR_NET, REMOTO, NUMERO, SIM_NAO };

typedef struct {
    int tipo;
    const char *rotulo, *chave, *unidade, *ajuda;
    float passo, minimo, maximo;
} Linha;

static const Linha linhas[] = {
    { PROCURAR,   "Procurar o PC na rede", NULL, NULL,
      "Pergunta para a rede quais PCs estão com o\n3DS Hub aberto e deixa você escolher um.", 0, 0, 0 },
    { DIGITAR_IP, "Digitar o IP do PC", NULL, NULL,
      "Para quando a busca não acha o PC. O IP\naparece no menu do ícone do 3DS Hub no PC.", 0, 0, 0 },
    { REMOTO,     "Acesso pela internet", NULL, NULL,
      "Pareia um PC para usar longe de casa: na mesma\nrede é automático; longe dela, digite a senha\ndo PC. Vale para vários PCs.", 0, 0, 0 },
    { ATUALIZAR_NET, "Atualizar pela internet", NULL, NULL,
      "Baixa do GitHub a versão mais nova do 3DS Hub\n(precisa de internet no Wi-Fi do 3DS).", 0, 0, 0 },
    { ATUALIZAR,  "Atualizar pelo PC", NULL, NULL,
      "Baixa do PC a versão do 3DS Hub que está na\npasta do Hub, sem tirar o cartão SD.", 0, 0, 0 },
    { NUMERO,     "Giroscópio: velocidade", "giro_pixels_por_grau", "px/°",
      "Quanto a mira anda (no mouse) para cada grau\nque você gira o console. Maior = mais rápido.", 1, 1, 80 },
    { NUMERO,     "Giroscópio: analógico", "giro_analogico_graus", "°/s",
      "Quão rápido girar para o analógico chegar no\nmáximo (perfis com giroscópio no analógico).", 10, 20, 600 },
    { NUMERO,     "Giroscópio: zona morta", "giro_zona_morta", "°/s",
      "Movimentos mais lentos que isso são ignorados\n(tremida da mão). Maior = mira mais firme.", 0.5f, 0, 15 },
    { SIM_NAO,    "Giroscópio: inverter lados", "giro_inverter_horizontal", NULL,
      "Ligue se a mira vai para a esquerda quando\nvocê gira o console para a direita.", 0, 0, 1 },
    { SIM_NAO,    "Giroscópio: inverter cima", "giro_inverter_vertical", NULL,
      "Ligue se a mira sobe quando você inclina o\nconsole para baixo.", 0, 0, 1 },
    { NUMERO,     "Touchpad: velocidade", "sensibilidade_toque", "x",
      "Quanto o mouse anda para cada pixel que o\ndedo anda na tela de toque.", 0.25f, 0.25f, 10 },
    { NUMERO,     "Analógico direito: sensib.", "analogico_direito_sensibilidade", "x",
      "C-stick e analógico de toque (SELECT+X).\nMaior = chega no máximo mexendo menos.", 0.1f, 0.2f, 4 },
};
#define N_LINHAS (int)(sizeof(linhas) / sizeof(linhas[0]))

static float valores[N_LINHAS];
static bool temValores;          // conseguiu ler as configurações do PC
static int sock = -1;
static char status[96];          // mensagem curta sobre a última ação

// ---------------------------------------------------------------- telas auxiliares

static void desenharMensagem(const char *titulo, const char *texto, float progresso, const char *rodape) {
    uiQuadro();
    uiTela(alvoCima);
    uiBarraStatus("Ajustes", IC_ENGRENAGEM, COR_TEXTO2);
    uiTexto(200, 70, 20, COR_TEXTO, CENTRO, 360, "%s", titulo);
    uiTexto(200, 104, 13, COR_TEXTO2, CENTRO, 0, "%s", texto);
    if (progresso >= 0) uiBarra(60, 160, 280, 12, progresso, COR_AGUA_CL, COR_CARTAO);
    uiTela(alvoBaixo);
    if (rodape) uiTexto(160, 110, 14, COR_MUDO, CENTRO, 300, "%s", rodape);
    uiFim();
}

// Pergunta sim/não com dois botões grandes. A/toque no da esquerda = sim.
static bool confirmar(const char *titulo, const char *texto, const char *sim, const char *nao) {
    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        touchPosition tp;
        hidTouchRead(&tp);
        if (down & KEY_A) return true;
        if (down & (KEY_B | KEY_START)) return false;
        if (down & KEY_TOUCH) {
            if (uiDentro(&tp, 10, 150, 145, 60)) return true;
            if (uiDentro(&tp, 165, 150, 145, 60)) return false;
        }
        uiQuadro();
        uiTela(alvoCima);
        uiBarraStatus("Ajustes", IC_ENGRENAGEM, COR_TEXTO2);
        uiTexto(200, 64, 20, COR_TEXTO, CENTRO, 360, "%s", titulo);
        uiTexto(200, 100, 13, COR_TEXTO2, CENTRO, 0, "%s", texto);
        uiTela(alvoBaixo);
        uiBotao(10, 150, 145, 60, sim, IC_OK, COR_VERDE_AGUA, COR_AGUA_ESC);
        uiBotao(165, 150, 145, 60, nao, IC_X, COR_CARTAO, COR_TEXTO);
        uiFim();
    }
    return false;
}

// ---------------------------------------------------------------- conversa com o PC

static void desconectar(void) {
    if (sock >= 0) fecharRede(sock);
    sock = -1;
    temValores = false;
}

static bool lerResposta(const char *magic, u32 *valor) {
    char cab[8];
    if (!receberTudo(sock, cab, 8, 5000) || memcmp(cab, magic, 4)) return false;
    memcpy(valor, cab + 4, 4);
    return true;
}

static void lerConfiguracoes(void) {
    temValores = false;
    u32 tam;
    static char texto[512];
    if (!enviarTudo(sock, "C\n", 2, 3000) || !lerResposta("3DSC", &tam) || tam >= sizeof(texto) ||
        (tam && !receberTudo(sock, texto, tam, 5000))) {
        desconectar();
        return;
    }
    texto[tam] = 0;
    char *salvo;
    for (char *l = strtok_r(texto, "\n", &salvo); l; l = strtok_r(NULL, "\n", &salvo)) {
        char *igual = strchr(l, '=');
        if (!igual) continue;
        *igual = 0;
        for (int i = 0; i < N_LINHAS; i++)
            if (linhas[i].chave && !strcmp(linhas[i].chave, l)) valores[i] = atof(igual + 1);
    }
    temValores = true;
}

static void conectar(void) {
    desconectar();
    if (!ipPC[0] && !modoRemoto) return;
    desenharMensagem("Conectando ao PC...", modoRemoto ? "pela internet" : ipPC, -1, "B ou START: cancelar");
    sock = conectarTCP(PORTA, 3000);
    if (sock < 0) {
        sock = -1;
        snprintf(status, sizeof(status), "Sem conexão com o PC (%s)", ipPC);
        return;
    }
    lerConfiguracoes();
    if (temValores) status[0] = 0;
    else snprintf(status, sizeof(status), "O PC não respondeu aos ajustes (o Hub do PC está atualizado?)");
}

static void gravar(int i) {
    if (sock < 0) return;
    char cmd[80];
    int n = linhas[i].tipo == SIM_NAO
        ? snprintf(cmd, sizeof(cmd), "W %s %d\n", linhas[i].chave, valores[i] != 0)
        : snprintf(cmd, sizeof(cmd), "W %s %g\n", linhas[i].chave, valores[i]);
    u32 ok;
    if (!enviarTudo(sock, cmd, n, 3000) || !lerResposta("3DSW", &ok)) {
        desconectar();
        snprintf(status, sizeof(status), "A conexão com o PC caiu");
    }
}

// ---------------------------------------------------------------- procurar o PC

// A lista mostra os PCs que responderam nesta rede (conexão direta) e, embaixo, os PCs
// pareados para a internet. Escolher um da internet passa a usar o túnel na hora.
// Um PC pareado que também está nesta rede aparece só uma vez, pela rede (mais rápido).
#define LINHAS_VISIVEIS 5

static bool procurarPC(void) {
    int s = descobertaAbrir();
    char ips[6][32], nomes[6][32];
    int n = 0, sel = 0, topo = 0;
    u64 inicio = osGetTime(), ultimaPergunta = 0;
    bool escolheu = false;

    while (aptMainLoop()) {
        // PCs da internet que não estão nesta rede
        int remotos[TUNEL_MAX_PCS], nr = 0;
        for (int k = 0; k < tunelQuantos(); k++) {
            bool aqui = false;
            for (int i = 0; i < n; i++) aqui |= !strcmp(nomes[i], tunelNomePC(k));
            if (!aqui) remotos[nr++] = k;
        }
        int total = n + nr;
        if (sel >= total) sel = total ? total - 1 : 0;

        hidScanInput();
        u32 down = hidKeysDown();
        touchPosition tp;
        hidTouchRead(&tp);
        if (down & (KEY_B | KEY_START)) break;
        if ((down & KEY_DUP) && sel > 0) sel--;
        if ((down & KEY_DDOWN) && sel < total - 1) sel++;
        if (sel < topo) topo = sel;
        if (sel >= topo + LINHAS_VISIVEIS) topo = sel - LINHAS_VISIVEIS + 1;
        int escolha = -1;
        if ((down & KEY_A) && total > 0) escolha = sel;
        if (down & KEY_TOUCH)
            for (int i = topo; i < total && i < topo + LINHAS_VISIVEIS; i++)
                if (uiDentro(&tp, 10, 40 + (i - topo) * 34, 300, 30)) escolha = i;
        if (escolha >= 0 && escolha < n) {               // nesta rede: conexão direta
            definirIP(ips[escolha]);
            snprintf(nomePC, sizeof(nomePC), "%s", nomes[escolha]);
            if (modoRemoto) { modoRemoto = false; redeMudou = true; }
            escolheu = true;
            break;
        }
        if (escolha >= n) {                              // pela internet
            int k = remotos[escolha - n];
            tunelEscolher(k);
            snprintf(nomePC, sizeof(nomePC), "%s", tunelNomePC(k));
            modoRemoto = true;
            redeMudou = true;
            escolheu = true;
            break;
        }

        // pergunta a cada meio segundo nos primeiros 4 segundos
        bool procurando = osGetTime() - inicio < 4000;
        if (s >= 0 && procurando && osGetTime() - ultimaPergunta > 500) {
            descobertaPerguntar(s);
            ultimaPergunta = osGetTime();
        }
        char ip[32], nome[32];
        while (s >= 0 && n < 6 && descobertaResposta(s, ip, sizeof(ip), nome, sizeof(nome))) {
            bool repetido = false;
            for (int i = 0; i < n; i++) repetido |= !strcmp(ips[i], ip);
            if (repetido) continue;
            snprintf(ips[n], sizeof(ips[0]), "%s", ip);
            snprintf(nomes[n], sizeof(nomes[0]), "%s", nome);
            n++;
        }

        uiQuadro();
        uiTela(alvoCima);
        uiBarraStatus("Ajustes", IC_ENGRENAGEM, COR_TEXTO2);
        uiTexto(200, 70, 20, COR_TEXTO, CENTRO, 0, "%s", total ? "Escolha o PC" :
                procurando ? "Procurando PCs..." : "Nenhum PC encontrado");
        uiTexto(200, 104, 13, COR_TEXTO2, CENTRO, 0, "%s", total || procurando
                ? "Nesta rede: conexão direta (mais rápida).\nPela internet: os PCs que você pareou."
                : "Confira se o 3DS Hub está aberto no PC e se\no 3DS e o PC estão na mesma rede (ou hotspot).");
        if (procurando && total) uiTexto(200, 150, 13, COR_MUDO, CENTRO, 0, "Ainda procurando nesta rede...");
        uiTela(alvoBaixo);
        uiTexto(10, 12, 13, COR_MUDO, ESQ, 0, "PCs (%d)", total);
        for (int i = topo; i < total && i < topo + LINHAS_VISIVEIS; i++) {
            bool s2 = i == sel;
            float y = 40 + (i - topo) * 34;
            bool rede = i < n;
            const char *nomeLinha = rede ? nomes[i] : tunelNomePC(remotos[i - n]);
            bool emUso = !rede ? modoRemoto && remotos[i - n] == tunelAtual() : !modoRemoto && !strcmp(ips[i], ipPC);
            uiRetArred(10, y, 300, 30, 8, s2 ? (rede ? COR_VERDE_AGUA : COR_AZUL) : COR_CARTAO);
            uiTexto(20, y + 7, 13, s2 ? COR_BRANCO : COR_TEXTO, ESQ, 160, "%s%s", nomeLinha, emUso ? "  (em uso)" : "");
            uiTexto(300, y + 7, 13, s2 ? COR_BRANCO : rede ? COR_MUDO : COR_AZUL_CL, DIR, 0, "%s", rede ? ips[i] : "pela internet");
        }
        uiTexto(160, 222, 11, COR_MUDO, CENTRO, 0, "A ou toque: escolher   B: voltar");
        uiFim();
    }
    if (s >= 0) close(s);
    return escolheu;
}

// ---------------------------------------------------------------- atualizar o app

static u32 crcDoArquivo(const char *caminho) {
    FILE *f = fopen(caminho, "rb");
    if (!f) return 0;
    static u8 buf[32 * 1024];
    u32 crc = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) crc = crc32(crc, buf, n);
    fclose(f);
    return crc;
}

// Recebe 'tam' bytes do PC em pedaços, mostrando o progresso. 'gravar' recebe
// cada pedaço; devolve false se der erro. B cancela.
static bool baixar(u32 tam, bool (*gravarPedaco)(const u8 *, u32, u32, void *), void *ctx, u32 *crc) {
    static u8 buf[32 * 1024];
    u32 feito = 0;
    *crc = 0;
    while (feito < tam) {
        u32 n = tam - feito > sizeof(buf) ? sizeof(buf) : tam - feito;
        if (!receberTudo(sock, buf, n, 8000)) return false;
        if (!gravarPedaco(buf, n, feito, ctx)) return false;
        *crc = crc32(*crc, buf, n);
        feito += n;
        hidScanInput();
        if (hidKeysDown() & KEY_B) return false;
        char texto[64];
        snprintf(texto, sizeof(texto), "%lu de %lu KB", feito / 1024, tam / 1024);
        desenharMensagem("Baixando a versão nova...", texto, (float)feito / tam, "B: cancelar");
    }
    return true;
}

static bool gravarNoArquivo(const u8 *dados, u32 n, u32 offset, void *ctx) {
    (void)offset;
    return fwrite(dados, 1, n, (FILE *)ctx) == n;
}

static bool gravarNoCia(const u8 *dados, u32 n, u32 offset, void *ctx) {
    u32 escritos;
    return R_SUCCEEDED(FSFILE_Write(*(Handle *)ctx, &escritos, offset, dados, n, 0)) && escritos == n;
}

// Troca o .3dsx com segurança: o antigo só sai depois que o novo chegou inteiro e
// conferido, e se a troca falhar no meio o antigo volta para o lugar.
static bool trocarArquivo(const char *novo) {
    char antigo[300];
    snprintf(antigo, sizeof(antigo), "%s.anterior", caminhoApp);
    remove(antigo);
    if (rename(caminhoApp, antigo) != 0) return false;
    if (rename(novo, caminhoApp) != 0) {
        rename(antigo, caminhoApp);
        return false;
    }
    remove(antigo);
    return true;
}

static void atualizar(void) {
    if (sock < 0) conectar();
    if (sock < 0) {
        uiAviso("Sem conexão", "Não consegui falar com o PC para atualizar.\nConfira o PC em Ajustes.", IC_X, COR_AMARELO);
        return;
    }
    bool ehCia = !envIsHomebrew();
    const char *tipo = ehCia ? "cia" : "3dsx";

    // o que o PC tem
    char cmd[16];
    int n = snprintf(cmd, sizeof(cmd), "I %s\n", tipo);
    char cab[12];
    if (!enviarTudo(sock, cmd, n, 3000) || !receberTudo(sock, cab, 12, 5000) || memcmp(cab, "3DSI", 4)) {
        desconectar();
        uiAviso("Sem conexão", "A conexão com o PC caiu.", IC_X, COR_AMARELO);
        return;
    }
    u32 tamPC, crcPC;
    memcpy(&tamPC, cab + 4, 4);
    memcpy(&crcPC, cab + 8, 4);
    if (!tamPC) {
        uiAviso("Nada para baixar", ehCia
                ? "O PC não tem o 3dshub.cia (na pasta do 3DS Hub)."
                : "O PC não tem o 3dshub.3dsx (na pasta do 3DS Hub).", IC_X, COR_AMARELO);
        return;
    }

    // o que este 3DS tem
    desenharMensagem("Verificando a versão...", "", -1, NULL);
    u32 crcLocal = 0;
    if (ehCia) {
        FILE *f = fopen(ARQ_CRC_CIA, "r");
        if (f) {
            if (fscanf(f, "%lx", &crcLocal) != 1) crcLocal = 0;
            fclose(f);
        }
    } else {
        crcLocal = crcDoArquivo(caminhoApp);
    }
    if (crcLocal == crcPC) {
        uiAviso("Tudo certo", "Você já tem a versão mais nova.", IC_OK, COR_VERDE_AGUA);
        return;
    }

    char texto[128];
    snprintf(texto, sizeof(texto), "O PC tem uma versão diferente (%lu KB).\nBaixar e instalar agora?", tamPC / 1024);
    if (!confirmar("Versão nova disponível", texto, "Atualizar", "Agora não")) return;

    n = snprintf(cmd, sizeof(cmd), "B %s\n", tipo);
    u32 tam;
    if (!enviarTudo(sock, cmd, n, 3000) || !lerResposta("3DSB", &tam) || tam != tamPC) {
        desconectar();
        uiAviso("Falhou", "O PC não mandou o arquivo. Tente de novo.", IC_X, COR_VERMELHO);
        return;
    }

    u32 crc = 0;
    bool ok;
    if (ehCia) {
        // instala o CIA direto, como o FBI faz (sobrescrevendo o 3DS Hub instalado)
        ok = R_SUCCEEDED(amInit());
        Handle h = 0;
        if (ok && R_FAILED(AM_StartCiaInstallOverwrite(&h, MEDIATYPE_SD)) &&
            R_FAILED(AM_StartCiaInstall(MEDIATYPE_SD, &h)))
            ok = false;
        if (ok) {
            ok = baixar(tam, gravarNoCia, &h, &crc) && crc == crcPC;
            if (ok) ok = R_SUCCEEDED(AM_FinishCiaInstall(h));
            else    AM_CancelCIAInstall(h);
        }
        amExit();
        if (ok) {
            FILE *f = fopen(ARQ_CRC_CIA, "w");
            if (f) {
                fprintf(f, "%08lx\n", crcPC);
                fclose(f);
            }
        }
    } else {
        char novo[300];
        snprintf(novo, sizeof(novo), "%s.novo", caminhoApp);
        FILE *f = fopen(novo, "wb");
        ok = f != NULL;
        if (ok) {
            ok = baixar(tam, gravarNoArquivo, f, &crc);
            fclose(f);
            ok = ok && crc == crcPC && trocarArquivo(novo);
        }
        if (!ok) remove(novo);
    }

    if (!ok) {
        desconectar();     // a conexão pode ter ficado no meio de um arquivo
        uiAviso("Não atualizou", "O download falhou ou foi cancelado.\nA versão atual continua instalada.", IC_X, COR_VERMELHO);
        return;
    }
    if (confirmar("Atualizado!", "A versão nova já está no cartão SD.\nFeche e abra o 3DS Hub para usar.",
                  "Fechar agora", "Depois"))
        sairDoApp = true;
}

// ---------------------------------------------------------------- atualizar pela internet

// Espera o atualizador terminar o que está fazendo, mostrando o progresso.
static void esperarAtualizador(const char *titulo) {
    while (aptMainLoop() && updater_busy()) {
        hidScanInput();
        UpdState st = updater_state();
        float p = st == UPD_DOWNLOADING ? updater_progress() : -1;
        desenharMensagem(titulo, updater_message(), p, NULL);
    }
}

// ---------------------------------------------------------------- acesso pela internet

// "K7QM-2XPA" ou "k7qm2xpa": tem cara de código de pareamento (8 letras/números)
static bool pareceCodigo(const char *s) {
    int n = 0;
    for (; *s; s++) {
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9')) n++;
        else if (*s != '-' && *s != ' ') return false;
    }
    return n == 8;
}

// Pareia longe de casa com a senha fixa do PC (ou com um código de 30 minutos) e o 3DS já
// passa a usar esse PC pela internet.
static void parearLonge(void) {
    SwkbdState kb;
    char senha[64] = "";
    swkbdInit(&kb, SWKBD_TYPE_QWERTY, 2, 40);
    swkbdSetHintText(&kb, "Senha do PC (ou código)");
    swkbdSetPasswordMode(&kb, SWKBD_PASSWORD_HIDE_DELAY);
    swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    if (swkbdInputText(&kb, senha, sizeof(senha)) != SWKBD_BUTTON_CONFIRM) return;

    desenharMensagem("Pareando...", "Conferindo a senha", -1, NULL);
    char nome[32];
    int r = tunelParearSenha(senha, nome, sizeof(nome));
    if (r == 0 && pareceCodigo(senha)) r = tunelParearCodigo(senha, nome, sizeof(nome));
    memset(senha, 0, sizeof(senha));
    if (r == -1) { uiAviso("Sem internet", "Não consegui conferir a senha.\nConfira o Wi-Fi do 3DS e tente de novo.", IC_X, COR_AMARELO); return; }
    if (r == -2) { uiAviso("Muitos PCs", "Este 3DS já tem 8 PCs pareados.\nEsqueça um antes de parear outro.", IC_X, COR_AMARELO); return; }
    if (r == 0)  { uiAviso("Senha não achada", "Confira a senha (maiúsculas contam). No PC: ícone\ndo Hub > \"Senha do acesso pela internet\".", IC_X, COR_AMARELO); return; }

    // já passa a usar este PC pela internet
    snprintf(nomePC, sizeof(nomePC), "%s", nome);
    modoRemoto = true;
    redeMudou = true;
    char texto[200];
    snprintf(texto, sizeof(texto), "Pareado com \"%s\". Volte ao menu: ele conecta\npela internet. O PC precisa estar ligado\ncom o 3DS Hub aberto.", nome);
    uiAviso("Pronto!", texto, IC_OK, COR_VERDE_AGUA);
}

// Pareia em casa: o PC liga o acesso pela internet e manda o código e o canal secreto
// (só pela rede de casa). Fica guardado no cartão SD.
static void configurarRemoto(void) {
    char texto2[160];
    if (modoRemoto) {
        // longe de casa só dá para esquecer o PC em uso; parear é sempre na mesma rede
        int a = tunelAtual();
        snprintf(texto2, sizeof(texto2), "Esquecer o PC \"%s\" neste 3DS?\nPara parear de novo, use a senha do PC.", tunelNomePC(a));
        if (a >= 0 && confirmar("Acesso pela internet", texto2, "Esquecer", "Manter")) {
            tunelEsquecer(a);
            modoRemoto = false;
            redeMudou = true;
        }
        return;
    }
    if (sock < 0) conectar();
    if (sock < 0) {
        // longe de casa: pareia com o código que o PC mostra
        if (confirmar("Parear longe de casa",
                      "O PC não está nesta rede. Digite a senha do PC\n(no PC: ícone do 3DS Hub > \"Senha do acesso\npela internet\"). Também aceita um código.",
                      "Digitar senha", "Voltar"))
            parearLonge();
        return;
    }
    int ja = nomePC[0] ? tunelIndice(nomePC) : -1;
    if (ja >= 0) {
        snprintf(texto2, sizeof(texto2), "O PC \"%s\" já está pareado.\nQuer esquecer este PC?", nomePC);
        if (confirmar("Acesso pela internet", texto2, "Esquecer", "Manter")) tunelEsquecer(ja);
        return;
    }
    desenharMensagem("Pareando...", "Pedindo o código ao PC", -1, NULL);
    u32 tam = 0;
    static char texto[256];
    if (!enviarTudo(sock, "R\n", 2, 3000) || !lerResposta("3DSR", &tam)) {
        desconectar();
        uiAviso("PC desatualizado", "O 3DS Hub do PC precisa da versão 1.2 ou mais\nnova para o acesso pela internet.", IC_X, COR_AMARELO);
        return;
    }
    if (!tam || tam >= sizeof(texto) || !receberTudo(sock, texto, tam, 5000)) {
        uiAviso("O PC recusou", "Tente de novo com o PC e o 3DS na mesma rede.", IC_X, COR_AMARELO);
        return;
    }
    texto[tam] = 0;
    char *salvo = NULL;
    char *tok = strtok_r(texto, "\n", &salvo);
    char *top = strtok_r(NULL, "\n", &salvo);
    char *nome = strtok_r(NULL, "\n", &salvo);      // o PC 1.2 não manda o nome
    if (!nome || !*nome) nome = nomePC[0] ? nomePC : "PC";
    if (tunelQuantos() >= TUNEL_MAX_PCS && tunelIndice(nome) < 0) {
        uiAviso("Muitos PCs", "Este 3DS já tem 8 PCs pareados.\nEsqueça um antes de parear outro.", IC_X, COR_AMARELO);
        return;
    }
    if (!tok || !top || !tunelSalvar(nome, tok, top)) {
        uiAviso("Não deu certo", "Não consegui gravar no cartão SD.", IC_X, COR_VERMELHO);
        return;
    }
    if (!nomePC[0]) snprintf(nomePC, sizeof(nomePC), "%s", nome);
    snprintf(texto2, sizeof(texto2), "\"%s\" agora aparece em \"Procurar o PC\"\nmesmo longe de casa. O PC precisa estar\nligado com o 3DS Hub aberto.", nome);
    uiAviso("Pronto!", texto2, IC_OK, COR_VERDE_AGUA);
}

void atualizarPelaInternet(bool automatico) {
    if (!automatico) {
        updater_check();
        esperarAtualizador("Procurando versão nova...");
    }
    UpdState st = updater_state();
    if (st == UPD_UP_TO_DATE) {
        if (!automatico) uiAviso("Tudo certo", updater_message(), IC_OK, COR_VERDE_AGUA);
        return;
    }
    if (st != UPD_AVAILABLE) {
        if (!automatico) uiAviso("Não deu para procurar", updater_message(), IC_X, COR_AMARELO);
        return;
    }
    char texto[160];
    snprintf(texto, sizeof(texto), "Versão %s disponível (você tem a %s).\nBaixar e instalar agora?",
             updater_latest(), updater_version());
    if (!confirmar("Versão nova disponível", texto, "Atualizar", "Agora não")) return;
    updater_install();
    esperarAtualizador("Baixando a versão nova...");
    if (updater_state() != UPD_DONE) {
        uiAviso("Não atualizou", updater_message(), IC_X, COR_VERMELHO);
        return;
    }
    if (confirmar("Atualizado!", "A versão nova já está instalada.\nFeche e abra o 3DS Hub para usar.",
                  "Fechar agora", "Depois"))
        sairDoApp = true;
}

// ---------------------------------------------------------------- tela principal

#define LINHA_H   26
#define VISIVEIS  7
static int inicio;
static float linhaY(int i) { return 8 + (i - inicio) * (LINHA_H + 3); }

static bool precisaDoPC(int i) { return linhas[i].tipo == NUMERO || linhas[i].tipo == SIM_NAO; }

static void textoValor(int i, char *s, size_t n) {
    const Linha *l = &linhas[i];
    if (l->tipo == PROCURAR)        snprintf(s, n, "%s", nomePC[0] ? nomePC : "");
    else if (l->tipo == DIGITAR_IP) snprintf(s, n, "%s", ipPC[0] ? ipPC : "-");
    else if (l->tipo == ATUALIZAR)  snprintf(s, n, "%s", envIsHomebrew() ? ".3dsx" : "CIA");
    else if (l->tipo == ATUALIZAR_NET) snprintf(s, n, "v%s", updater_version());
    else if (l->tipo == REMOTO) {
        if (!tunelQuantos()) snprintf(s, n, "Nenhum PC");
        else snprintf(s, n, "%d PC%s%s", tunelQuantos(), tunelQuantos() > 1 ? "s" : "", modoRemoto ? " (em uso)" : "");
    }
    else if (!temValores)           snprintf(s, n, "-");
    else if (l->tipo == SIM_NAO)    snprintf(s, n, "%s", valores[i] ? "Sim" : "Não");
    else                            snprintf(s, n, "%g %s", valores[i], l->unidade);
}

static void desenhar(int sel) {
    const Linha *l = &linhas[sel];
    uiQuadro();
    uiTela(alvoCima);
    uiBarraStatus("Ajustes", IC_ENGRENAGEM, COR_TEXTO2);
    uiRetArred(16, 32, 368, 96, 12, COR_CARTAO);
    uiTexto(30, 42, 18, COR_TEXTO, ESQ, 340, "%s", l->rotulo);
    uiTexto(30, 70, 13, COR_TEXTO2, ESQ, 0, "%s", l->ajuda);

    uiRetArred(16, 136, 368, 56, 12, COR_CARTAO);
    uiIcone(IC_PC, 36, 164, 18, COR_MUDO, COR_CARTAO);
    uiTexto(54, 144, 13, COR_TEXTO, ESQ, 320, "%s", nomePC[0] ? nomePC : "PC");
    uiTexto(54, 162, 13, COR_MUDO, ESQ, 320, "%s", ipPC[0] ? ipPC : "sem IP");
    bool ok = sock >= 0 && temValores;
    uiTexto(372, 152, 13, ok ? COR_AGUA_CL : COR_AMARELO, DIR, 0, "%s", ok ? "Conectado" : "Sem conexão");
    if (status[0]) uiTexto(16, 200, 13, COR_AMARELO, ESQ, 368, "%s", status);
    else if (precisaDoPC(sel) && ok) uiTexto(16, 200, 13, COR_MUDO, ESQ, 368, "As mudanças valem na hora (o PC grava no config.json).");
    uiTexto(16, 220, 12, COR_MUDO, ESQ, 0, "START ou B: voltar ao menu");

    uiTela(alvoBaixo);
    for (int i = inicio; i < N_LINHAS && i < inicio + VISIVEIS; i++) {
        bool s = i == sel;
        float y = linhaY(i);
        bool apagada = precisaDoPC(i) && !temValores;
        uiRetArred(10, y, 300, LINHA_H, 8, s ? COR_CARTAO2 : COR_CARTAO);
        if (s) uiRet(10, y + 5, 3, LINHA_H - 10, COR_AGUA_CL);
        uiTexto(20, y + 5, 13, apagada ? COR_MUDO : COR_TEXTO, ESQ, 150, "%s", linhas[i].rotulo);
        char v[48];
        textoValor(i, v, sizeof(v));
        bool botoes = s && linhas[i].tipo == NUMERO && temValores;
        uiTexto(botoes ? 226 : 302, y + 5, 13, COR_TEXTO2, DIR, 100, "%s", v);
        if (botoes) {
            uiBotao(232, y + 2, 36, LINHA_H - 4, "-", -1, COR_CARTAO, COR_TEXTO);
            uiBotao(272, y + 2, 36, LINHA_H - 4, "+", -1, COR_CARTAO, COR_TEXTO);
        }
    }
    uiTexto(160, 222, 11, COR_MUDO, CENTRO, 0, "A: escolher   esquerda/direita: mudar");
    uiFim();
}

static void mudar(int i, int direcao) {
    const Linha *l = &linhas[i];
    if (!temValores) return;
    if (l->tipo == SIM_NAO) valores[i] = valores[i] ? 0 : 1;
    else if (l->tipo == NUMERO) {
        float v = valores[i] + direcao * l->passo;
        valores[i] = v < l->minimo ? l->minimo : v > l->maximo ? l->maximo : v;
    } else {
        return;
    }
    gravar(i);
}

void modAjustes(void) {
    int sel = 0;
    inicio = 0;
    status[0] = 0;
    conectar();

    while (aptMainLoop() && !sairDoApp) {
        hidScanInput();
        u32 down = hidKeysDown();
        touchPosition tp;
        hidTouchRead(&tp);
        if (down & (KEY_START | KEY_B)) break;

        int acao = -1, direcao = 0;
        if ((down & KEY_DUP) && sel > 0) sel--;
        if ((down & KEY_DDOWN) && sel < N_LINHAS - 1) sel++;
        if (down & KEY_DLEFT) direcao = -1;
        if (down & KEY_DRIGHT) direcao = 1;
        if (down & KEY_A) acao = sel;
        if (down & KEY_TOUCH) {
            for (int i = inicio; i < N_LINHAS && i < inicio + VISIVEIS; i++) {
                float y = linhaY(i);
                if (!uiDentro(&tp, 10, y, 300, LINHA_H)) continue;
                if (i == sel && linhas[i].tipo == NUMERO && tp.px >= 232)
                    direcao = tp.px >= 272 ? 1 : -1;    // os botões - e +
                else if (i == sel || linhas[i].tipo != NUMERO)
                    acao = i;
                sel = i;
            }
        }
        if (sel < inicio) inicio = sel;
        if (sel >= inicio + VISIVEIS) inicio = sel - VISIVEIS + 1;

        if (direcao && precisaDoPC(sel)) mudar(sel, direcao);
        if (acao >= 0) {
            sel = acao;
            switch (linhas[acao].tipo) {
            case PROCURAR:
                if (procurarPC()) conectar();
                break;
            case DIGITAR_IP:
                if (pedirIP()) conectar();
                break;
            case ATUALIZAR:
                atualizar();
                break;
            case REMOTO:
                configurarRemoto();
                break;
            case ATUALIZAR_NET:
                atualizarPelaInternet(false);
                break;
            case SIM_NAO:
                mudar(acao, 1);
                break;
            default:
                break;
            }
        }
        if (!sairDoApp) desenhar(sel);
    }
    desconectar();
}
