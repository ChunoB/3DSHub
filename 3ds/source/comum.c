#include "comum.h"
#include "tunel.h"

#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <jpeglib.h>

#define ARQ_CFG        "sdmc:/3ds/3dshub.cfg"
#define ARQ_PADREMOTO  "sdmc:/3ds/padremoto.cfg"   // usa o IP do PadRemoto na primeira vez

char ipPC[32];
struct in_addr enderecoPC;
char nomePC[32];
char caminhoApp[256] = "sdmc:/3ds/3dshub.3dsx";
bool sairDoApp;
bool modoRemoto;
bool redeMudou;
void (*aoEsperarRede)(void);
bool apertouStart;

bool desistirDaRede(void) {
    if (aptShouldClose()) return true;
    hidScanInput();
    if (hidKeysHeld() & KEY_START) apertouStart = true;
    return apertouStart;
}

// ---------------------------------------------------------------- IP

static void lerArquivo(const char *caminho) {
    ipPC[0] = 0;
    FILE *f = fopen(caminho, "r");
    if (!f) return;
    if (!fgets(ipPC, sizeof(ipPC), f)) ipPC[0] = 0;
    fclose(f);
    ipPC[strcspn(ipPC, "\r\n")] = 0;
}

void carregarIP(void) {
    lerArquivo(ARQ_CFG);
    if (!ipPC[0]) lerArquivo(ARQ_PADREMOTO);
    if (!inet_aton(ipPC, &enderecoPC)) ipPC[0] = 0;
}

bool definirIP(const char *ip) {
    struct in_addr a;
    if (!inet_aton(ip, &a)) return false;
    snprintf(ipPC, sizeof(ipPC), "%s", ip);
    enderecoPC = a;
    FILE *f = fopen(ARQ_CFG, "w");
    if (f) {
        fprintf(f, "%s\n", ipPC);
        fclose(f);
    }
    return true;
}

bool pedirIP(void) {
    SwkbdState kb;
    char novo[32];
    swkbdInit(&kb, SWKBD_TYPE_NUMPAD, 2, 15);
    swkbdSetNumpadKeys(&kb, '.', 0);
    swkbdSetHintText(&kb, "IP do PC (ex: 192.168.0.10)");
    swkbdSetInitialText(&kb, ipPC);
    if (swkbdInputText(&kb, novo, sizeof(novo)) != SWKBD_BUTTON_CONFIRM) return false;
    if (!definirIP(novo)) return false;
    nomePC[0] = 0;                 // IP digitado: ainda não sabemos o nome desse PC
    return true;
}

// ---------------------------------------------------------------- descoberta

#define PORTA_DESCOBERTA 5009

int descobertaAbrir(void) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;
    // porta local qualquer: a resposta volta para a porta de onde a pergunta saiu
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_port        = 0;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(sock, (struct sockaddr *)&local, sizeof(local));
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    return sock;
}

void descobertaPerguntar(int sock) {
    if (sock < 0) return;
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(PORTA_DESCOBERTA);
    // para todo mundo da rede, e também para o "broadcast" da sub-rede atual
    dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(sock, "3DSD", 4, 0, (struct sockaddr *)&dest, sizeof(dest));
    struct in_addr ip, mascara, broadcast;
    if (SOCU_GetIPInfo(&ip, &mascara, &broadcast) == 0 && broadcast.s_addr &&
        broadcast.s_addr != htonl(INADDR_BROADCAST)) {
        dest.sin_addr = broadcast;
        sendto(sock, "3DSD", 4, 0, (struct sockaddr *)&dest, sizeof(dest));
    }
}

