package io.github.chunob.hub3ds;

import android.os.Build;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;

/**
 * O 3DS pergunta "3DSD" para a rede inteira (UDP 5009) e o celular responde com o
 * nome, igual ao PC (pc/descoberta.py). O nome começa com "Celular": é assim que o
 * 3DS sabe que do outro lado está um celular e não um PC.
 */
final class Descoberta extends Thread {
    static final int PORTA = 5009;
    private DatagramSocket sock;

    Descoberta() { super("descoberta"); setDaemon(true); }

    static String nome() {
        String m = Build.MODEL == null ? "" : Build.MODEL.trim();
        String n = "Celular " + m;
        return n.length() > 30 ? n.substring(0, 30) : n;
    }

    @Override public void run() {
        try {
            sock = new DatagramSocket(null);
            sock.setReuseAddress(true);
            sock.setBroadcast(true);
            sock.bind(new InetSocketAddress(PORTA));
            byte[] buf = new byte[64];
            byte[] resposta = ("3DSD" + nome()).getBytes(StandardCharsets.UTF_8);
            while (!isInterrupted()) {
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                sock.receive(p);
                // só a pergunta (4 bytes): respostas de outros aparelhos também começam com 3DSD
                if (p.getLength() == 4 && buf[0] == '3' && buf[1] == 'D' && buf[2] == 'S' && buf[3] == 'D') {
                    sock.send(new DatagramPacket(resposta, resposta.length, p.getSocketAddress()));
                }
            }
        } catch (Exception e) {
            if (!isInterrupted()) android.util.Log.w("3dshub", "Descoberta parou", e);
        }
    }

    void parar() {
        interrupt();
        if (sock != null) sock.close();
    }
}
