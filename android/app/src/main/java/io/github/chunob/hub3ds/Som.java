package io.github.chunob.hub3ds;

import android.annotation.SuppressLint;
import android.media.AudioAttributes;
import android.media.AudioFormat;
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
 * Alguns apps proíbem a captura do som deles; aí o 3DS recebe silêncio.
 */
final class Som {
    static final int PORTA = 5013;
    private static final int TAXA_SAIDA = 22050, TAXA_ENTRADA = 48000, AMOSTRAS = 512;
    private static final float GANHO_MAXIMO = 8f, ALVO = 0.9f;

    private final MediaProjection projecao;
    private DatagramSocket sock;
    private volatile SocketAddress destino;
    private volatile long ultimoOi;
    private Thread escuta, gravacao;
    private volatile boolean rodando;

    Som(MediaProjection projecao) { this.projecao = projecao; }

    private boolean ativo() {
        return destino != null && SystemClock.elapsedRealtime() - ultimoOi < 3000;
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
            Estado.somPedido = true;
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
        byte[] saida = new byte[12 + AMOSTRAS / 2];
        saida[0] = '3'; saida[1] = 'D'; saida[2] = 'S'; saida[3] = 'S';
        Adpcm adpcm = new Adpcm();
        int seq = 0, cheio = 0;
        double pos = 0;                 // posição (em amostras de entrada) da próxima amostra de saída
        final double passo = (double) TAXA_ENTRADA / TAXA_SAIDA;
        float anterior = 0, pico = 0, ganho = 1;

        while (rodando && ativo()) {
            int n = rec.read(entrada, 0, entrada.length);
            if (n <= 0) continue;
            int quadros = n / 2;
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

                // ganho automático (sobe devagar, desce na hora) + limitador suave, como no PC
                float a = Math.abs(x);
                pico = Math.max(a, pico * 0.999994f);
                float alvo = Math.min(GANHO_MAXIMO, ALVO / Math.max(pico, 1e-4f));
                ganho = alvo < ganho ? alvo : ganho + (alvo - ganho) * 0.00012f;
                float y = Math.max(-1.5f, Math.min(1.5f, x * ganho));
                y = y - y * y * y / 6.75f;

                pacote[cheio++] = (short) Math.round(Math.max(-1f, Math.min(1f, y)) * 32767);
                if (cheio == AMOSTRAS) {
                    int previsao = adpcm.previsao, indice = adpcm.indice;
                    saida[4] = (byte) seq; saida[5] = (byte) (seq >> 8);
                    saida[6] = (byte) (seq >> 16); saida[7] = (byte) (seq >> 24);
                    saida[8] = (byte) previsao; saida[9] = (byte) (previsao >> 8);
                    saida[10] = (byte) indice; saida[11] = 0;
                    adpcm.codificar(pacote, AMOSTRAS, saida, 12);
                    SocketAddress d = destino;
                    if (d != null) sock.send(new DatagramPacket(saida, saida.length, d));
                    seq++;
                    cheio = 0;
                }
                pos += passo;
            }
            pos -= quadros;
        }
    }
}
