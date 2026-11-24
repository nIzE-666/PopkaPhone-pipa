package com.popka.naomiphone.bootswitch;

import android.app.ActivityOptions;
import android.app.PendingIntent;
import android.content.Intent;
import android.os.Build;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

public final class UbuntuTile extends TileService {
    @Override
    public void onStartListening() {
        Tile tile = getQsTile();
        if (tile != null) {
            tile.setState(Tile.STATE_INACTIVE);
            tile.updateTile();
        }
    }

    @Override
    public void onClick() {
        if (isLocked()) {
            unlockAndRun(this::showConfirmation);
        } else {
            showConfirmation();
        }
    }

    @SuppressWarnings("deprecation")
    private void showConfirmation() {
        Intent intent = new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        ActivityOptions options = ActivityOptions.makeBasic();
        int mode = Build.VERSION.SDK_INT >= 36 ? ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOW_ALWAYS
                : ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOWED;
        options.setPendingIntentCreatorBackgroundActivityStartMode(mode);
        startActivityAndCollapse(PendingIntent.getActivity(
                this, 0, intent, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_ONE_SHOT, options.toBundle()));
    }
}
