// Kit visual do 3DS Hub (citro2d): cores, texto com a fonte do sistema,
// cartões arredondados, ícones desenhados, barra de status e imagens.
#pragma once

#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

// ---------------------------------------------------------------- cores
// Mesmo resultado de C2D_Color32(r, g, b, 255), mas como conta pura: assim
// pode ser usado em tabelas fixas (o C não deixa chamar função ali).
#define HEX(rgb) ((u32)((((rgb) >> 16) & 0xFF) | ((rgb) & 0xFF00) | (((rgb) & 0xFF) << 16) | 0xFF000000u))

#define COR_FUNDO      HEX(0x151823)
#define COR_BARRA      HEX(0x1C2030)
#define COR_CARTAO     HEX(0x20253A)
#define COR_CARTAO2    HEX(0x2A3048)
#define COR_LINHA      HEX(0x343B57)
#define COR_BORDA      HEX(0x2C3250)   // contorno fino dos cartões
#define COR_TEXTO      HEX(0xE8EAF2)
#define COR_TEXTO2     HEX(0xB8BDD0)
#define COR_MUDO       HEX(0x9AA0B5)
#define COR_ESCURO     HEX(0x0E1018)

#define COR_ROXO       HEX(0x7F77DD)   // Controle
#define COR_ROXO_CL    HEX(0xAFA9EC)
#define COR_VERDE_AGUA HEX(0x1D9E75)   // Segunda Tela
#define COR_AGUA_CL    HEX(0x5DCAA5)
#define COR_AGUA_ESC   HEX(0x04342C)
#define COR_ROSA       HEX(0xD4537E)   // Sintetizador
#define COR_ROSA_CL    HEX(0xED93B1)
#define COR_CORAL      HEX(0xD85A30)   // Câmera
#define COR_CORAL_CL   HEX(0xF0997B)
#define COR_AZUL       HEX(0x378ADD)   // Espelho
#define COR_AZUL_CL    HEX(0x85B7EB)
#define COR_STEAM      HEX(0x5C8A1E)   // Steam (o verde do botão Jogar)
#define COR_STEAM_CL   HEX(0xA4D007)
#define COR_BICHO      HEX(0xC77A2E)   // Bichinho (laranja-caramelo)
#define COR_BICHO_CL   HEX(0xF2B872)
#define COR_KARAOKE    HEX(0xB0399E)   // Karaokê (magenta)
#define COR_KARAOKE_CL HEX(0xE88BD8)
#define COR_PESCA      HEX(0x1F7FA6)   // Pescaria (azul do mar)
#define COR_PESCA_CL   HEX(0x7FD3EE)
#define COR_VERDE      HEX(0x97C459)
#define COR_AMARELO    HEX(0xEF9F27)
#define COR_VERMELHO   HEX(0xE24B4A)
#define COR_BRANCO     HEX(0xF4F5FA)

// ---------------------------------------------------------------- telas
extern C3D_RenderTarget *alvoCima, *alvoCimaDir, *alvoBaixo;

void uiIniciar(void);
void uiEncerrar(void);
void uiQuadro(void);                 // começa um quadro (limpa o texto do quadro anterior)
void uiTela(C3D_RenderTarget *alvo); // passa a desenhar nesta tela (já limpa com o fundo)
void uiFim(void);                    // termina e mostra o quadro (espera o vblank)
// Igual ao uiTela, mas com o fundo em degradê (corCima no alto, corBaixo embaixo).
void uiTelaGrad(C3D_RenderTarget *alvo, u32 corCima, u32 corBaixo);

// ---------------------------------------------------------------- cor e movimento
u32   uiMistura(u32 a, u32 b, float t);              // t = 0 dá a, t = 1 dá b
u32   uiAlfa(u32 cor, float alfa);                   // a mesma cor, transparente (0 a 1)
// Chega perto do alvo um pouco a cada quadro (rápido no começo, suave no fim).
float uiSuave(float atual, float alvo, float taxa);

