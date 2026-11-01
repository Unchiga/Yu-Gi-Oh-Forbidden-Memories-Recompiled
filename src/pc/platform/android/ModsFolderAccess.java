package org.yfmredecomp.game;

import android.Manifest;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;

/**
 * The right to read the mods folder the player chose
 * (src/pc/platform/android.c, Mods folder...): "All files access" on Android 11
 * and later (MANAGE_EXTERNAL_STORAGE), the storage read permission before
 * that. Asked only when the player agrees to it in the Mods panel. On Android
 * 11 and later it is a switch on the system's settings page for the app, with
 * two other pages to try where a maker's settings lack it; the record says
 * "granted" or, when the player came back without it, "refused".
 */
public class ModsFolderAccess extends ModsFolderBase {
    private static final int SETTINGS = 2, RUNTIME = 3;
    private int attempt;

    /** Whether the game may read a folder of shared storage now. Called by android.c. */
    public static boolean has(Context context) {
        if (Build.VERSION.SDK_INT >= 30)
            return Environment.isExternalStorageManager();
        return context.checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
                == PackageManager.PERMISSION_GRANTED;
    }

    @Override
    void ask() {
        if (has(this)) {
            answer("chosen", "granted");
            return;
        }
        if (Build.VERSION.SDK_INT < 30) {
            asked = true;
            requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE}, RUNTIME);
            return;
        }
        next();
    }

    /** The settings page for the app, else the list of apps, else the app's details. */
    private void next() {
        Intent intent;
        if (attempt == 0)
            intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
        else if (attempt == 1)
            intent = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
        else
            intent = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:" + getPackageName()));
        attempt++;
        try {
            startActivityForResult(intent, SETTINGS);
            asked = true;
        } catch (ActivityNotFoundException | SecurityException e) {
            Log.i(TAG, "memories-pc: mods folder: settings page " + attempt + " not available: " + e);
            if (attempt < 3)
                next();
            else
                answer("failed", "This phone has no settings page for All files access.");
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != SETTINGS || isFinishing())
            return;
        Log.i(TAG, "memories-pc: mods folder: back from settings, access " + has(this));
        if (has(this))
            answer("chosen", "granted");
        else
            answer("cancelled", "refused");
    }

    @Override
    public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(request, permissions, results);
        if (request != RUNTIME || isFinishing())
            return;
        if (has(this))
            answer("chosen", "granted");
        else
            answer("cancelled", "refused");
    }
}
