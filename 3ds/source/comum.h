// Coisas usadas por todos os módulos do 3DS Hub.
#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>
#include <netinet/in.h>

#include "ui.h"

extern char ipPC[32];
extern struct in_addr enderecoPC;
extern char nomePC[32];             // nome do PC que respondeu à descoberta ("" = não sabe)
extern char caminhoApp[256];        // onde está o .3dsx que está rodando (para a atualização)
extern bool sairDoApp;              // um módulo pediu para fechar o app (ex.: depois de atualizar)

// Atualização pela internet (GitHub). automatico = a busca já foi feita ao abrir o app.
void atualizarPelaInternet(bool automatico);

// IP do PC (salvo em sdmc:/3ds/3dshub.cfg)
void carregarIP(void);
bool pedirIP(void);                 // teclado numérico; false se cancelar
bool definirIP(const char *ip);     // troca e salva; false se o IP for inválido

// Descoberta: pergunta para a rede inteira se tem algum 3DS Hub (UDP 5009).
int  descobertaAbrir(void);                       // socket sem bloqueio; -1 se falhar
void descobertaPerguntar(int sock);               // manda a pergunta (broadcast)
bool descobertaResposta(int sock, char *ip, size_t nIp, char *nome, size_t nNome);

// Chamada enquanto enviarTudo/receberTudo esperam a rede (a cada ~16 ms), para
// um módulo continuar lendo botões e toque nessas esperas. NULL = nada.
extern void (*aoEsperarRede)(void);

u32 crc32(u32 crc, const void *dados, size_t n);  // igual ao zlib.crc32 (comece com 0)

// Rede
int  abrirUDP(int porta, struct sockaddr_in *destino);    // não bloqueia
// Conecta sem travar: B ou START cancela. Retorna o socket, -1 se falhar ou -2 se cancelado.
int  conectarTCP(int porta, int timeoutMs);
#define CONEXAO_CANCELADA -2
bool enviarTudo(int sock, const void *dados, size_t n, int timeoutMs);
bool receberTudo(int sock, void *dados, size_t n, int timeoutMs);

// JPEG
bool jpegCodificar(const u16 *rgb565, int w, int h, int qualidade, u8 **saida, unsigned long *tam);
// Decodifica para RGB565 em 'destino' (larguraLinha pixels por linha).
bool jpegDecodificar(const u8 *dados, unsigned long tam, u16 *destino, int larguraLinha,
                     int maxW, int maxH, int *w, int *h);

// Módulos
void modControle(void);
void modTela(void);
void modSintetizador(void);
void modCamera(void);
void modEspelho(void);
void modAjustes(void);
