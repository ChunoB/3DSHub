// Pescaria — o lago aparece numa janela do PC (bom na TV) e o 3DS é a vara.
//
//   circle pad / cruz      : mirar (o tracejado da sua cor anda no lago)
//   balançar o 3DS         : lançar (mais forte = mais longe). Ou segure A e solte.
//   girar a caneta (baixo) : molinete: puxa o peixe, mas estica a linha
//   B                      : recolher a linha
//   START                  : sair
//
// Quem roda o jogo é o PC (pc/pesca.py, UDP 5015): o 3DS manda mira, lançamentos e
// o giro do molinete ~60 vezes por segundo e mostra o que o PC responde.

#include "comum.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORTA 5015

enum { MIRANDO, VOANDO, NA_AGUA, FISGOU, PUXANDO, PEGOU, PERDEU };

typedef struct __attribute__((packed)) {
    char magic[4];                      // "3DSF"
    s16  mira;
    u8   evento, tipo, forca;
    u16  giro;
} Envio;

static const u32 coresJog[4] = { HEX(0xE8483C), HEX(0x3C8CE8), HEX(0xF2B632), HEX(0x5CC85C) };
static const char *nomesJog[4] = { "Vermelho", "Azul", "Amarelo", "Verde" };

static struct {
    int estado, jogador, tensao, distancia, pontos, mordisca;
    char msg[96];
    u64 ultimaResposta;
} pc;

#define REEL_X 160
#define REEL_Y 112
#define REEL_R 92

static void desenharCima(float mira, float carga, bool carregando, u64 agora) {
    uiTela(alvoCima);
    bool conectado = pc.ultimaResposta && agora - pc.ultimaResposta < 2000;
    u32 cor = conectado && pc.jogador ? coresJog[(pc.jogador - 1) & 3] : COR_PESCA_CL;

    uiRet(0, UI_BARRA, 400, 60, HEX(0x9ED4F0));              // céu
    uiRet(0, UI_BARRA + 60, 400, 240 - UI_BARRA - 60, HEX(0x2874A6));   // água
    for (int i = 0; i < 8; i++) {
        float x = fmodf(i * 61 + (agora % 3600000) / 40.0f, 400);
        uiRet(x, 110 + (i * 37) % 100, 18, 2, HEX(0x5DADE2));
    }
    uiBarraStatus("Pescaria", IC_PEIXE, COR_PESCA_CL);

    if (!conectado) {
        uiRetArred(30, 90, 340, 70, 12, COR_ESCURO);
        uiTexto(200, 100, 16, COR_TEXTO, CENTRO, 320, "Procurando o PC...");
        uiTexto(200, 126, 12, COR_MUDO, CENTRO, 320, "O lago abre numa janela no PC (F11: tela cheia)");
        return;
    }

    // placa do pescador
    uiRetArred(8, UI_BARRA + 4, 220, 24, 10, COR_ESCURO);
    uiCirculo(22, UI_BARRA + 16, 6, cor);
    uiTexto(34, UI_BARRA + 9, 13, COR_TEXTO, ESQ, 190, "Pescador %s - %d pontos", nomesJog[(pc.jogador - 1) & 3], pc.pontos);

    // a vara saindo da parte de baixo e a boia
    float bx = 200 + mira * 1.4f, by = 130;
    bool linha = pc.estado != MIRANDO && pc.estado != PEGOU && pc.estado != PERDEU;
    uiLinha(330, 240, 270, 150, 5, HEX(0x5C3A1E));
    if (linha) {
        if (pc.mordisca || pc.estado == FISGOU || pc.estado == PUXANDO) by += sinf((agora % 3600000) / 40.0f) * 4 + 4;
        uiLinha(270, 150, bx, by, 1, COR_BRANCO);
        uiCirculo(bx, by, 7, COR_BRANCO);
        uiCirculo(bx, by - 2, 5, cor);
    }

    const char *dica = "";
    switch (pc.estado) {
        case MIRANDO: {
            // mira: onde vai cair (esquerda/direita)
            uiRet(40, 196, 320, 4, COR_CARTAO2);
            uiTri(200 + mira * 1.6f - 7, 186, 200 + mira * 1.6f + 7, 186, 200 + mira * 1.6f, 196, cor);
            if (carregando) {
                uiTexto(200, 150, 13, COR_TEXTO, CENTRO, 0, "Força");
                uiBarra(100, 168, 200, 12, carga / 100, cor, COR_CARTAO2);
            }
            dica = "Mire com o circle pad. Balance o 3DS (ou segure A) para lançar!";
            break;
        }
        case VOANDO:  dica = "Lá vai!"; break;
        case NA_AGUA: dica = pc.mordisca ? "Algo está mordiscando... espere!" : "Esperando um peixe..."; break;
        case FISGOU:
            if ((agora / 150) % 2) uiTexto(200, 60, 34, COR_AMARELO, CENTRO, 0, "FISGOU!");
            dica = "GIRE O MOLINETE AGORA!";
            break;
        case PUXANDO: {
            u32 corT = pc.tensao < 60 ? COR_VERDE : pc.tensao < 85 ? COR_AMARELO : COR_VERMELHO;
            uiTexto(40, 150, 12, COR_TEXTO, ESQ, 0, "Linha");
            uiBarra(100, 152, 260, 12, pc.tensao / 100.0f, corT, COR_CARTAO2);
            uiTexto(40, 172, 12, COR_TEXTO, ESQ, 0, "Peixe");
            uiBarra(100, 174, 260, 12, 1 - pc.distancia / 100.0f, COR_PESCA_CL, COR_CARTAO2);
            dica = pc.tensao >= 85 ? "Cuidado! A linha vai arrebentar!" : "Gire a caneta para puxar";
            break;
        }
        default: break;
    }
    uiRetArred(8, 210, 384, 26, 10, COR_ESCURO);
    uiTexto(200, 215, 13, pc.msg[0] ? COR_AMARELO : COR_TEXTO2, CENTRO, 370, "%s", pc.msg[0] ? pc.msg : dica);
}