// ---------------------------------------------------------------- formas
void uiRet(float x, float y, float w, float h, u32 cor);
void uiRetArred(float x, float y, float w, float h, float raio, u32 cor);
void uiCirculo(float cx, float cy, float r, u32 cor);
void uiAnel(float cx, float cy, float r, float espessura, u32 cor, u32 corDentro);
void uiLinha(float x0, float y0, float x1, float y1, float espessura, u32 cor);
void uiTri(float x0, float y0, float x1, float y1, float x2, float y2, u32 cor);
void uiQuad(float cx, float cy, float w, float h, float angulo, u32 cor);   // retângulo girado
void uiBarra(float x, float y, float w, float h, float fracao, u32 cor, u32 corFundo);
void uiRetGrad(float x, float y, float w, float h, u32 corCima, u32 corBaixo);
// Cartão padrão: COR_CARTAO com um contorno fino (destaca do fundo sem pesar).
void uiCartao(float x, float y, float w, float h, float raio);
// Item de lista ou aba: escolhido = cor cheia; senão, o cartão padrão.
void uiCartaoSel(float x, float y, float w, float h, float raio, bool escolhido, u32 corEscolhido);
// Cartão de outra cor, com o contorno dado.
void uiCartaoCor(float x, float y, float w, float h, float raio, u32 cor, u32 corBorda);

// ---------------------------------------------------------------- texto
enum { ESQ, CENTRO, DIR };
// Desenha texto (UTF-8, com acentos). y = topo da linha; tamanho em pixels.
// larguraMax > 0 corta com "..." se não couber. Retorna a largura desenhada.
float uiTexto(float x, float y, float tamanho, u32 cor, int alinhamento, float larguraMax,
              const char *fmt, ...) __attribute__((format(printf, 7, 8)));
// Largura que o texto teria nesse tamanho (só mede, não desenha).
float uiLarguraTexto(const char *s, float tamanho);
// O mesmo, para um texto que vai ser desenhado com larguraMax (cortado com "...").
float uiLarguraTextoMax(const char *s, float tamanho, float larguraMax);

// ---------------------------------------------------------------- ícones
enum {
    IC_CONTROLE, IC_GRAFICO, IC_PIANO, IC_CAMERA, IC_ESPELHO, IC_ENGRENAGEM,
    IC_PLAY, IC_PAUSE, IC_PROXIMA, IC_ANTERIOR, IC_VOL_MAIS, IC_VOL_MENOS, IC_MUDO,
    IC_MUSICA, IC_PC, IC_OK, IC_X, IC_SETA_BAIXO, IC_SETA_CIMA, IC_LUPA, IC_LED,
    IC_RAIO, IC_LISTA, IC_TELA_CHEIA, IC_GIRO, IC_TECLADO,
    IC_BICHO, IC_MICROFONE, IC_PEIXE,
};
// corFundo = cor por trás do ícone (usada para "recortar" detalhes)
void uiIcone(int icone, float cx, float cy, float tamanho, u32 cor, u32 corFundo);

// ---------------------------------------------------------------- componentes
// Barra de status no topo da tela de cima: ícone + título do módulo à
// esquerda; hora, Wi-Fi e bateria à direita. Altura UI_BARRA.
#define UI_BARRA 22
void uiBarraStatus(const char *titulo, int icone, u32 cor);

// Botão (cartão arredondado com ícone e/ou texto). icone < 0 = sem ícone.
void uiBotao(float x, float y, float w, float h, const char *rotulo, int icone,
             u32 corFundo, u32 corConteudo);

bool uiDentro(const touchPosition *tp, float x, float y, float w, float h);

