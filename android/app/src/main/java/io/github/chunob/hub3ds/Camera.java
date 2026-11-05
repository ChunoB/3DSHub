package io.github.chunob.hub3ds;

import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Intent;

import java.io.DataInputStream;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;

/**
 * Câmera (TCP 5007), o mesmo protocolo do PC (pc/camera.py): o 3DS manda fotos JPEG
 * sem parar, cada uma com o cabeçalho "3DSF" + u8 olho (0 esq, 1 dir) + u8 total de
 * olhos + u16 reservado + u32 tamanho. No celular elas aparecem ao vivo numa tela do
 * app (CameraActivity), que também salva a foto na galeria.
 */
final class Camera extends Thread {
    static final int PORTA = 5007;
    static final int NOTIFICACAO = 2;
    static volatile byte[] esquerdo, direito;
    static volatile int olhos = 1;
    static volatile long numero;       // muda a cada foto nova
    static volatile boolean conectado;

    private final HubService hub;
    private ServerSocket servidor;

    Camera(HubService hub) {
        super("camera");
        setDaemon(true);
        this.hub = hub;
    }

    @Override public void run() {
        try {
            servidor = new ServerSocket();
            servidor.setReuseAddress(true);
            servidor.bind(new InetSocketAddress(PORTA));
            while (!isInterrupted()) {
                try (Socket s = servidor.accept()) {
                    s.setSoTimeout(10000);
                    conectado = true;
                    Estado.viu3ds(s.getInetAddress().getHostAddress());
                    avisar();
                    receber(new DataInputStream(s.getInputStream()));
                } catch (Exception e) {
                    if (isInterrupted()) return;
                } finally {
                    conectado = false;
                    hub.getSystemService(NotificationManager.class).cancel(NOTIFICACAO);
                }
            }
        } catch (Exception e) {
            if (!isInterrupted()) android.util.Log.w("3dshub", "Câmera parou", e);
        }
    }

    private static int u32(byte[] b, int i) {
        return (b[i] & 0xFF) | (b[i + 1] & 0xFF) << 8 | (b[i + 2] & 0xFF) << 16 | (b[i + 3] & 0xFF) << 24;
    }

    private void receber(DataInputStream in) throws Exception {
        byte[] cab = new byte[12];
        while (!isInterrupted()) {
            in.readFully(cab);
            if (cab[0] != '3' || cab[1] != 'D' || cab[2] != 'S' || cab[3] != 'F') return;
            int olho = cab[4], total = Math.max(1, cab[5]), tam = u32(cab, 8);
            if (tam <= 0 || tam > 2_000_000) return;
            byte[] jpeg = new byte[tam];
            in.readFully(jpeg);
            olhos = total;
            if (olho == 1) direito = jpeg;
            else esquerdo = jpeg;
            numero++;
        }
    }

    /** Notificação "toque para ver", se a tela da câmera não estiver aberta. */
    private void avisar() {
        if (CameraActivity.aberta) return;
        PendingIntent abrir = PendingIntent.getActivity(hub, 2, new Intent(hub, CameraActivity.class),
                PendingIntent.FLAG_IMMUTABLE);
        hub.getSystemService(NotificationManager.class).notify(NOTIFICACAO,
                new android.app.Notification.Builder(hub, HubService.CANAL)
                        .setSmallIcon(R.drawable.ic_notificacao)
                        .setContentTitle("Câmera do 3DS")
                        .setContentText("Toque para ver a câmera do 3DS aqui")
                        .setContentIntent(abrir)
                        .setAutoCancel(true)
                        .build());
    }

    void parar() {
        interrupt();
        try { if (servidor != null) servidor.close(); } catch (Exception ignorado) {}
    }
}