static void desenharBaixo(float anguloReel, float giro, bool tocando) {
    uiTela(alvoBaixo);
    bool puxar = pc.estado == FISGOU || pc.estado == PUXANDO;
    uiAnel(REEL_X, REEL_Y, REEL_R, 6, puxar ? COR_PESCA : COR_LINHA, COR_FUNDO);
    uiCirculo(REEL_X, REEL_Y, 34, COR_CARTAO2);
    for (int i = 0; i < 6; i++) {                              // raios girando
        float a = anguloReel + i * (float)M_PI / 3;
        uiLinha(REEL_X + cosf(a) * 36, REEL_Y + sinf(a) * 36, REEL_X + cosf(a) * (REEL_R - 6),
                REEL_Y + sinf(a) * (REEL_R - 6), 2, COR_CARTAO2);
    }
    float hx = REEL_X + cosf(anguloReel) * (REEL_R - 18), hy = REEL_Y + sinf(anguloReel) * (REEL_R - 18);
    uiLinha(REEL_X, REEL_Y, hx, hy, 6, COR_PESCA_CL);           // manivela
    uiCirculo(hx, hy, 12, tocando ? COR_BRANCO : COR_PESCA_CL);
    uiCirculo(REEL_X, REEL_Y, 8, COR_PESCA_CL);
    uiTexto(160, 212, 11, COR_MUDO, CENTRO, 0, puxar ? "Gire a caneta em círculos!" : "Molinete: gire quando fisgar");
    uiTexto(160, 226, 10, COR_MUDO, CENTRO, 0, "B: recolher   START: sair   %.0f°/s", giro);
}

