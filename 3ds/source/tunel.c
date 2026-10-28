// Túnel para o acesso pela internet. Cada conexão é um WebSocket sobre HTTPS feito à mão
// em cima do curl (modo CONNECT_ONLY: o curl só abre o TLS e a gente manda os bytes).
#include "tunel.h"
#include "comum.h"

#include <curl/curl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARQ_REMOTO  "sdmc:/3ds/3dshub_remoto.cfg"
#define CA          "romfs:/cacert.pem"
#define MAX_TUNEIS  8
#define MAX_MSGS    32

typedef struct {
    bool usado, datagramas, fechado;
    CURL *c;
    curl_socket_t fd;
    u8 *rx;    size_t nrx, caprx;       // bytes da rede ainda não separados em mensagens
    u8 *flx;   size_t nflx, capflx;     // fluxo (TCP) pronto para ler
    u8 *msg[MAX_MSGS]; size_t tamMsg[MAX_MSGS];
    int inicio, quantas;                // fila de mensagens (UDP)
} Tunel;

typedef struct { char nome[32], token[100], topico[64]; } PCPareado;

static Tunel tuneis[MAX_TUNEIS];
static PCPareado pcs[TUNEL_MAX_PCS];
static int npcs, atual = -1;
static char endereco[200], enderecoFixo[200];
static bool carregado;

// ---------------------------------------------------------------- PCs pareados
// Arquivo: uma linha "pc=nome<TAB>token<TAB>topico" por PC e "atual=topico" (o último
// usado). As linhas "token=" e "topico=" da versão 1.2 (um PC só) ainda são lidas.

static void salvarArquivo(void) {
    FILE *f = fopen(ARQ_REMOTO, "w");
    if (!f) return;
    for (int i = 0; i < npcs; i++) fprintf(f, "pc=%s\t%s\t%s\n", pcs[i].nome, pcs[i].token, pcs[i].topico);
    if (atual >= 0) fprintf(f, "atual=%s\n", pcs[atual].topico);
    if (enderecoFixo[0]) fprintf(f, "url=%s\n", enderecoFixo);
    fclose(f);
}

static void carregar(void) {
    if (carregado) return;
    carregado = true;
    npcs = 0;
    atual = -1;
    enderecoFixo[0] = 0;
    char tokenAntigo[100] = "", topicoAntigo[64] = "", topicoAtual[64] = "";
    FILE *f = fopen(ARQ_REMOTO, "r");
    if (!f) return;
    char l[300];
    while (fgets(l, sizeof(l), f)) {
        l[strcspn(l, "\r\n")] = 0;
        if (!strncmp(l, "pc=", 3) && npcs < TUNEL_MAX_PCS) {
            char *nome = l + 3, *tok = strchr(nome, '\t'), *top = tok ? strchr(tok + 1, '\t') : NULL;
            if (!tok || !top) continue;
            *tok++ = 0;
            *top++ = 0;
            PCPareado *p = &pcs[npcs++];
            snprintf(p->nome, sizeof(p->nome), "%.31s", nome);
            snprintf(p->token, sizeof(p->token), "%.99s", tok);
            snprintf(p->topico, sizeof(p->topico), "%.63s", top);
        } else if (!strncmp(l, "atual=", 6)) snprintf(topicoAtual, sizeof(topicoAtual), "%.63s", l + 6);
        else if (!strncmp(l, "token=", 6)) snprintf(tokenAntigo, sizeof(tokenAntigo), "%.99s", l + 6);
        else if (!strncmp(l, "topico=", 7)) snprintf(topicoAntigo, sizeof(topicoAntigo), "%.63s", l + 7);
        else if (!strncmp(l, "url=", 4)) snprintf(enderecoFixo, sizeof(enderecoFixo), "%.199s", l + 4);   // para testes
    }
    fclose(f);
    if (!npcs && tokenAntigo[0]) {          // pareado na versão 1.2
        snprintf(pcs[0].nome, sizeof(pcs[0].nome), "PC de casa");
        snprintf(pcs[0].token, sizeof(pcs[0].token), "%s", tokenAntigo);
        snprintf(pcs[0].topico, sizeof(pcs[0].topico), "%s", topicoAntigo);
        npcs = 1;
    }
    for (int i = 0; i < npcs; i++) if (!strcmp(pcs[i].topico, topicoAtual)) atual = i;
    if (atual < 0 && npcs) atual = 0;
}

bool tunelPareado(void) {
    carregar();
    return atual >= 0;
}

int tunelQuantos(void) { carregar(); return npcs; }
const char *tunelNomePC(int i) { carregar(); return i >= 0 && i < npcs ? pcs[i].nome : ""; }
int tunelAtual(void) { carregar(); return atual; }

