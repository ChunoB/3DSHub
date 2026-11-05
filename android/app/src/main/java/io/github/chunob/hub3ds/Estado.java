package io.github.chunob.hub3ds;

/** O que a tela do app mostra: atualizado pelos serviços, lido pela MainActivity. */
final class Estado {
    static volatile boolean servicoRodando;
    static volatile String ip3ds = "";          // de quem veio o último "oi" do 3DS
    static volatile long ultimoOi3ds;           // quando (SystemClock.elapsedRealtime)
    static volatile boolean somLigado;          // a captura do som está rodando
    static volatile boolean somPedido;          // o 3DS está pedindo o som agora
    static volatile String erroSom = "";
    static volatile boolean querCalar = true;   // opção do app: o som sai só no 3DS
    static volatile boolean calado;             // o celular está mudo enquanto o som vai para o 3DS
    static volatile boolean calarNaoFunciona;   // neste celular, calar também cala o som do 3DS
    static volatile int volume3ds = 80;         // volume do som que vai para o 3DS (0-100) quando calado
    static volatile boolean somBloqueado;       // o app tocando agora proíbe a captura do som (ex.: Spotify)

    static boolean conectado() {
        return ultimoOi3ds != 0 && android.os.SystemClock.elapsedRealtime() - ultimoOi3ds < 4000;
    }

    static void viu3ds(String ip) {
        ip3ds = ip;
        ultimoOi3ds = android.os.SystemClock.elapsedRealtime();
    }

    private Estado() {}
}