void modPescaria(void) {
    struct sockaddr_in dest;
    int sock = abrirUDP(PORTA, &dest);
    if (sock < 0) return;
    memset(&pc, 0, sizeof(pc));

    HIDUSER_EnableGyroscope();
    float coef = 14.375f;
    if (R_FAILED(HIDUSER_GetGyroscopeRawToDpsCoefficient(&coef)) || coef < 1.0f) coef = 14.375f;

    Envio e;
    memset(&e, 0, sizeof(e));
    memcpy(e.magic, "3DSF", 4);
    u8 evento = 0;
    float mira = 0, carga = 0, giro = 0, anguloReel = 0, anguloAntes = 0, pico = 0;
    float vies[3] = { 0 };
    int calibrando = 30;
    bool carregando = false, tocandoAntes = false;
    u64 tAntes = osGetTime(), carregaDesde = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown(), held = hidKeysHeld(), up = hidKeysUp();
        if (down & KEY_START) break;
        u64 agora = osGetTime();
        float dt = (agora - tAntes) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        tAntes = agora;
        int tipo = 0, forca = 0;

        // mira: circle pad ou cruz, anda devagar e fica onde parou
        circlePosition cp;
        hidCircleRead(&cp);
        float vx = abs(cp.dx) > 15 ? cp.dx / 150.0f : 0;
        if (held & KEY_DLEFT) vx = -1;
        if (held & KEY_DRIGHT) vx = 1;
        mira += vx * 90 * dt;
        if (mira < -100) mira = -100;
        if (mira > 100) mira = 100;

        // lançar balançando: um pico de giro (graus/s) acima do limite vira um lançamento
        angularRate g;
        hidGyroRead(&g);
        if (calibrando > 0) {
            vies[0] += g.x / 30.0f; vies[1] += g.y / 30.0f; vies[2] += g.z / 30.0f;
            calibrando--;
        } else if (pc.estado == MIRANDO) {
            float gx = (g.x - vies[0]) / coef, gy = (g.y - vies[1]) / coef, gz = (g.z - vies[2]) / coef;
            float mag = sqrtf(gx * gx + gy * gy + gz * gz);
            if (mag > 250) pico = mag > pico ? mag : pico;
            else if (pico > 0 && mag < 120) {                 // terminou o movimento: lança
                tipo = 1;
                forca = (int)((pico - 250) / 6) + 20;
                pico = 0;
            }
        }
        // ou segurando A: a força sobe e desce; soltar lança
        if ((down & KEY_A) && pc.estado == MIRANDO) {
            carregando = true;
            carregaDesde = agora;
        }
        if (carregando) {
            float t = (agora - carregaDesde) / 1200.0f;
            carga = (1 - fabsf(fmodf(t, 2.0f) - 1.0f)) * 100;   // sobe até 100 e volta
            if (up & KEY_A) {
                carregando = false;
                if (pc.estado == MIRANDO) {
                    tipo = 1;
                    forca = (int)carga;
                }
            }
        }
        if (down & KEY_B) tipo = 2;
        if (forca < 5) forca = 5;
        if (forca > 100) forca = 100;
        if (tipo) evento++;

        // molinete: quanto o dedo girou em volta do centro
        touchPosition tp;
        hidTouchRead(&tp);
        bool tocando = held & KEY_TOUCH;
        float omega = 0;
        if (tocando) {
            float dx = tp.px - REEL_X, dy = tp.py - REEL_Y;
            float ang = atan2f(dy, dx);
            if (tocandoAntes && dx * dx + dy * dy > 15 * 15) {
                float d = ang - anguloAntes;
                while (d > (float)M_PI) d -= 2 * (float)M_PI;
                while (d < -(float)M_PI) d += 2 * (float)M_PI;
                anguloReel += d;
                if (dt > 0) omega = fabsf(d) * 57.3f / dt;
            }
            anguloAntes = ang;
        }
        tocandoAntes = tocando;
        giro += (omega - giro) * (dt * 8 > 1 ? 1 : dt * 8);     // suaviza (o toque é tremido)
        if (!tocando && giro < 5) giro = 0;

        e.mira = (s16)mira;
        e.evento = evento;
        e.tipo = tipo ? tipo : e.tipo;                          // repete o último tipo com o mesmo evento
        e.forca = tipo == 1 ? forca : e.forca;
        e.giro = (u16)(giro > 65535 ? 65535 : giro);
        enviarPara(sock, &e, sizeof(e), &dest);

        static u8 resp[256];
        int n;
        while ((n = receberDe(sock, resp, sizeof(resp) - 1)) >= 11) {
            if (memcmp(resp, "3DSF", 4)) continue;
            pc.estado = resp[4];
            pc.jogador = resp[5];
            pc.tensao = resp[6];
            pc.distancia = resp[7];
            pc.pontos = resp[8] | (resp[9] << 8);
            pc.mordisca = resp[10];
            resp[n] = 0;
            snprintf(pc.msg, sizeof(pc.msg), "%.95s", (char *)resp + 11);
            pc.ultimaResposta = agora;
        }
        if (pc.estado != MIRANDO) carregando = false;

        uiQuadro();
        desenharCima(mira, carga, carregando, agora);
        desenharBaixo(anguloReel, giro, tocando);
        uiFim();
    }

    HIDUSER_DisableGyroscope();
    fecharRede(sock);
}