int tunelIndice(const char *nome) {
    carregar();
    for (int i = 0; i < npcs; i++) if (!strcmp(pcs[i].nome, nome)) return i;
    return -1;
}

void tunelEscolher(int i) {
    carregar();
    if (i < 0 || i >= npcs || i == atual) return;
    atual = i;
    endereco[0] = 0;            // outro PC, outro endereço
    salvarArquivo();
}

bool tunelSalvar(const char *nome, const char *t, const char *p) {
    carregar();
    // o mesmo PC pareado de novo substitui o antigo
    int i = 0;
    while (i < npcs && strcmp(pcs[i].nome, nome) && strcmp(pcs[i].topico, p)) i++;
    if (i == npcs) {
        if (npcs == TUNEL_MAX_PCS) return false;
        npcs++;
    }
    snprintf(pcs[i].nome, sizeof(pcs[i].nome), "%s", nome[0] ? nome : "PC");
    snprintf(pcs[i].token, sizeof(pcs[i].token), "%s", t);
    snprintf(pcs[i].topico, sizeof(pcs[i].topico), "%s", p);
    atual = i;
    endereco[0] = 0;
    salvarArquivo();
    return true;
}

void tunelEsquecer(int i) {
    carregar();
    if (i < 0 || i >= npcs) return;
    memmove(&pcs[i], &pcs[i + 1], (npcs - i - 1) * sizeof(PCPareado));
    npcs--;
    if (atual == i) { atual = npcs ? 0 : -1; endereco[0] = 0; }
    else if (atual > i) atual--;
    salvarArquivo();
}

const char *tunelEndereco(void) { return endereco; }

// ---------------------------------------------------------------- endereço atual (ntfy)

// curl: START ou fechar o app interrompe a conexão (senão a tela fica parada até 15 s)
static int desistir(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    return desistirDaRede();
}

static void podeCancelar(CURL *c) {
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, desistir);
}

typedef struct { char *buf; size_t n, cap; } Mem;

static size_t guardar(char *p, size_t sz, size_t n, void *ud) {
    Mem *m = ud;
    size_t k = sz * n;
    if (m->n + k + 1 > m->cap) {
        size_t nc = (m->n + k + 1) * 2;
        if (nc > 256 * 1024) return 0;
        char *nb = realloc(m->buf, nc);
        if (!nb) return 0;
        m->buf = nb;
        m->cap = nc;
    }
    memcpy(m->buf + m->n, p, k);
    m->n += k;
    m->buf[m->n] = 0;
    return k;
}

bool tunelBuscarEndereco(void) {
    carregar();
    if (enderecoFixo[0]) { snprintf(endereco, sizeof(endereco), "%s", enderecoFixo); return true; }
    if (atual < 0) return false;
    char url[300];
    snprintf(url, sizeof(url), "https://ntfy.sh/%s/raw?poll=1", pcs[atual].topico);
    CURL *c = curl_easy_init();
    if (!c) return false;
    Mem m = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CAINFO, CA);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "3DSHub/" APP_VERSION);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, guardar);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    podeCancelar(c);
    CURLcode r = curl_easy_perform(c);
    curl_easy_cleanup(c);
    bool ok = false;
    if (r == CURLE_OK && m.buf) {
        // uma mensagem por linha, da mais antiga para a mais nova: vale a última
        char *salvo = NULL;
        for (char *l = strtok_r(m.buf, "\r\n", &salvo); l; l = strtok_r(NULL, "\r\n", &salvo))
            if (!strncmp(l, "https://", 8) && strlen(l) < sizeof(endereco)) {
                snprintf(endereco, sizeof(endereco), "%s", l);
                ok = true;
            }
    }
    free(m.buf);
    return ok;
}

// ---------------------------------------------------------------- bytes crus no TLS

static Tunel *pegar(int h) {
    int i = h - TUNEL_BASE;
    return i >= 0 && i < MAX_TUNEIS && tuneis[i].usado ? &tuneis[i] : NULL;
}

bool ehTunel(int h) { return pegar(h) != NULL; }

static bool anexar(u8 **buf, size_t *n, size_t *cap, const void *dados, size_t k) {
    if (*n + k > *cap) {
        size_t nc = *cap ? *cap * 2 : 16384;
        while (nc < *n + k) nc *= 2;
        u8 *nb = realloc(*buf, nc);
        if (!nb) return false;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *n, dados, k);
    *n += k;
    return true;
}

static bool esperarFd(curl_socket_t fd, short ev, u64 fim) {
    while (true) {
        struct pollfd pf = { .fd = fd, .events = ev };
        int r = poll(&pf, 1, 16);
        if (r > 0) return true;
        if (r < 0 || osGetTime() >= fim) return false;
        if (aoEsperarRede) aoEsperarRede();
        else if (desistirDaRede()) return false;
    }
}

