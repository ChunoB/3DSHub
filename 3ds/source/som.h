// Som do PC tocando no 3DS (ver som.c).
#pragma once

#include <3ds.h>
#include <stdbool.h>

bool somIniciar(void);      // liga o som e pede ao PC; false = sem NDSP (falta o dspfirm.cdc)
void somEncerrar(void);     // desliga (antes do HOME e ao sair)
void somAtualizar(void);    // chame sempre (a cada ~16 ms): pede ao PC e guarda o que chegou
bool somLigado(void);
bool somChegando(void);     // chegou som do PC no último segundo

// Aviso "falta o dspfirm.cdc" (o mesmo para todo módulo que toca som), na cor do módulo.
void somAvisoSemDsp(u32 cor);

// Com o som (NDSP) ligado, deixar o libctru suspender o app pelo HOME pode travar o
// console esperando o DSP. O módulo desliga o HOME automático (aptSetHomeAllowed(false))
// e chama isto no começo de cada volta do laço: se o HOME foi apertado, o som é
// desligado, o app vai para o HOME e, na volta, o som é religado.
enum { SOM_HOME_NADA, SOM_HOME_VOLTOU, SOM_HOME_FECHAR };
int somCuidarDoHome(void);

