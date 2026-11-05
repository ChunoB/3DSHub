package io.github.chunob.hub3ds;

import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.media.AudioAttributes;
import android.media.Ringtone;
import android.media.RingtoneManager;
import android.net.TrafficStats;
import android.os.BatteryManager;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.StatFs;
import android.os.SystemClock;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.net.SocketAddress;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.text.Normalizer;
import java.util.Calendar;
import java.util.Locale;

/**
 * Segunda Tela (UDP 5006), o mesmo protocolo do PC (pc/tela.py): o 3DS manda "3DSH"
 * (oi) ou "3DSC"+comando e recebe "3DST\n" + linhas chave=valor. Também responde o
 * "3DSP" que o menu do 3DS usa para saber se o Hub está no ar.
 *
 * No celular não tem CPU/GPU para mostrar (o Android não deixa ler), então esses
 * campos vão como -1 e o 3DS mostra a tela do celular: música, bateria e volume.
 */
final class SegundaTela extends Thread {
    static final int PORTA = 5006;
    private static final int CMD_PLAY = 1, CMD_PROXIMA = 2, CMD_ANTERIOR = 3;
    private static final int CMD_VOL_MAIS = 4, CMD_VOL_MENOS = 5, CMD_MUDO = 6, CMD_ATALHO = 100;
    private static final String[] DIAS = { "dom", "seg", "ter", "qua", "qui", "sex", "sab" };
    private static final String[] ATALHOS = { "Lanterna", "Achar cel.", "Tocar/Pausa" };

    private final Context ctx;
    private final Midia midia;
    private final Handler principal = new Handler(Looper.getMainLooper());
    private DatagramSocket sock;
    private byte[] texto = new byte[0];
    private long rxAntes = -1, txAntes = -1, tAntes;
    private boolean lanterna;
    private Ringtone toque;

    SegundaTela(Context ctx) {
        super("segunda-tela");
        setDaemon(true);
        this.ctx = ctx.getApplicationContext();
        this.midia = new Midia(ctx);
    }