static bool enviarBruto(Tunel *t, const u8 *p, size_t n, int timeoutMs) {
    u64 fim = osGetTime() + timeoutMs;
    while (n > 0) {
        size_t foi = 0;
        CURLcode r = curl_easy_send(t->c, p, n, &foi);
        if (r == CURLE_AGAIN) {
            if (!esperarFd(t->fd, POLLOUT, fim)) return false;
            continue;
        }
        if (r != CURLE_OK) { t->fechado = true; return false; }
        p += foi;
        n -= foi;
        fim = osGetTime() + timeoutMs;
    }
    return true;
}

// Separa as mensagens WebSocket que já chegaram inteiras.
static void separar(Tunel *t) {
    while (t->nrx >= 2) {
        u8 *b = t->rx;
        int op = b[0] & 0x0F;
        size_t n = b[1] & 0x7F, cab = 2;
        if (n == 126) {
            if (t->nrx < 4) return;
            n = (b[2] << 8) | b[3];
            cab = 4;
        } else if (n == 127) {
            if (t->nrx < 10) return;
            n = 0;
            for (int i = 0; i < 8; i++) n = (n << 8) | b[2 + i];
            cab = 10;
        }
        if (b[1] & 0x80) cab += 4;          // o servidor não deveria mascarar
        if (t->nrx < cab + n) return;
        u8 *dados = b + cab;
        if (b[1] & 0x80) for (size_t i = 0; i < n; i++) dados[i] ^= b[cab - 4 + (i & 3)];
        if (op == 8) t->fechado = true;
        else if (op == 9) {                 // ping -> pong
            u8 pong[2] = { 0x8A, 0x80 };    // sem dados, máscara obrigatória vem a seguir
            u8 q[6] = { pong[0], pong[1], 0, 0, 0, 0 };
            enviarBruto(t, q, 6, 1000);
        } else if (op == 0 || op == 1 || op == 2) {
            if (t->datagramas) {
                if (t->quantas < MAX_MSGS) {
                    int k = (t->inicio + t->quantas) % MAX_MSGS;
                    t->msg[k] = malloc(n ? n : 1);
                    if (t->msg[k]) {
                        memcpy(t->msg[k], dados, n);
                        t->tamMsg[k] = n;
                        t->quantas++;
                    }
                }                           // fila cheia: perde, como no UDP
            } else {
                anexar(&t->flx, &t->nflx, &t->capflx, dados, n);
            }
        }
        memmove(t->rx, t->rx + cab + n, t->nrx - cab - n);
        t->nrx -= cab + n;
    }
}

// Lê tudo o que já chegou, sem esperar.
static void puxar(Tunel *t) {
    static u8 tmp[16384];
    while (!t->fechado) {
        size_t n = 0;
        CURLcode r = curl_easy_recv(t->c, tmp, sizeof(tmp), &n);
        if (r == CURLE_AGAIN) break;
        if (r != CURLE_OK || n == 0) { t->fechado = true; break; }
        anexar(&t->rx, &t->nrx, &t->caprx, tmp, n);
    }
    separar(t);
}

// ---------------------------------------------------------------- abrir e fechar

