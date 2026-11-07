package io.github.chunob.hub3ds;

import android.annotation.SuppressLint;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioPlaybackCaptureConfiguration;
import android.media.AudioRecord;
import android.media.projection.MediaProjection;
import android.os.SystemClock;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.net.SocketAddress;
import java.net.SocketTimeoutException;

/**
 * Som do celular tocando no 3DS (UDP 5013), o mesmo protocolo do PC (pc/som.py).
 *
 * Enquanto quer som, o 3DS manda "3DSU" a cada segundo. O celular grava o que os
 * apps estão tocando (o Android chama isso de "captura de áudio", e pede permissão
 * igual à gravação de tela), passa para 22050 Hz mono, compacta em IMA ADPCM e
 * manda em pacotes de 512 amostras:
 *     "3DSS" + u32 seq + s16 previsão + u8 índice + u8 0 + 256 bytes
 * Sem "3DSU" por 3 segundos, para de gravar.
 *
 * Alguns apps proíbem a captura do som deles (o Spotify, por exemplo); aí o 3DS
 * recebe silêncio e a Segunda Tela avisa.
 *
 * Enquanto o som vai para o 3DS, o celular fica mudo (opção do app). Em alguns
 * celulares a captura pega o som antes do volume e o 3DS continua ouvindo. Em outros
 * ela pega depois, e o mudo cala o 3DS junto: o app percebe e passa a usar o volume
 * mínimo do celular (quase inaudível), compensando com mais ganho para o 3DS.
 */
final class Som {
    static final int PORTA = 5013;
    private static final int TAXA_SAIDA = 22050, TAXA_ENTRADA = 48000, AMOSTRAS = 512;
    private static final float GANHO_MAXIMO = 8f, GANHO_MAXIMO_BAIXO = 160f, ALVO = 0.9f;

    private final MediaProjection projecao;
    private final AudioManager audio;
    private DatagramSocket sock;
    private volatile SocketAddress destino;
    private volatile long ultimoOi;
    private Thread escuta, gravacao;
    private volatile boolean rodando;

    Som(MediaProjection projecao, AudioManager audio) {
        this.projecao = projecao;
        this.audio = audio;
    }

    // ------------------------------------------------------------ celular mudo
    private int volumeGuardado = -1;

