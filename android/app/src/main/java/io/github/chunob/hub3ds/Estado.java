package io.github.chunob.hub3ds;

/** O que a tela do app mostra: atualizado pelos serviços, lido pela MainActivity. */
final class Estado {
    static volatile boolean servicoRodando;
    static volatile String ip3ds = "";          // de quem veio o último "oi" do 3DS
    static volatile long ultimoOi3ds;           // quando (SystemClock.elapsedRealtime)
    static volatile boolean somLigado;          // a captura do som está rodando
    static volatile boolean somPedido;          // o 3DS está pedindo o som agora
    static volatile String erroSom = "";

    static boolean conectado() {
        return ultimoOi3ds != 0 && android.os.SystemClock.elapsedRealtime() - ultimoOi3ds < 4000;
    }

    static void viu3ds(String ip) {
        ip3ds = ip;
        ultimoOi3ds = android.os.SystemClock.elapsedRealtime();
    }

    private Estado() {}
}