bool descobertaResposta(int sock, char *ip, size_t nIp, char *nome, size_t nNome) {
    if (sock < 0) return false;
    char buf[64];
    struct sockaddr_in de;
    socklen_t tam = sizeof(de);
    int n;
    while ((n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&de, &tam)) >= 4) {
        // a própria pergunta pode voltar (broadcast): só vale resposta com nome
        if (memcmp(buf, "3DSD", 4) || n == 4) {
            tam = sizeof(de);
            continue;
        }
        buf[n] = 0;
        snprintf(ip, nIp, "%s", inet_ntoa(de.sin_addr));
        snprintf(nome, nNome, "%s", buf + 4);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- CRC32 (igual ao zlib)

u32 crc32(u32 crc, const void *dados, size_t n) {
    static u32 tabela[256];
    if (!tabela[1])
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tabela[i] = c;
        }
    const u8 *p = dados;
    crc = ~crc;
    while (n--) crc = tabela[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// ---------------------------------------------------------------- rede

int abrirUDP(int porta, struct sockaddr_in *destino) {
    memset(destino, 0, sizeof(*destino));
    destino->sin_family = AF_INET;
    destino->sin_port   = htons(porta);
    destino->sin_addr   = enderecoPC;
    if (modoRemoto) return tunelAbrir(porta, true, 12000);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    // escuta na mesma porta para receber as respostas do PC
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_port        = htons(porta);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(sock, (struct sockaddr *)&local, sizeof(local));
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    return sock;
}

int conectarTCP(int porta, int timeoutMs) {
    // pela internet o TLS demora mais para abrir (sobretudo no Old 3DS)
    if (modoRemoto) return tunelAbrir(porta, false, timeoutMs < 12000 ? 12000 : timeoutMs);
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_in destino;
    memset(&destino, 0, sizeof(destino));
    destino.sin_family = AF_INET;
    destino.sin_port   = htons(porta);
    destino.sin_addr   = enderecoPC;

    // O socket fica sem bloqueio para sempre: connect, send e recv nunca
    // prendem o 3DS (se o Wi-Fi cair, o timeout do poll resolve).
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    if (connect(sock, (struct sockaddr *)&destino, sizeof(destino)) == 0) return sock;
    if (errno != EINPROGRESS && errno != EALREADY && errno != EWOULDBLOCK && errno != EAGAIN) {
        close(sock);
        return -1;
    }

    u64 fim = osGetTime() + timeoutMs;
    while (osGetTime() < fim && aptMainLoop()) {
        struct pollfd pf = { .fd = sock, .events = POLLOUT };
        if (poll(&pf, 1, 50) > 0) {
            // No 3DS o SO_ERROR e o POLLHUP não são confiáveis aqui: vinham com
            // "erro" mesmo com a conexão aberta, e o app fechava uma conexão boa.
            // Então pergunta direto: tem alguém do outro lado?
            struct sockaddr_in par;
            socklen_t tamPar = sizeof(par);
            if (getpeername(sock, (struct sockaddr *)&par, &tamPar) == 0) return sock;
            // Ainda não: um novo connect diz se está conectado, conectando ou se falhou.
            if (connect(sock, (struct sockaddr *)&destino, sizeof(destino)) == 0 || errno == EISCONN)
                return sock;
            if (errno != EINPROGRESS && errno != EALREADY && errno != EWOULDBLOCK && errno != EAGAIN) {
                close(sock);
                return -1;
            }
            svcSleepThread(10 * 1000000LL);   // 10 ms: o poll pode voltar na hora sem ter conectado
        }
        hidScanInput();
        if ((hidKeysDown() | hidKeysHeld()) & (KEY_B | KEY_START)) {
            close(sock);
            return CONEXAO_CANCELADA;
        }
    }
    close(sock);
    return -1;
}

static bool deNovo(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }

// Espera o socket ficar pronto em fatias de 16 ms, chamando aoEsperarRede entre
// elas (o módulo continua lendo botões e toque). false = timeout ou erro.
static bool esperar(int sock, short evento, int timeoutMs) {
    u64 fim = osGetTime() + timeoutMs;
    while (true) {
        struct pollfd pf = { .fd = sock, .events = evento };
        int r = poll(&pf, 1, 16);
        if (r > 0) return true;
        if (r < 0) return false;
        if (osGetTime() >= fim) return false;
        if (aptShouldClose()) return false;     // fechando pelo HOME: não espera a rede
        if (aoEsperarRede) aoEsperarRede();
    }
}

// O timeout vale para cada espera: se nada andar nesse tempo, desiste.
bool enviarTudo(int sock, const void *dados, size_t n, int timeoutMs) {
    if (ehTunel(sock)) return tunelEnviar(sock, dados, n, timeoutMs);
    const u8 *p = dados;
    while (n > 0) {
        if (!esperar(sock, POLLOUT, timeoutMs)) return false;
        ssize_t r = send(sock, p, n, 0);
        if (r < 0 && deNovo()) continue;
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

bool receberTudo(int sock, void *dados, size_t n, int timeoutMs) {
    if (ehTunel(sock)) return tunelReceberTudo(sock, dados, n, timeoutMs);
    u8 *p = dados;
    while (n > 0) {
        if (!esperar(sock, POLLIN, timeoutMs)) return false;
        ssize_t r = recv(sock, p, n, 0);
        if (r < 0 && deNovo()) continue;
        if (r <= 0) return false;      // 0 = o PC fechou a conexão
        p += r;
        n -= r;
    }
    return true;
}

int enviarPara(int sock, const void *dados, size_t n, const struct sockaddr_in *destino) {
    if (ehTunel(sock)) return tunelEnviar(sock, dados, n, 1000) ? (int)n : -1;
    return sendto(sock, dados, n, 0, (const struct sockaddr *)destino, sizeof(*destino));
}

int receberDe(int sock, void *buf, size_t n) {
    if (ehTunel(sock)) return tunelReceberDatagrama(sock, buf, n);
    return recvfrom(sock, buf, n, 0, NULL, NULL);
}

void fecharRede(int sock) {
    if (ehTunel(sock)) tunelFechar(sock);
    else if (sock >= 0) close(sock);
}

// ---------------------------------------------------------------- JPEG
// O tratamento de erro padrão do libjpeg fecha o programa; este volta com false.

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf pulo;
} ErroJpeg;

static void erroJpeg(j_common_ptr c) {
    longjmp(((ErroJpeg *)c->err)->pulo, 1);
}

bool jpegCodificar(const u16 *rgb565, int w, int h, int qualidade, u8 **saida, unsigned long *tam) {
    struct jpeg_compress_struct c;
    ErroJpeg erro;
    static u8 linha[400 * 3];

    c.err = jpeg_std_error(&erro.pub);
    erro.pub.error_exit = erroJpeg;
    if (setjmp(erro.pulo)) {
        jpeg_destroy_compress(&c);
        return false;
    }
    jpeg_create_compress(&c);
    *saida = NULL;
    *tam = 0;
    jpeg_mem_dest(&c, saida, tam);
    c.image_width      = w;
    c.image_height     = h;
    c.input_components = 3;
    c.in_color_space   = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, qualidade, TRUE);
    c.dct_method = JDCT_IFAST;
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < (unsigned)h) {
        const u16 *src = rgb565 + c.next_scanline * w;
        for (int x = 0; x < w; x++) {
            u16 d = src[x];
            linha[x * 3 + 0] = (d >> 11) << 3;
            linha[x * 3 + 1] = ((d >> 5) & 0x3F) << 2;
            linha[x * 3 + 2] = (d & 0x1F) << 3;
        }
        JSAMPROW r = linha;
        jpeg_write_scanlines(&c, &r, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    return true;
}

// direto = decodifica já em RGB565 (rápido); senão decodifica em RGB e converte.
static bool decodificar(const u8 *dados, unsigned long tam, u16 *destino, int larguraLinha,
                        int maxW, int maxH, int *w, int *h, bool direto) {
    struct jpeg_decompress_struct c;
    ErroJpeg erro;
    static u8 rgb[1024 * 3];
    static u16 descarte[1024];              // linhas que não cabem: lê e joga fora

    c.err = jpeg_std_error(&erro.pub);
    erro.pub.error_exit = erroJpeg;
    if (setjmp(erro.pulo)) {
        jpeg_destroy_decompress(&c);
        return false;
    }
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, dados, tam);
    jpeg_read_header(&c, TRUE);
    c.out_color_space     = direto ? JCS_RGB565 : JCS_RGB;
    c.dither_mode         = JDITHER_NONE;
    c.dct_method          = JDCT_IFAST;
    c.do_fancy_upsampling = FALSE;
    jpeg_start_decompress(&c);

    if ((int)c.output_width > maxW || c.output_width > 1024) longjmp(erro.pulo, 1);
    *w = c.output_width;
    *h = c.output_height > (unsigned)maxH ? maxH : (int)c.output_height;
    while (c.output_scanline < c.output_height) {
        int y = c.output_scanline;
        u16 *saida = y < maxH ? destino + y * larguraLinha : descarte;
        JSAMPROW linha = direto ? (JSAMPROW)saida : rgb;
        jpeg_read_scanlines(&c, &linha, 1);
        if (!direto)
            for (int x = 0; x < *w; x++)
                saida[x] = ((rgb[x * 3] >> 3) << 11) | ((rgb[x * 3 + 1] >> 2) << 5) | (rgb[x * 3 + 2] >> 3);
    }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    return true;
}

bool jpegDecodificar(const u8 *dados, unsigned long tam, u16 *destino, int larguraLinha,
                     int maxW, int maxH, int *w, int *h) {
    // Tenta o modo rápido; se esta versão do libjpeg não aceitar RGB565 e o modo
    // com conversão funcionar, passa a usar só o modo com conversão.
    static bool rgb565Funciona = true;
    if (rgb565Funciona && decodificar(dados, tam, destino, larguraLinha, maxW, maxH, w, h, true))
        return true;
    if (!decodificar(dados, tam, destino, larguraLinha, maxW, maxH, w, h, false)) return false;
    rgb565Funciona = false;
    return true;
}
