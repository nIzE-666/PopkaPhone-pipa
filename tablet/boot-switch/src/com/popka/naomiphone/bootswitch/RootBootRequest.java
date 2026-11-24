package com.popka.naomiphone.bootswitch;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

final class RootBootRequest {
    private static final String REBOOT_REQUESTED = "UBUNTU_TOUCH_REBOOT_REQUESTED";
    private static final int REPORT_LIMIT = 512;

    static void reboot(InputStream script, String sourceApk) throws IOException, InterruptedException {
        String command = "PATH=/system/bin:/system/xbin:/system_ext/bin:/product/bin:/vendor/bin "
                + "NAOMI_BOOT_SWITCH_APK='" + sourceApk.replace("'", "'\\''") + "' "
                + "/system/bin/sh -s -- ubuntu-touch";
        Process shell;
        try {
            shell = new ProcessBuilder("su", "-c", command).redirectErrorStream(true).start();
        } catch (IOException error) {
            throw new IOException("Для переключения нужен root-доступ через su.", error);
        }
        run(shell, script);
    }

    static void run(Process shell, InputStream script) throws IOException, InterruptedException {
        IOException sendError = null;
        try (OutputStream input = shell.getOutputStream()) {
            byte[] buffer = new byte[4096];
            for (int n; (n = script.read(buffer)) != -1; ) {
                input.write(buffer, 0, n);
            }
        } catch (IOException error) {
            // При отказе в root канал ввода может закрыться раньше, чем придёт сообщение su.
            sendError = error;
        }

        boolean requested = false;
        StringBuilder report = new StringBuilder();
        try (BufferedReader output =
                new BufferedReader(new InputStreamReader(shell.getInputStream(), StandardCharsets.UTF_8))) {
            for (String line; (line = output.readLine()) != null; ) {
                if (line.trim().equals(REBOOT_REQUESTED)) {
                    requested = true;
                } else if (report.length() < REPORT_LIMIT) {
                    report.append(line, 0, Math.min(line.length(), REPORT_LIMIT - report.length())).append('\n');
                }
            }
        } finally {
            // Дожидаемся завершения операции выбора слота даже при закрытии канала вывода.
            shell.waitFor();
        }
        if (shell.exitValue() == 0 && requested) {
            return;
        }
        String detail = report.toString().trim();
        if (!detail.isEmpty()) {
            throw new IOException(detail);
        }
        if (sendError != null) {
            throw new IOException("Не удалось передать команду в su. Проверьте разрешение root-доступа.", sendError);
        }
        throw new IOException("Помощник не подтвердил запрос перезагрузки. Код " + shell.exitValue() + ".");
    }
}
