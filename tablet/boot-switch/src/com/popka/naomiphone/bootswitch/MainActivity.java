package com.popka.naomiphone.bootswitch;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

import java.io.IOException;
import java.io.InputStream;
import java.util.LinkedHashSet;
import java.util.Set;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;

public final class MainActivity extends Activity {
    private enum Phase { CONFIRM, SELECTING, RESTARTING, FAILED }

    private static final Executor WORKER = Executors.newSingleThreadExecutor();
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static final Set<Runnable> VISIBLE = new LinkedHashSet<>();
    private static Phase phase = Phase.CONFIRM;
    private static String failure = "";

    private final Runnable refresh = this::refreshDialog;
    private AlertDialog dialog;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        dialog = new AlertDialog.Builder(this)
                .setTitle(R.string.title)
                .setMessage(R.string.confirm)
                .setNegativeButton(android.R.string.cancel, (ignored, button) -> finish())
                .setPositiveButton(R.string.reboot, null)
                .create();
        dialog.setOnCancelListener(ignored -> finish());
        dialog.show();
        dialog.getButton(DialogInterface.BUTTON_POSITIVE).setFilterTouchesWhenObscured(true);
        dialog.getButton(DialogInterface.BUTTON_POSITIVE).setOnClickListener(ignored -> startSwitch());
        refreshDialog();
    }

    @Override
    protected void onStart() {
        super.onStart();
        VISIBLE.add(refresh);
        refreshDialog();
    }

    @Override
    protected void onStop() {
        VISIBLE.remove(refresh);
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        dialog.dismiss();
        super.onDestroy();
    }

    private void startSwitch() {
        if (phase == Phase.SELECTING || phase == Phase.RESTARTING) {
            return;
        }
        phase = Phase.SELECTING;
        notifyVisible();
        Context app = getApplicationContext();
        WORKER.execute(() -> {
            String issue = null;
            try (InputStream helper = app.getAssets().open("naomi-pipa-slot-switch")) {
                RootBootRequest.reboot(helper, app.getApplicationInfo().sourceDir);
            } catch (IOException error) {
                issue = error.getMessage() == null ? "" : error.getMessage();
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                issue = "";
            }
            String result = issue;
            MAIN.post(() -> {
                phase = result == null ? Phase.RESTARTING : Phase.FAILED;
                failure = result;
                notifyVisible();
            });
        });
    }

    private static void notifyVisible() {
        VISIBLE.forEach(Runnable::run);
    }

    private void refreshDialog() {
        boolean canConfirm = phase == Phase.CONFIRM || phase == Phase.FAILED;
        dialog.setCancelable(canConfirm);
        dialog.setCanceledOnTouchOutside(false);
        dialog.getButton(DialogInterface.BUTTON_NEGATIVE).setEnabled(canConfirm);
        dialog.getButton(DialogInterface.BUTTON_POSITIVE).setEnabled(canConfirm);
        switch (phase) {
            case SELECTING:
                dialog.setMessage(getString(R.string.selecting));
                break;
            case RESTARTING:
                dialog.setMessage(getString(R.string.restarting));
                break;
            case FAILED:
                String heading = getString(R.string.failed);
                dialog.setMessage(failure.isEmpty() ? heading : heading + "\n\n" + failure);
                break;
            default:
                dialog.setMessage(getString(R.string.confirm));
                break;
        }
    }
}
