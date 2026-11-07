package io.github.chunob.hub3ds;

import android.app.Application;

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Se o app fechar por um erro, guarda o motivo (erro.txt). Na próxima vez, a tela
 * do app mostra o erro com um botão de copiar, para ser fácil de mandar e corrigir.
 */
public class App extends Application {
    static File arquivoErro(android.content.Context c) { return new File(c.getFilesDir(), "erro.txt"); }

    @Override public void onCreate() {
        super.onCreate();
        Thread.UncaughtExceptionHandler padrao = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((t, e) -> {
            try (FileWriter w = new FileWriter(arquivoErro(this))) {
                StringWriter s = new StringWriter();
                e.printStackTrace(new PrintWriter(s));
                w.write(new SimpleDateFormat("dd/MM HH:mm", Locale.ROOT).format(new Date())
                        + " · versão " + BuildConfig.VERSION_NAME + " · thread " + t.getName() + "\n" + s);
            } catch (Exception ignorado) {}
            if (padrao != null) padrao.uncaughtException(t, e);
        });
    }
}
