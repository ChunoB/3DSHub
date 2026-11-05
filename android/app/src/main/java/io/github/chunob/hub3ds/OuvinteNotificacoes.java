package io.github.chunob.hub3ds;

import android.service.notification.NotificationListenerService;
import android.service.notification.StatusBarNotification;

/**
 * Existe para o Android liberar a leitura da música tocando (acesso às notificações).
 * Também conta as notificações, para o Bichinho ficar bravo quando tem muitas.
 */
public class OuvinteNotificacoes extends NotificationListenerService {
    private static volatile OuvinteNotificacoes ativo;

    @Override public void onListenerConnected() { ativo = this; }
    @Override public void onListenerDisconnected() { ativo = null; }

    /** Quantas notificações de outros apps estão abertas (só as que dá para limpar). */
    static int quantas() {
        OuvinteNotificacoes o = ativo;
        if (o == null) return 0;
        try {
            int n = 0;
            for (StatusBarNotification s : o.getActiveNotifications())
                if (s.isClearable() && !s.getPackageName().equals(o.getPackageName())) n++;
            return n;
        } catch (Exception e) {
            return 0;
        }
    }
}