    static String limpo(String t, int max) {
        t = Normalizer.normalize(t == null ? "" : t, Normalizer.Form.NFC).replace('|', '/');
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < t.length(); i++) {
            char c = t.charAt(i);
            b.append(Character.isISOControl(c) ? ' ' : c);
        }
        String s = b.toString().trim();
        return s.length() > max ? s.substring(0, max) : s;
    }

    private void atualizar() {
        Calendar agora = Calendar.getInstance();
        Intent bat = ctx.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        int nivel = -1, carregando = 0;
        if (bat != null) {
            int l = bat.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), e = bat.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
            if (l >= 0 && e > 0) nivel = Math.round(l * 100f / e);
            carregando = bat.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0 ? 1 : 0;
        }
        ActivityManager am = (ActivityManager) ctx.getSystemService(Context.ACTIVITY_SERVICE);
        ActivityManager.MemoryInfo mem = new ActivityManager.MemoryInfo();
        am.getMemoryInfo(mem);
        long usada = mem.totalMem - mem.availMem;

        long rx = TrafficStats.getTotalRxBytes(), tx = TrafficStats.getTotalTxBytes();
        long t = SystemClock.elapsedRealtime();
        int down = 0, up = 0;
        if (rxAntes >= 0 && t > tAntes) {
            down = (int) ((rx - rxAntes) * 1000 / (t - tAntes) / 1024);
            up = (int) ((tx - txAntes) * 1000 / (t - tAntes) / 1024);
        }
        rxAntes = rx; txAntes = tx; tAntes = t;

        int disco = 0;
        try {
            StatFs fs = new StatFs(Environment.getDataDirectory().getPath());
            disco = Math.round(100f - fs.getAvailableBytes() * 100f / fs.getTotalBytes());
        } catch (Exception ignorado) {}

        String[] m = midia.tocando();
        StringBuilder s = new StringBuilder("3DST\n");
        s.append(String.format(Locale.ROOT, "hora=%02d:%02d\n", agora.get(Calendar.HOUR_OF_DAY), agora.get(Calendar.MINUTE)));
        s.append(String.format(Locale.ROOT, "data=%s %02d/%02d\n", DIAS[agora.get(Calendar.DAY_OF_WEEK) - 1],
                agora.get(Calendar.DAY_OF_MONTH), agora.get(Calendar.MONTH) + 1));
        s.append("cpu=-1\ngpu=-1\n");
        s.append("ram=").append(Math.round(usada * 100f / mem.totalMem)).append('\n');
        s.append(String.format(Locale.ROOT, "ramu=%.1f\nramt=%.1f\n", usada / 1073741824f, mem.totalMem / 1073741824f));
        s.append("disco=").append(disco).append('\n');
        s.append("down=").append(Math.max(0, down)).append("\nup=").append(Math.max(0, up)).append('\n');
        s.append("bat=").append(nivel).append("\ncarg=").append(carregando).append('\n');
        s.append("vol=").append(midia.volume()).append("\nmudo=").append(midia.mudo() ? 1 : 0).append('\n');
        s.append("titulo=").append(limpo(m[0], 48)).append('\n');
        s.append("artista=").append(limpo(m[1], 48)).append('\n');
        s.append("tocando=").append(m[2]).append('\n');
        s.append("celular=1\n");
        s.append("som=").append(Estado.somLigado ? 1 : 0).append('\n');
        Estado.somBloqueado = midia.capturaBloqueada();
        s.append("bloqueado=").append(Estado.somBloqueado ? 1 : 0).append('\n');
        s.append("atalhos=").append(String.join("|", ATALHOS)).append('\n');
        texto = s.toString().getBytes(StandardCharsets.UTF_8);
    }

    private void executar(int cmd) {
        try {
            switch (cmd) {
                case CMD_PLAY: midia.playPause(); break;
                case CMD_PROXIMA: midia.proxima(); break;
                case CMD_ANTERIOR: midia.anterior(); break;
                case CMD_VOL_MAIS: midia.volumeMais(); break;
                case CMD_VOL_MENOS: midia.volumeMenos(); break;
                case CMD_MUDO: midia.alternarMudo(); break;
                default:
                    if (cmd == CMD_ATALHO) alternarLanterna();
                    else if (cmd == CMD_ATALHO + 1) acharCelular();
                    else if (cmd == CMD_ATALHO + 2) midia.playPause();
            }
        } catch (Exception e) {
            android.util.Log.w("3dshub", "Comando " + cmd, e);
        }
    }

    private void alternarLanterna() throws Exception {
        CameraManager cm = (CameraManager) ctx.getSystemService(Context.CAMERA_SERVICE);
        for (String id : cm.getCameraIdList()) {
            Boolean flash = cm.getCameraCharacteristics(id).get(CameraCharacteristics.FLASH_INFO_AVAILABLE);
            if (Boolean.TRUE.equals(flash)) {
                lanterna = !lanterna;
                cm.setTorchMode(id, lanterna);
                return;
            }
        }
    }

    /** Toca o alarme por 15 s, mesmo no silencioso, para achar o celular. */
    private void acharCelular() {
        principal.post(() -> {
            if (toque != null && toque.isPlaying()) { toque.stop(); return; }
            toque = RingtoneManager.getRingtone(ctx, RingtoneManager.getDefaultUri(RingtoneManager.TYPE_ALARM));
            if (toque == null) return;
            toque.setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_ALARM).build());
            toque.play();
            principal.postDelayed(() -> { if (toque != null) toque.stop(); }, 15000);
        });
    }

    @Override public void run() {
        try {
            sock = new DatagramSocket(null);
            sock.setReuseAddress(true);
            sock.bind(new InetSocketAddress(PORTA));
            sock.setSoTimeout(200);
            byte[] buf = new byte[256];
            long ultima = 0;
            while (!isInterrupted()) {
                long agora = SystemClock.elapsedRealtime();
                if (agora - ultima >= 1000) {
                    try { atualizar(); } catch (Exception e) { android.util.Log.w("3dshub", "Segunda Tela", e); }
                    ultima = agora;
                }
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                try {
                    sock.receive(p);
                } catch (SocketTimeoutException e) {
                    continue;
                }
                if (p.getLength() < 4 || buf[0] != '3' || buf[1] != 'D' || buf[2] != 'S') continue;
                SocketAddress origem = p.getSocketAddress();
                Estado.viu3ds(p.getAddress().getHostAddress());
                if (buf[3] == 'P') {                       // ping do menu do 3DS
                    responder("3DSP".getBytes(StandardCharsets.US_ASCII), origem);
                    continue;
                }
                if (buf[3] == 'B' && p.getLength() > 4) {    // buscar música (texto UTF-8)
                    String busca = new String(buf, 4, p.getLength() - 4, StandardCharsets.UTF_8).trim();
                    if (!busca.isEmpty()) midia.buscar(busca);
                    continue;
                }
                if (buf[3] != 'H' && buf[3] != 'C') continue;
                if (buf[3] == 'C' && p.getLength() >= 5) {
                    int cmd = buf[4] & 0xFF;
                    executar(cmd);
                    if (cmd < CMD_ATALHO) {                // mostra o volume novo na hora
                        try { Thread.sleep(60); } catch (InterruptedException e) { return; }
                        atualizar();
                        ultima = SystemClock.elapsedRealtime();
                    }
                }
                responder(texto, origem);
            }
        } catch (Exception e) {
            if (!isInterrupted()) android.util.Log.w("3dshub", "Segunda Tela parou", e);
        }
    }

    private void responder(byte[] dados, SocketAddress para) {
        try {
            sock.send(new DatagramPacket(dados, dados.length, para));
        } catch (Exception ignorado) {}
    }

    void parar() {
        interrupt();
        if (sock != null) sock.close();
        if (lanterna) try { alternarLanterna(); } catch (Exception ignorado) {}
    }
}
