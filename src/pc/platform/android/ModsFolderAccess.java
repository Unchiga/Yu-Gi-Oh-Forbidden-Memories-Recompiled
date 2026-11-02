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
 * a second page to try where a maker's settings lack the first; a device with
 * neither (an Android TV, some Go editions) cannot give the right at all, and
 * the record says "failed" with a sentence that sends the player to Import mod.
 * Otherwise it says "granted" or, when the player came back without it,
 * "refused".
 */
public class ModsFolderAccess extends ModsFolderBase {
    private static final int SETTINGS = 2, RUNTIME = 3;
    private int attempt;

    /** The pages that hold the All files access switch, the app's own first. */
    private static Intent[] pages(Context context) {
        return new Intent[] {
            new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + context.getPackageName())),
            new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)
        };
    }

    /** Whether this device has a page to give the right on (or asks for it by a prompt, before Android 11). */
    public static boolean canAsk(Context context) {
        if (Build.VERSION.SDK_INT < 30)
            return true;
        for (Intent page : pages(context))
            if (page.resolveActivity(context.getPackageManager()) != null)
                return true;
        return false;
    }

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

    /**
     * The settings page for the app, else the list of apps. The app's details page is not tried: it
     * opens everywhere and has no switch to find, so a phone without the two would only loop.
     */
    private void next() {
        Intent[] pages = pages(this);
        while (attempt < pages.length) {
            Intent intent = pages[attempt++];
            if (intent.resolveActivity(getPackageManager()) == null) {
                Log.i(TAG, "memories-pc: mods folder: settings page " + attempt + " not on this device");
                continue;
            }
            try {
                startActivityForResult(intent, SETTINGS);
                asked = true;
                return;
            } catch (ActivityNotFoundException | SecurityException e) {
                Log.i(TAG, "memories-pc: mods folder: settings page " + attempt + " not available: " + e);
            }
        }
        answer("failed", "This device cannot give the game that right. Use Import mod... instead.");
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
