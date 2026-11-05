package io.github.chunob.hub3ds;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.PowerManager;
import android.os.SystemClock;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.Calendar;

/**
 * Bichinho (UDP 5014), o mesmo protocolo do PC (pc/bicho.py): o 3DS manda "3DSB" e
 * recebe "3DSB" + linhas chave=valor. No celular o bichinho sente outras coisas:
 *   - dança com a música do celular;
 *   - sua com o celular quente (temperatura da bateria; vai no campo "cpu");
 *   - fica bravo com muitas notificações (vão no campo "janelas");
 *   - dorme com a tela apagada há uns minutos ("ocioso") ou de madrugada.
 * Skins ainda não: "skin=0".
 */
final class Bicho extends Thread {
    static final int PORTA = 5014;
    private final Context ctx;
    private final Midia midia;
    private DatagramSocket sock;
    private long telaApagouEm;

    Bicho(Context ctx) {
        super("bicho");
        setDaemon(true);
        this.ctx = ctx.getApplicationContext();
        this.midia = new Midia(ctx);
    }

    private byte[] resposta() {
        Intent bat = ctx.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        int temp = 30, nivel = -1;
        if (bat != null) {
            temp = Math.round(bat.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 300) / 10f);
            int l = bat.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), e = bat.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
            if (l >= 0 && e > 0) nivel = Math.round(l * 100f / e);
        }
        // o bichinho começa a suar com "cpu" 70: no celular isso é perto de 41 °C
        int quente = Math.max(0, Math.min(100, (temp - 27) * 5));

        PowerManager pm = (PowerManager) ctx.getSystemService(Context.POWER_SERVICE);
        long agora = SystemClock.elapsedRealtime();
        if (pm.isInteractive()) telaApagouEm = 0;
        else if (telaApagouEm == 0) telaApagouEm = agora;
        int ocioso = telaApagouEm == 0 ? 0 : (int) ((agora - telaApagouEm) / 1000);

        String[] m = midia.tocando();
        StringBuilder s = new StringBuilder("3DSB");
        s.append("cpu=").append(quente).append('\n');
        s.append("ram=0\n");
        s.append("musica=").append(m[2]).append('\n');
        s.append("titulo=").append(SegundaTela.limpo(m[0], 40)).append('\n');
        s.append("ocioso=").append(ocioso).append('\n');
        s.append("janelas=").append(OuvinteNotificacoes.quantas()).append('\n');
        s.append("hora=").append(Calendar.getInstance().get(Calendar.HOUR_OF_DAY)).append('\n');
        s.append("skin=0\n");
        s.append("celular=1\ntemp=").append(temp).append("\nbateria=").append(nivel).append('\n');
        return s.toString().getBytes(StandardCharsets.UTF_8);
    }

    @Override public void run() {
        try {
            sock = new DatagramSocket(null);
            sock.setReuseAddress(true);
            sock.bind(new InetSocketAddress(PORTA));
            sock.setSoTimeout(1000);
            byte[] buf = new byte[64];
            while (!isInterrupted()) {
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                try {
                    sock.receive(p);
                } catch (SocketTimeoutException e) {
                    continue;
                }
                if (p.getLength() < 4 || buf[0] != '3' || buf[1] != 'D' || buf[2] != 'S' || buf[3] != 'B') continue;
                Estado.viu3ds(p.getAddress().getHostAddress());
                byte[] r = resposta();
                sock.send(new DatagramPacket(r, r.length, p.getSocketAddress()));
            }
        } catch (Exception e) {
            if (!isInterrupted()) android.util.Log.w("3dshub", "Bichinho parou", e);
        }
    }

    void parar() {
        interrupt();
        if (sock != null) sock.close();
    }
}
