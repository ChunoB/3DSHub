// Acesso pela internet: conexões com o PC pelo túnel da Cloudflare (HTTPS + WebSocket).
// Cada conexão pede uma porta do Hub; o portão do PC liga ela ao serviço de sempre.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define TUNEL_BASE 0x4000          // "sockets" do túnel são números a partir daqui

bool tunelPareado(void);           // tem código salvo (pareou em casa)
bool tunelSalvar(const char *token, const char *topico);
void tunelEsquecer(void);
// Descobre o endereço atual do PC (lê o canal do ntfy). Bloqueia alguns segundos.
bool tunelBuscarEndereco(void);
const char *tunelEndereco(void);   // "" se ainda não sabe

// datagramas = true: uma mensagem por pacote (portas UDP); false: fluxo de bytes (TCP)
int  tunelAbrir(int porta, bool datagramas, int timeoutMs);    // handle ou -1
bool ehTunel(int h);
void tunelFechar(int h);
bool tunelEnviar(int h, const void *dados, size_t n, int timeoutMs);
bool tunelReceberTudo(int h, void *dados, size_t n, int timeoutMs);   // fluxo
int  tunelReceberDatagrama(int h, void *buf, size_t n);               // sem bloquear; -1 = nada
