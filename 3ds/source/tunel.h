// Acesso pela internet: conexões com o PC pelo túnel da Cloudflare (HTTPS + WebSocket).
// Cada conexão pede uma porta do Hub; o portão do PC liga ela ao serviço de sempre.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define TUNEL_BASE    0x4000       // "sockets" do túnel são números a partir daqui
#define TUNEL_MAX_PCS 8

// PCs pareados (cada um pareado uma vez, em casa, na mesma rede dele)
bool tunelPareado(void);           // tem algum PC escolhido para a internet
int  tunelQuantos(void);
const char *tunelNomePC(int i);    // nome do Windows, o mesmo que a descoberta mostra
int  tunelAtual(void);             // PC usado pela internet (-1 = nenhum)
int  tunelIndice(const char *nome);    // -1 se esse PC não está pareado
void tunelEscolher(int i);
bool tunelSalvar(const char *nome, const char *token, const char *topico);   // pareia (ou atualiza) e escolhe
void tunelEsquecer(int i);

// Pareia longe de casa com o código que o PC mostra (ex.: "K7QM-2XPA"): lê os dados no canal
// do ntfy desse código. 1 = pareou (nome do PC em 'nome'), 0 = código não achado ou vencido,
// -1 = sem internet, -2 = 8 PCs já pareados ou erro ao gravar.
int  tunelParearCodigo(const char *codigo, char *nome, size_t nNome);

// Descobre o endereço atual do PC escolhido (lê o canal do ntfy). Bloqueia alguns segundos.
bool tunelBuscarEndereco(void);
const char *tunelEndereco(void);   // "" se ainda não sabe

// datagramas = true: uma mensagem por pacote (portas UDP); false: fluxo de bytes (TCP)
int  tunelAbrir(int porta, bool datagramas, int timeoutMs);    // handle ou -1
bool ehTunel(int h);
void tunelFechar(int h);
bool tunelEnviar(int h, const void *dados, size_t n, int timeoutMs);
bool tunelReceberTudo(int h, void *dados, size_t n, int timeoutMs);   // fluxo
int  tunelReceberDatagrama(int h, void *buf, size_t n);               // sem bloquear; -1 = nada
