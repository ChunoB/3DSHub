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
    static final String CANAL = "hub";
    private static final int NOTIFICACAO = 1;

    private Descoberta descoberta;
    private SegundaTela tela;
    private Bicho bicho;
    private Espelho espelho;
    private Camera camera;
    private Pescaria pescaria;
    private Karaoke karaoke;
    private Controle controle;
    private volatile CapturaTela captura;
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
        bicho = new Bicho(this);
        bicho.start();
        espelho = new Espelho(this);
        espelho.start();
        camera = new Camera(this);
        camera.start();
        pescaria = new Pescaria(this);
        pescaria.start();
        karaoke = new Karaoke(this);
        karaoke.start();
        controle = new Controle(this);
        controle.start();
        Estado.servicoRodando = true;
        handler.postDelayed(this::vigiar, 1000);
    }

    /** A imagem da tela (para o Espelho); null antes de permitir no app. */
    CapturaTela captura() { return captura; }

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
                .addAction(new Notification.Action.Builder(null, "Botões do controle", PendingIntent.getActivity(this, 4,
                        new Intent(this, ControleConfigActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                        PendingIntent.FLAG_IMMUTABLE)).build())
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
                if (captura != null) captura.encerrar();
                captura = null;
                projecao = null;
                handler.post(() -> primeiroPlano(false));
            }
        }, handler);
        if (captura != null) captura.encerrar();
        captura = new CapturaTela(this, projecao);
        try {
            som = new Som(projecao, getSystemService(android.media.AudioManager.class));
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
        if (bicho != null) bicho.parar();
        if (espelho != null) espelho.parar();
        if (camera != null) camera.parar();
        if (pescaria != null) pescaria.parar();
        if (karaoke != null) karaoke.parar();
        if (controle != null) controle.parar();
        if (captura != null) captura.encerrar();
        if (trava != null) trava.release();
        Estado.servicoRodando = false;
        Estado.somLigado = false;
        super.onDestroy();
    }
}