static void base64(const u8 *in, int n, char *out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        u32 v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = i + 1 < n ? t[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = 0;
}

static void liberar(Tunel *t) {
    if (t->c) curl_easy_cleanup(t->c);
    free(t->rx);
    free(t->flx);
    for (int i = 0; i < t->quantas; i++) free(t->msg[(t->inicio + i) % MAX_MSGS]);
    memset(t, 0, sizeof(*t));
}

int tunelAbrir(int porta, bool datagramas, int timeoutMs) {
    if (!endereco[0] && !tunelBuscarEndereco()) return -1;
    int i = 0;
    while (i < MAX_TUNEIS && tuneis[i].usado) i++;
    if (i == MAX_TUNEIS) return -1;
    Tunel *t = &tuneis[i];
    memset(t, 0, sizeof(*t));
    t->usado = true;
    t->datagramas = datagramas;
    t->c = curl_easy_init();
    if (!t->c) { liberar(t); return -1; }
    curl_easy_setopt(t->c, CURLOPT_URL, endereco);
    curl_easy_setopt(t->c, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(t->c, CURLOPT_CAINFO, CA);
    curl_easy_setopt(t->c, CURLOPT_CONNECTTIMEOUT_MS, (long)timeoutMs);
    podeCancelar(t->c);
    if (curl_easy_perform(t->c) != CURLE_OK ||
        curl_easy_getinfo(t->c, CURLINFO_ACTIVESOCKET, &t->fd) != CURLE_OK || t->fd == CURL_SOCKET_BAD) {
        liberar(t);
        endereco[0] = 0;          // o endereço pode ter mudado: busca de novo na próxima
        return -1;
    }

    // pedido de WebSocket
    const char *host = strstr(endereco, "://");
    host = host ? host + 3 : endereco;
    u8 chave[16];
    for (int k = 0; k < 16; k++) chave[k] = rand() & 0xFF;
    char chave64[32];
    base64(chave, 16, chave64);
    char pedido[512];
    int n = snprintf(pedido, sizeof(pedido),
                     "GET /t?porta=%d HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nAuthorization: Bearer %s\r\n"
                     "User-Agent: 3DSHub/" APP_VERSION "\r\n\r\n", porta, host, chave64, atual >= 0 ? pcs[atual].token : "");
    if (!enviarBruto(t, (const u8 *)pedido, n, timeoutMs)) { liberar(t); return -1; }

    // resposta: espera o "101" e guarda o que vier depois do cabeçalho
    u64 fim = osGetTime() + timeoutMs;
    char *fimCab = NULL;
    while (!fimCab) {
        static u8 tmp[2048];
        size_t k = 0;
        CURLcode r = curl_easy_recv(t->c, tmp, sizeof(tmp), &k);
        if (r == CURLE_AGAIN) {
            if (!esperarFd(t->fd, POLLIN, fim)) { liberar(t); return -1; }
            continue;
        }
        if (r != CURLE_OK || k == 0 || !anexar(&t->rx, &t->nrx, &t->caprx, tmp, k) || t->nrx > 8192) {
            liberar(t);
            return -1;
        }
        anexar(&t->rx, &t->nrx, &t->caprx, "", 1);   // termina a string para procurar
        t->nrx--;
        fimCab = strstr((char *)t->rx, "\r\n\r\n");
    }
    if (strncmp((char *)t->rx, "HTTP/1.1 101", 12)) {
        bool recusado = !strncmp((char *)t->rx, "HTTP/1.1 403", 12);
        liberar(t);
        if (recusado) endereco[0] = 0;
        return -1;
    }
    size_t usado = fimCab + 4 - (char *)t->rx;
    memmove(t->rx, t->rx + usado, t->nrx - usado);
    t->nrx -= usado;
    separar(t);
    return TUNEL_BASE + i;
}

void tunelFechar(int h) {
    Tunel *t = pegar(h);
    if (!t) return;
    if (!t->fechado) {
        u8 q[6] = { 0x88, 0x80, 0, 0, 0, 0 };          // "fechar", com máscara vazia
        enviarBruto(t, q, sizeof(q), 300);
    }
    liberar(t);
}

// ---------------------------------------------------------------- dados

bool tunelEnviar(int h, const void *dados, size_t n, int timeoutMs) {
    Tunel *t = pegar(h);
    if (!t || t->fechado) return false;
    static u8 *buf;
    static size_t cap;
    size_t total = n + 14;
    if (total > cap) {
        u8 *nb = realloc(buf, total);
        if (!nb) return false;
        buf = nb;
        cap = total;
    }
    size_t k = 0;
    buf[k++] = 0x82;                                   // mensagem binária, completa
    if (n < 126) buf[k++] = 0x80 | n;
    else if (n < 65536) { buf[k++] = 0x80 | 126; buf[k++] = n >> 8; buf[k++] = n & 0xFF; }
    else {
        buf[k++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) buf[k++] = (u64)n >> (i * 8);
    }
    u8 m[4];
    for (int i = 0; i < 4; i++) m[i] = buf[k++] = rand() & 0xFF;
    const u8 *p = dados;
    for (size_t i = 0; i < n; i++) buf[k + i] = p[i] ^ m[i & 3];
    return enviarBruto(t, buf, k + n, timeoutMs);
}

bool tunelReceberTudo(int h, void *dados, size_t n, int timeoutMs) {
    Tunel *t = pegar(h);
    if (!t) return false;
    u64 fim = osGetTime() + timeoutMs;
    while (true) {
        puxar(t);
        if (t->nflx >= n) {
            memcpy(dados, t->flx, n);
            memmove(t->flx, t->flx + n, t->nflx - n);
            t->nflx -= n;
            return true;
        }
        if (t->fechado) return false;
        if (!esperarFd(t->fd, POLLIN, fim)) return false;
        fim = osGetTime() + timeoutMs;   // o tempo vale para cada espera, como no TCP
    }
}

int tunelReceberDatagrama(int h, void *buf, size_t n) {
    Tunel *t = pegar(h);
    if (!t) return -1;
    if (!t->quantas) puxar(t);
    if (!t->quantas) return -1;
    int k = t->inicio;
    size_t tam = t->tamMsg[k] < n ? t->tamMsg[k] : n;
    memcpy(buf, t->msg[k], tam);
    free(t->msg[k]);
    t->inicio = (t->inicio + 1) % MAX_MSGS;
    t->quantas--;
    return (int)tam;
}
