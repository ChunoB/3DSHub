package io.github.chunob.hub3ds;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.net.wifi.WifiManager;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;

/**
 * Fica rodando com uma notificação fixa enquanto o 3DS Hub do celular está ligado.
 * Responde ao 3DS nas mesmas portas do PC: descoberta (5009), Segunda Tela (5006)
 * e, depois que você permite, o som (5013).
 */
public class HubService extends Service {
    static final String LIGAR_SOM = "ligar_som", DESLIGAR = "desligar";
    static final String EXTRA_CODIGO = "codigo", EXTRA_DADOS = "dados";
    private static final String CANAL = "hub";
    private static final int NOTIFICACAO = 1;

    private Descoberta descoberta;
    private SegundaTela tela;
    private Som som;
    private MediaProjection projecao;
    private WifiManager.MulticastLock trava;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private boolean ultimoConectado;

    static void ligar(Context ctx) {
        ctx.startForegroundService(new Intent(ctx, HubService.class));
    }

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public void onCreate() {
        super.onCreate();
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CANAL, "3DS Hub ligado", NotificationManager.IMPORTANCE_LOW));
        primeiroPlano(false);

        // sem isto, alguns celulares jogam fora o "broadcast" do 3DS procurando o Hub
        WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        trava = wifi.createMulticastLock("3dshub");
        trava.setReferenceCounted(false);
        trava.acquire();

        descoberta = new Descoberta();
        descoberta.start();
        tela = new SegundaTela(this);
        tela.start();
        Estado.servicoRodando = true;
        handler.postDelayed(this::vigiar, 1000);
    }

    private void primeiroPlano(boolean comSom) {
        int tipo = ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE;
        if (comSom) tipo |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION;
        startForeground(NOTIFICACAO, notificacao(), tipo);
    }

    private Notification notificacao() {
        PendingIntent abrir = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
                PendingIntent.FLAG_IMMUTABLE);
        PendingIntent desligar = PendingIntent.getService(this, 1,
                new Intent(this, HubService.class).setAction(DESLIGAR), PendingIntent.FLAG_IMMUTABLE);
        String texto = Estado.conectado() ? "3DS conectado" + (Estado.somPedido ? " · som no 3DS" : "")
                                          : "Esperando o 3DS na rede";
        return new Notification.Builder(this, CANAL)
                .setSmallIcon(R.drawable.ic_notificacao)
                .setContentTitle("3DS Hub")
                .setContentText(texto)
                .setContentIntent(abrir)
                .setOngoing(true)
                .addAction(new Notification.Action.Builder(null, "Desligar", desligar).build())
                .build();
    }

    /** Atualiza a notificação quando o 3DS conecta ou sai. */
    private void vigiar() {
        boolean c = Estado.conectado();
        if (c != ultimoConectado) {
            ultimoConectado = c;
            getSystemService(NotificationManager.class).notify(NOTIFICACAO, notificacao());
        }
        handler.postDelayed(this::vigiar, 1000);
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        String acao = intent == null ? null : intent.getAction();
        if (DESLIGAR.equals(acao)) {
            stopSelf();
        } else if (LIGAR_SOM.equals(acao)) {
            ligarSom(intent.getIntExtra(EXTRA_CODIGO, 0), intent.getParcelableExtra(EXTRA_DADOS));
        }
        return START_STICKY;
    }

    private void ligarSom(int codigo, Intent dados) {
        if (dados == null) return;
        if (som != null) som.parar();
        if (projecao != null) projecao.stop();
        // no Android 14 a permissão precisa estar no primeiro plano ANTES de pegar a captura
        primeiroPlano(true);
        MediaProjectionManager mpm = getSystemService(MediaProjectionManager.class);
        projecao = mpm.getMediaProjection(codigo, dados);
        if (projecao == null) return;
        projecao.registerCallback(new MediaProjection.Callback() {
            @Override public void onStop() {
                if (som != null) som.parar();
                som = null;
                projecao = null;
                handler.post(() -> primeiroPlano(false));
            }
        }, handler);
        try {
            som = new Som(projecao);
            som.iniciar();
        } catch (Exception e) {
            Estado.erroSom = e.toString();
            som = null;
        }
    }

    @Override public void onDestroy() {
        handler.removeCallbacksAndMessages(null);
        if (som != null) som.parar();
        if (projecao != null) projecao.stop();
        if (descoberta != null) descoberta.parar();
        if (tela != null) tela.parar();
        if (trava != null) trava.release();
        Estado.servicoRodando = false;
        Estado.somLigado = false;
        super.onDestroy();
    }
}
