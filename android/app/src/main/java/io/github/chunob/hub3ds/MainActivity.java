package io.github.chunob.hub3ds;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.media.projection.MediaProjectionManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.ScrollView;
import android.widget.TextView;

import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** A única tela do app: mostra se o 3DS conectou e libera o som e a música. */
public class MainActivity extends Activity {
    private static final int FUNDO = 0xFF151823, CARTAO = 0xFF20253A, TEXTO = 0xFFE8EAF2, MUDO = 0xFF9AA0B5;
    private static final int AGUA = 0xFF1D9E75, AGUA_CL = 0xFF5DCAA5, AMARELO = 0xFFEF9F27, VERMELHO = 0xFFC81E28;
    private static final int PEDIR_PERMISSOES = 1, PEDIR_CAPTURA = 2;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private TextView ponto, estado, enderecos, somTexto, musicaTexto;
    private Button somBotao, musicaBotao;
    private Switch calar;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().setStatusBarColor(FUNDO);
        getWindow().setNavigationBarColor(FUNDO);

        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(18), dp(24), dp(18), dp(24));

        TextView titulo = texto("3DS Hub", 30, TEXTO, true);
        col.addView(titulo);
        col.addView(texto("no celular", 16, AGUA_CL, false));
        espaco(col, 18);

        // conexão
        LinearLayout c1 = cartao(col);
        LinearLayout linha = new LinearLayout(this);
        linha.setGravity(Gravity.CENTER_VERTICAL);
        ponto = texto("●", 22, AMARELO, true);
        estado = texto("", 20, TEXTO, true);
        estado.setPadding(dp(10), 0, 0, 0);
        linha.addView(ponto);
        linha.addView(estado);
        c1.addView(linha);
        enderecos = texto("", 14, MUDO, false);
        enderecos.setPadding(0, dp(6), 0, 0);
        c1.addView(enderecos);

        // som
        LinearLayout c2 = cartao(col);
        c2.addView(texto("Som do celular no 3DS", 18, TEXTO, true));
        somTexto = texto("", 14, MUDO, false);
        somTexto.setPadding(0, dp(4), 0, dp(10));
        c2.addView(somTexto);
        somBotao = botao("Ligar o som", v -> pedirSom());
        c2.addView(somBotao);
        calar = new Switch(this);
        calar.setText("Som só no 3DS (o celular fica mudo)");
        calar.setTextColor(TEXTO);
        calar.setTextSize(15);
        calar.setPadding(0, dp(12), 0, 0);
        Estado.querCalar = getPreferences(MODE_PRIVATE).getBoolean("calar", true);
        calar.setChecked(Estado.querCalar);
        calar.setOnCheckedChangeListener((chave, ligado) -> {
            Estado.querCalar = ligado;
            Estado.calarNaoFunciona = false;      // tenta de novo
            getPreferences(MODE_PRIVATE).edit().putBoolean("calar", ligado).apply();
        });
        c2.addView(calar);

        // música
        LinearLayout c3 = cartao(col);
        c3.addView(texto("Música e botões", 18, TEXTO, true));
        musicaTexto = texto("", 14, MUDO, false);
        musicaTexto.setPadding(0, dp(4), 0, dp(10));
        c3.addView(musicaTexto);
        musicaBotao = botao("Permitir ler a música", v ->
                startActivity(new Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)));
        c3.addView(musicaBotao);

        // como usar
        LinearLayout c4 = cartao(col);
        c4.addView(texto("Como usar", 18, TEXTO, true));
        TextView passos = texto(
                "1. Deixe o 3DS e o celular na mesma rede Wi-Fi. Na rua, ligue o roteador "
                + "(hotspot) do celular e conecte o 3DS nele.\n"
                + "2. Abra o 3DS Hub no 3DS: ele acha o celular sozinho.\n"
                + "3. No 3DS, abra a Segunda Tela: a música do celular aparece lá, com os botões, "
                + "e o som sai no 3DS (X liga e desliga).", 14, MUDO, false);
        passos.setPadding(0, dp(6), 0, 0);
        passos.setLineSpacing(dp(3), 1f);
        c4.addView(passos);

        espaco(col, 8);
        Button desligar = botao("Desligar o Hub", v -> {
            startService(new Intent(this, HubService.class).setAction(HubService.DESLIGAR));
            finish();
        });
        estiloBotao(desligar, CARTAO, MUDO);
        col.addView(desligar);

        ScrollView rolagem = new ScrollView(this);
        rolagem.setBackgroundColor(FUNDO);
        rolagem.addView(col);
        setContentView(rolagem);

        pedirPermissoesBasicas();
        HubService.ligar(this);
    }

    @Override protected void onResume() {
        super.onResume();
        handler.post(atualizar);
    }

    @Override protected void onPause() {
        super.onPause();
        handler.removeCallbacks(atualizar);
    }

    private final Runnable atualizar = new Runnable() {
        @Override public void run() {
            boolean c = Estado.conectado();
            ponto.setTextColor(c ? AGUA_CL : AMARELO);
            estado.setText(c ? "3DS conectado" : "Esperando o 3DS");
            enderecos.setText(textoEnderecos(c));

            if (!Estado.somLigado) {
                somTexto.setText("Toque em Ligar o som e permita. O Android mostra um aviso de "
                        + "\"gravar a tela\": é assim que ele chama a captura do som.");
                somBotao.setText("Ligar o som");
                somBotao.setEnabled(true);
                estiloBotao(somBotao, AGUA, Color.WHITE);
            } else {
                String s = !Estado.somPedido ? "Pronto. No 3DS, a Segunda Tela liga o som."
                        : Estado.somBloqueado ? "O app que está tocando não deixa mandar o som dele "
                                + "(o Spotify faz isso). Ele continua saindo no celular."
                        : Estado.calado ? "Mandando o som para o 3DS. O celular está mudo: o volume "
                                + "agora é o do 3DS."
                        : "Mandando o som para o 3DS.";
                if (Estado.calarNaoFunciona)
                    s += "\nNeste celular, calar o celular calava o 3DS também, então o som sai nos dois.";
                if (!Estado.erroSom.isEmpty()) s += "\nErro: " + Estado.erroSom;
                somTexto.setText(s);
                somBotao.setText("Som liberado");
                somBotao.setEnabled(false);
                estiloBotao(somBotao, CARTAO, AGUA_CL);
            }

            if (Midia.temAcesso(MainActivity.this)) {
                musicaTexto.setText("Liberado: o 3DS mostra o nome da música e controla o player.");
                musicaBotao.setVisibility(View.GONE);
            } else {
                musicaTexto.setText("Para o 3DS mostrar o nome da música, libere o 3DS Hub no "
                        + "\"acesso às notificações\". Sem isso, os botões funcionam mesmo assim.");
                musicaBotao.setVisibility(View.VISIBLE);
                estiloBotao(musicaBotao, AGUA, Color.WHITE);
            }
            handler.postDelayed(this, 500);
        }
    };

    private String textoEnderecos(boolean conectado) {
        List<String> ips = new ArrayList<>();
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                String nome = ni.getName();
                if (nome.startsWith("rmnet") || nome.startsWith("ccmni") || nome.startsWith("dummy")) continue;   // dados móveis
                for (InetAddress a : Collections.list(ni.getInetAddresses()))
                    if (a instanceof Inet4Address) ips.add(a.getHostAddress());
            }
        } catch (Exception ignorado) {}
        String linha = ips.isEmpty() ? "Sem Wi-Fi: conecte o celular a uma rede ou ligue o roteador dele."
                                     : "Celular: " + String.join(", ", ips);
        if (conectado && !Estado.ip3ds.isEmpty()) linha += "\n3DS: " + Estado.ip3ds;
        return linha + "\nNome que aparece no 3DS: " + Descoberta.nome();
    }

    // ------------------------------------------------------------ permissões
    private void pedirPermissoesBasicas() {
        List<String> faltam = new ArrayList<>();
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED)
            faltam.add(Manifest.permission.RECORD_AUDIO);
        if (android.os.Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            faltam.add(Manifest.permission.POST_NOTIFICATIONS);
        if (!faltam.isEmpty()) requestPermissions(faltam.toArray(new String[0]), PEDIR_PERMISSOES);
    }

    private void pedirSom() {
        // a captura do som dos apps exige a permissão do microfone (ele mesmo não é usado)
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] { Manifest.permission.RECORD_AUDIO }, PEDIR_PERMISSOES);
            return;
        }
        MediaProjectionManager mpm = getSystemService(MediaProjectionManager.class);
        startActivityForResult(mpm.createScreenCaptureIntent(), PEDIR_CAPTURA);
    }

    @Override protected void onActivityResult(int pedido, int resultado, Intent dados) {
        super.onActivityResult(pedido, resultado, dados);
        if (pedido == PEDIR_CAPTURA && resultado == RESULT_OK && dados != null) {
            startForegroundService(new Intent(this, HubService.class)
                    .setAction(HubService.LIGAR_SOM)
                    .putExtra(HubService.EXTRA_CODIGO, resultado)
                    .putExtra(HubService.EXTRA_DADOS, dados));
        }
    }

    // ------------------------------------------------------------ visual
    private int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    private TextView texto(String s, float sp, int cor, boolean negrito) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(cor);
        if (negrito) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private void espaco(LinearLayout col, int alt) {
        View v = new View(this);
        col.addView(v, new LinearLayout.LayoutParams(1, dp(alt)));
    }

    private LinearLayout cartao(LinearLayout col) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(16), dp(14), dp(16), dp(16));
        GradientDrawable fundo = new GradientDrawable();
        fundo.setColor(CARTAO);
        fundo.setCornerRadius(dp(16));
        c.setBackground(fundo);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(12);
        col.addView(c, lp);
        return c;
    }

    private Button botao(String s, View.OnClickListener acao) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextSize(16);
        b.setOnClickListener(acao);
        estiloBotao(b, AGUA, Color.WHITE);
        return b;
    }

    private void estiloBotao(Button b, int fundo, int texto) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fundo);
        d.setCornerRadius(dp(12));
        b.setBackground(d);
        b.setTextColor(texto);
        b.setStateListAnimator(null);
        b.setPadding(dp(12), dp(12), dp(12), dp(12));
    }
}