    private void calar() {
        if (Estado.calado || !Estado.querCalar || Estado.baixoNaoFunciona || Estado.somBloqueado) return;
        if (!Estado.calarNaoFunciona) {
            audio.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_MUTE, 0);
            Estado.modoBaixo = false;
        } else {
            volumeGuardado = audio.getStreamVolume(AudioManager.STREAM_MUSIC);
            audio.setStreamVolume(AudioManager.STREAM_MUSIC, Math.max(1, audio.getStreamMinVolume(AudioManager.STREAM_MUSIC)), 0);
            Estado.modoBaixo = true;
        }
        Estado.calado = true;
    }

    private void descalar() {
        if (!Estado.calado) return;
        Estado.calado = false;
        if (Estado.modoBaixo) {
            if (volumeGuardado >= 0) audio.setStreamVolume(AudioManager.STREAM_MUSIC, volumeGuardado, 0);
            volumeGuardado = -1;
        } else {
            audio.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_UNMUTE, 0);
        }
        Estado.modoBaixo = false;
    }

    /** O 3DS está pedindo o som agora. */
    private boolean somQuerido() {
        return destino != null && SystemClock.elapsedRealtime() - ultimoOi < 3000;
    }

    /** A captura roda para o som do 3DS ou para o Karaokê (que ouve as notas da música). */
    private boolean ativo() {
        return somQuerido() || Karaoke.querOuvir();
    }

    void iniciar() throws Exception {
        sock = new DatagramSocket(null);
        sock.setReuseAddress(true);
        sock.bind(new InetSocketAddress(PORTA));
        sock.setSoTimeout(1000);
        rodando = true;
        escuta = new Thread(this::escutar, "som-escuta");
        gravacao = new Thread(this::gravar, "som");
        escuta.setDaemon(true);
        gravacao.setDaemon(true);
        escuta.start();
        gravacao.start();
        Estado.somLigado = true;
    }

    void parar() {
        rodando = false;
        descalar();
        Estado.somLigado = false;
        Estado.somPedido = false;
        if (sock != null) sock.close();
        if (escuta != null) escuta.interrupt();
        if (gravacao != null) gravacao.interrupt();
    }

    private void escutar() {
        byte[] buf = new byte[64];
        while (rodando) {
            DatagramPacket p = new DatagramPacket(buf, buf.length);
            try {
                sock.receive(p);
            } catch (SocketTimeoutException e) {
                continue;
            } catch (Exception e) {
                return;
            }
            if (p.getLength() >= 4 && buf[0] == '3' && buf[1] == 'D' && buf[2] == 'S' && buf[3] == 'U') {
                destino = p.getSocketAddress();
                ultimoOi = SystemClock.elapsedRealtime();
                Estado.viu3ds(p.getAddress().getHostAddress());
            }
        }
    }

    @SuppressLint("MissingPermission")   // a MainActivity pede o microfone antes de ligar o som
    private AudioRecord abrirCaptura() {
        AudioPlaybackCaptureConfiguration config = new AudioPlaybackCaptureConfiguration.Builder(projecao)
                .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                .addMatchingUsage(AudioAttributes.USAGE_GAME)
                .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
                .build();
        AudioFormat formato = new AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(TAXA_ENTRADA)
                .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                .build();
        int minimo = AudioRecord.getMinBufferSize(TAXA_ENTRADA, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        return new AudioRecord.Builder()
                .setAudioFormat(formato)
                .setBufferSizeInBytes(Math.max(minimo, TAXA_ENTRADA * 4 / 5))
                .setAudioPlaybackCaptureConfig(config)
                .build();
    }

    private void gravar() {
        while (rodando) {
            if (!ativo()) {
                Estado.somPedido = false;
                SystemClock.sleep(200);
                continue;
            }
            AudioRecord rec = null;
            try {
                rec = abrirCaptura();
                rec.startRecording();
                Estado.erroSom = "";
                transmitir(rec);
            } catch (Exception e) {
                Estado.erroSom = e.getMessage() == null ? e.toString() : e.getMessage();
                android.util.Log.w("3dshub", "Som", e);
                SystemClock.sleep(2000);
            } finally {
                descalar();
                if (rec != null) {
                    try { rec.stop(); } catch (Exception ignorado) {}
                    rec.release();
                }
            }
        }
    }

    private void transmitir(AudioRecord rec) throws Exception {
        short[] entrada = new short[2048 * 2];
        short[] pacote = new short[AMOSTRAS];
        float[] ouvido = new float[2048];
        byte[] saida = new byte[12 + AMOSTRAS / 2];
        saida[0] = '3'; saida[1] = 'D'; saida[2] = 'S'; saida[3] = 'S';
        Adpcm adpcm = new Adpcm();
        int seq = 0, cheio = 0;
        double pos = 0;                 // posição (em amostras de entrada) da próxima amostra de saída
        final double passo = (double) TAXA_ENTRADA / TAXA_SAIDA;
        float anterior = 0, pico = 0, ganho = 1;
        long caladoDesde = 0, ultimoSom = SystemClock.elapsedRealtime();

        while (rodando && ativo()) {
            int n = rec.read(entrada, 0, entrada.length);
            if (n <= 0) continue;
            int quadros = n / 2;
            boolean querido = somQuerido();
            Estado.somPedido = querido;

            // celular mudo: liga, desliga e confere se o 3DS continua recebendo som
            long agora = SystemClock.elapsedRealtime();
            int maior = 0;
            for (int i = 0; i < n; i++) maior = Math.max(maior, Math.abs((int) entrada[i]));
            if (maior > 3) ultimoSom = agora;
            Estado.nivelSom = Math.max(maior * 100 / 32767, Estado.nivelSom * 9 / 10);
            boolean querMudo = querido && Estado.querCalar && !Estado.somBloqueado && !Estado.baixoNaoFunciona;
            if (querMudo && !Estado.calado) { calar(); caladoDesde = agora; }
            else if (!querMudo && Estado.calado) descalar();
            if (Estado.calado && audio.isMusicActive() && agora - caladoDesde > 4000 && agora - ultimoSom > 4000) {
                // tocando, mas a captura ficou muda desde que calou: o mudo não serve neste
                // celular (tenta o volume mínimo); se nem ele serve, o som sai nos dois
                if (Estado.modoBaixo) Estado.baixoNaoFunciona = true;
                else Estado.calarNaoFunciona = true;
                descalar();
                pico = 0;
            }
            float volume = Estado.calado ? Estado.volume3ds / 100f : 1f;
            boolean karaoke = Karaoke.querOuvir();
            int nOuvido = 0;
            // mono + passa-baixa simples (média com a amostra anterior) e reamostragem
            float[] mono = new float[quadros + 1];
            mono[0] = anterior;
            for (int i = 0; i < quadros; i++) {
                float m = (entrada[2 * i] + entrada[2 * i + 1]) / 65536f;
                mono[i + 1] = (m + mono[i]) * 0.5f;
            }
            anterior = mono[quadros];
            while (pos < quadros) {
                int i = (int) pos;
                float f = (float) (pos - i);
                float x = mono[i] * (1 - f) + mono[i + 1] * f;
                if (karaoke) {
                    if (nOuvido == ouvido.length) { Karaoke.ouvir(ouvido, nOuvido); nOuvido = 0; }
                    ouvido[nOuvido++] = x;
                }

                // ganho automático (sobe devagar, desce na hora) + limitador suave, como no PC
                float a = Math.abs(x);
                pico = Math.max(a, pico * 0.999994f);
                float maximo = Estado.modoBaixo ? GANHO_MAXIMO_BAIXO : GANHO_MAXIMO;
                float alvo = Math.min(maximo, ALVO / Math.max(pico, 1e-5f));
                ganho = alvo < ganho ? alvo : ganho + (alvo - ganho) * 0.00012f;
                float y = Math.max(-1.5f, Math.min(1.5f, x * ganho));
                y = y - y * y * y / 6.75f;

                pacote[cheio++] = (short) Math.round(Math.max(-1f, Math.min(1f, y)) * 32767 * volume);
                if (cheio == AMOSTRAS) {
                    int previsao = adpcm.previsao, indice = adpcm.indice;
                    saida[4] = (byte) seq; saida[5] = (byte) (seq >> 8);
                    saida[6] = (byte) (seq >> 16); saida[7] = (byte) (seq >> 24);
                    saida[8] = (byte) previsao; saida[9] = (byte) (previsao >> 8);
                    saida[10] = (byte) indice; saida[11] = 0;
                    adpcm.codificar(pacote, AMOSTRAS, saida, 12);
                    SocketAddress d = destino;
                    if (d != null && querido) sock.send(new DatagramPacket(saida, saida.length, d));
                    seq++;
                    cheio = 0;
                }
                pos += passo;
            }
            pos -= quadros;
            if (karaoke && nOuvido > 0) Karaoke.ouvir(ouvido, nOuvido);
        }
    }
}
