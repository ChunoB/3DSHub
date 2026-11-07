package io.github.chunob.hub3ds;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.media.projection.MediaProjectionManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;

import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** A tela do app: se o 3DS conectou, as permissões de cada parte e atalhos. */
public class MainActivity extends Activity {
    private static final int FUNDO = 0xFF151823, CARTAO = 0xFF20253A, CARTAO2 = 0xFF2A3048, TEXTO = 0xFFE8EAF2, MUDO = 0xFF9AA0B5;
    private static final int AGUA = 0xFF1D9E75, AGUA_CL = 0xFF5DCAA5, AMARELO = 0xFFEF9F27;
    private static final int PEDIR_PERMISSOES = 1, PEDIR_CAPTURA = 2;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private TextView ponto, estado, enderecos, somTexto;
    private Switch calar;

    /** Uma permissão na lista: título, para que serve, botão. */
    private final class Permissao {
        final TextView marca, explica;
        final Button botao;
        Permissao(LinearLayout cartao, String titulo, String paraQue, String textoBotao, View.OnClickListener acao) {
            LinearLayout linha = new LinearLayout(MainActivity.this);
            linha.setGravity(Gravity.CENTER_VERTICAL);
            marca = texto("○", 18, MUDO, true);
            TextView t = texto(titulo, 16, TEXTO, true);
            t.setPadding(dp(8), 0, 0, 0);
            linha.addView(marca);
            linha.addView(t);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.topMargin = dp(12);
            cartao.addView(linha, lp);
            explica = texto(paraQue, 13, MUDO, false);
            explica.setPadding(dp(26), dp(2), 0, dp(6));
            cartao.addView(explica);
            botao = botao(textoBotao, acao);
            LinearLayout.LayoutParams lb = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            lb.leftMargin = dp(26);
            cartao.addView(botao, lb);
        }
        void mostrar(boolean ok) {
            marca.setText(ok ? "●" : "○");
            marca.setTextColor(ok ? AGUA_CL : AMARELO);
            botao.setVisibility(ok ? View.GONE : View.VISIBLE);
        }
    }