// Dica de botão: o desenho do botão ("A", "START", "L/R"...) e o que ele faz.
// y = topo da linha (altura UI_DICA). Retorna a largura desenhada.
#define UI_DICA 14
float uiDica(float x, float y, const char *botao, const char *texto);
float uiLarguraDica(const char *botao, const char *texto);
// Várias dicas numa linha só: pares (botão, texto) terminados por NULL.
// ESQ começa em x, CENTRO centra em x, DIR termina em x.
void uiDicas(float x, float y, int alinhamento, ...) __attribute__((sentinel));

// Barra de rolagem fina de uma lista (total de itens, quantos cabem, o primeiro visível).
void uiRolagem(float x, float y, float h, int total, int visiveis, int primeiro, u32 cor);

// Tela de aviso (erro de rede etc.): fica até apertar A, B ou START (ou tocar em OK).
void uiAviso(const char *titulo, const char *texto, int icone, u32 cor);

// Pergunta com dois botões grandes: A ou o da esquerda = sim; B, START ou o da direita = não.
// modulo/icone/cor = a barra de status de quem pergunta. aCadaQuadro (ou NULL) roda a cada
// quadro, para o módulo continuar conversando com o PC enquanto espera a resposta.
bool uiConfirmar(const char *modulo, int icone, u32 cor, const char *titulo, const char *texto,
                 const char *sim, const char *nao, void (*aCadaQuadro)(void));

// ---------------------------------------------------------------- sons da interface (uisom.c)
// O tique ao andar pelos ícones, abrir, voltar e erro. Só tocam onde foram permitidos
// (menu e Ajustes); nos outros módulos uiSomTocar não faz nada.
enum { SOM_MOVER, SOM_TIQUE, SOM_ABRIR, SOM_VOLTAR, SOM_ERRO, N_SONS };
void uiSomPermitir(bool sim);   // false antes de abrir um módulo (desliga o NDSP de vez)
void uiSomTocar(int som);
void uiSomPausar(void);         // antes do teclado do sistema: desliga; o próximo som religa
void uiSomQuadro(void);         // a cada quadro (o uiQuadro já chama): HOME e descanso
void uiSomFim(void);            // ao fechar o app
bool uiSomQuer(void);           // a opção dos Ajustes
void uiSomDefinir(bool ligado);
bool uiSomFunciona(void);       // false = o NDSP não ligou (falta o dspfirm.cdc)

// ---------------------------------------------------------------- imagens
// Imagem RGB565 que pode mudar a cada quadro (prévia da câmera, espelho).
// Escreva os pixels em 'pixels' (linha 0 = topo, largura de linha = UI_IMG_LARG)
// e chame uiImagemEnviar() fora do uiQuadro/uiFim.
#define UI_IMG_LARG 512
#define UI_IMG_ALT  256
typedef struct {
    C3D_Tex tex;
    Tex3DS_SubTexture sub;
    C2D_Image img;
    u16 *pixels;          // memória linear, UI_IMG_LARG x UI_IMG_ALT
    int w, h;             // tamanho da imagem dentro da textura
    bool ok;
} UiImagem;

bool uiImagemCriar(UiImagem *im, int w, int h);
// Igual, mas com transparência: pixels RGBA4444 (R nos 4 bits de cima, A nos de baixo).
bool uiImagemCriarAlfa(UiImagem *im, int w, int h);
void uiImagemEnviar(UiImagem *im);
void uiImagemDesenhar(UiImagem *im, float x, float y);
void uiImagemDesenharEscala(UiImagem *im, float x, float y, float escala);
// Um pedaço da imagem (ex.: várias capas pequenas numa textura só): envia só a
// região que mudou / desenha só o retângulo (sx, sy, w, h) dela.
void uiImagemEnviarParte(UiImagem *im, int x0, int y0, int w, int h);
// Ampliada: suave = mistura os pixels (padrão); senão, pixels quadrados (visual retrô).
void uiImagemFiltro(UiImagem *im, bool suave);
void uiImagemDesenharParte(UiImagem *im, int sx, int sy, int w, int h, float x, float y, float escala);
void uiImagemLiberar(UiImagem *im);