    private Permissao pSom, pMusica, pToques, pAbrir;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().setStatusBarColor(FUNDO);
        getWindow().setNavigationBarColor(FUNDO);

        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(18), dp(24), dp(18), dp(24));
        col.addView(texto("3DS Hub", 30, TEXTO, true));
        col.addView(texto("no celular", 16, AGUA_CL, false));
        espaco(col, 18);

        // o app fechou por um erro da última vez: mostra o motivo para mandar
        java.io.File erro = App.arquivoErro(this);
        if (erro.exists()) {
            String texto;
            try { texto = new String(java.nio.file.Files.readAllBytes(erro.toPath()), java.nio.charset.StandardCharsets.UTF_8); }
            catch (Exception e) { texto = e.toString(); }
            final String completo = texto;
            LinearLayout ce = cartao(col);
            ce.addView(texto("O app fechou por um erro", 18, 0xFFE24B4A, true));
            TextView det = texto(texto.length() > 600 ? texto.substring(0, 600) + "..." : texto, 11, MUDO, false);
            det.setTypeface(Typeface.MONOSPACE);
            det.setPadding(0, dp(6), 0, dp(8));
            ce.addView(det);
            LinearLayout bs = new LinearLayout(this);
            Button copiar = botao("Copiar o erro", v -> {
                ((android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE))
                        .setPrimaryClip(android.content.ClipData.newPlainText("erro do 3DS Hub", completo));
                android.widget.Toast.makeText(this, "Copiado", android.widget.Toast.LENGTH_SHORT).show();
            });
            Button limpar = botao("Apagar", v -> { erro.delete(); ce.setVisibility(View.GONE); });
            estiloBotao(limpar, CARTAO2, TEXTO);
            LinearLayout.LayoutParams m1 = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
            LinearLayout.LayoutParams m2 = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
            m2.leftMargin = dp(8);
            bs.addView(copiar, m1);
            bs.addView(limpar, m2);
            ce.addView(bs);
        }

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

        // permissões
        LinearLayout c2 = cartao(col);
        c2.addView(texto("Permissões", 18, TEXTO, true));
        c2.addView(texto("Cada uma libera uma parte. Pode liberar só o que for usar.", 13, MUDO, false));
        pSom = new Permissao(c2, "Som e tela",
                "Som do celular no 3DS, Espelho e as notas do Karaokê. O Android chama de \"gravar a tela\". "
                + "Precisa permitir de novo cada vez que o celular reinicia.",
                "Ligar som e tela", v -> pedirSom());
        pMusica = new Permissao(c2, "Ler a música",
                "O nome da música na Segunda Tela, no Bichinho e no Karaokê, e as notificações do Bichinho.",
                "Liberar", v -> startActivity(new Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)));
        pToques = new Permissao(c2, "Toques do 3DS",
                "Tocar e digitar no celular pelo Espelho, e os botões do Controle. Em Acessibilidade, "
                + "ative \"3DS Hub (toques do 3DS)\".",
                "Liberar", v -> startActivity(new Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS)));
        pAbrir = new Permissao(c2, "Abrir apps sozinho",
                "Buscar música pelo 3DS (abre o YouTube Music) e abrir o lago da Pescaria quando alguém entra.",
                "Liberar", v -> startActivity(new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                        Uri.parse("package:" + getPackageName()))));
        TextView restrita = texto("Se o Android disser \"configuração restrita\": Configurações > Apps > 3DS Hub > "
                + "três pontinhos > Permitir configurações restritas.", 12, MUDO, false);
        restrita.setPadding(0, dp(12), 0, 0);
        c2.addView(restrita);

        // som
        LinearLayout c3 = cartao(col);
        c3.addView(texto("Som no 3DS", 18, TEXTO, true));
        somTexto = texto("", 14, MUDO, false);
        somTexto.setPadding(0, dp(4), 0, 0);
        c3.addView(somTexto);
        calar = new Switch(this);
        calar.setText("Som só no 3DS (o celular fica mudo)");
        calar.setTextColor(TEXTO);
        calar.setTextSize(15);
        calar.setPadding(0, dp(12), 0, 0);
        Estado.querCalar = getPreferences(MODE_PRIVATE).getBoolean("calar", true);
        calar.setChecked(Estado.querCalar);
        calar.setOnCheckedChangeListener((chave, ligado) -> {
            Estado.querCalar = ligado;
            Estado.calarNaoFunciona = false;
            Estado.baixoNaoFunciona = false;
            getPreferences(MODE_PRIVATE).edit().putBoolean("calar", ligado).apply();
        });
        c3.addView(calar);

        // atalhos
        LinearLayout c4 = cartao(col);
        c4.addView(texto("Abrir aqui", 18, TEXTO, true));
        LinearLayout grade = new LinearLayout(this);
        grade.setPadding(0, dp(8), 0, 0);
        Button lago = botao("Lago da Pescaria", v -> startActivity(new Intent(this, PescariaActivity.class)));
        Button cam = botao("Câmera do 3DS", v -> startActivity(new Intent(this, CameraActivity.class)));
        estiloBotao(lago, CARTAO2, TEXTO);
        estiloBotao(cam, CARTAO2, TEXTO);
        LinearLayout.LayoutParams metade = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        LinearLayout.LayoutParams metade2 = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        metade2.leftMargin = dp(8);
        grade.addView(lago, metade);
        grade.addView(cam, metade2);
        c4.addView(grade);
        TextView dicaControle = texto("Controle: abra o jogo e toque em \"Botões do controle\" na notificação do "
                + "3DS Hub para marcar onde cada botão do 3DS toca.", 13, MUDO, false);
        dicaControle.setPadding(0, dp(10), 0, 0);
        c4.addView(dicaControle);

        // como usar
        LinearLayout c5 = cartao(col);
        c5.addView(texto("Como usar", 18, TEXTO, true));
        TextView passos = texto(
                "1. Deixe o 3DS e o celular na mesma rede Wi-Fi. Na rua, ligue o roteador "
                + "(hotspot) do celular e conecte o 3DS nele.\n"
                + "2. Abra o 3DS Hub no 3DS: ele acha o celular sozinho (SELECT escolhe outro aparelho).\n"
                + "3. Segunda Tela: música do celular, som no 3DS (X) e busca de música (Y).\n"
                + "4. Funcionam com o celular todos os módulos, menos a Steam.", 14, MUDO, false);
        passos.setPadding(0, dp(6), 0, 0);
        passos.setLineSpacing(dp(3), 1f);
        c5.addView(passos);

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

            pSom.mostrar(Estado.somLigado);
            pMusica.mostrar(Midia.temAcesso(MainActivity.this));
            pToques.mostrar(Toque.liberado(MainActivity.this));
            pAbrir.mostrar(Settings.canDrawOverlays(MainActivity.this));

            String s;
            if (!Estado.somLigado) s = "Libere \"Som e tela\" acima. Depois, no 3DS, a Segunda Tela liga o som (X).";
            else if (!Estado.somPedido) s = "Pronto. No 3DS, a Segunda Tela liga o som.";
            else if (Estado.somBloqueado) s = "O app que está tocando não deixa mandar o som dele (o Spotify faz isso). Ele continua no celular.";
            else if (Estado.calado) s = "Mandando o som para o 3DS. O celular está mudo: o volume agora é o do 3DS.";
            else s = "Mandando o som para o 3DS.";
            if (Estado.baixoNaoFunciona) s += "\nNeste celular não deu para calar sem calar o 3DS junto, então o som sai nos dois.";
            else if (Estado.modoBaixo) s += "\nNeste celular o mudo calava o 3DS junto: o celular fica no volume mínimo.";
            if (!Estado.erroSom.isEmpty()) s += "\nErro: " + Estado.erroSom;
            somTexto.setText(s);
            handler.postDelayed(this, 500);
        }
    };

    private String textoEnderecos(boolean conectado) {
        List<String> ips = new ArrayList<>();
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                String nome = ni.getName();
                if (nome.startsWith("rmnet") || nome.startsWith("ccmni") || nome.startsWith("dummy")) continue;
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
        col.addView(new View(this), new LinearLayout.LayoutParams(1, dp(alt)));
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
        b.setTextSize(15);
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
        b.setPadding(dp(12), dp(10), dp(12), dp(10));
    }
}
