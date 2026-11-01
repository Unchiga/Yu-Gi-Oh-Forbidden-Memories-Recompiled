package org.yfmredecomp.game;

import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.util.Log;
import java.io.File;
import java.io.IOException;

/**
 * The Mods panel's Mods folder... on Android (src/pc/platform/android.c):
 * the system's folder picker (ACTION_OPEN_DOCUMENT_TREE), whose answer is
 * turned into the folder's path, since the game reads the folder with plain
 * file calls once it holds "All files access" ({@link ModsFolderAccess}).
 * Only a folder of the phone's storage or an SD card has a path: the
 * external storage provider's. The record's text is that path.
 */
public class ModsFolderPicker extends ModsFolderBase {
    private static final int PICK = 1;
    private static final String STORAGE = "com.android.externalstorage.documents";

    @Override
    void ask() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        // Opens in Download, where a pack of mods is most likely: the player
        // chooses a folder in it (Android 11 and later will not hand back
        // Download itself, nor the storage root).
        intent.putExtra(DocumentsContract.EXTRA_INITIAL_URI,
                Uri.parse("content://" + STORAGE + "/document/primary%3ADownload"));
        try {
            startActivityForResult(intent, PICK);
            asked = true;
        } catch (ActivityNotFoundException e) {
            Log.e(TAG, "memories-pc: mods folder: no folder picker: " + e);
            answer("failed", "This device has no app to choose a folder with.");
        } catch (RuntimeException e) {
            Log.e(TAG, "memories-pc: mods folder: the folder picker could not be opened: " + e);
            answer("failed", "The folder picker could not be opened.");
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK || isFinishing())
            return;
        Uri tree = data != null ? data.getData() : null;
        if (result != RESULT_OK || tree == null) {
            answer("cancelled", "result " + result);
            return;
        }
        String path = pathOf(tree);
        Log.i(TAG, "memories-pc: mods folder: chosen " + tree + (path == null ? ", no path" : ", " + path));
        if (path == null)
            answer("failed", "That folder is not on the phone or on an SD card (a cloud drive or another app has "
                    + "no path the game can read). Choose a folder such as Download/MyMods.");
        else
            answer("chosen", path);
    }

    /**
     * The path of a folder the external storage provider gave: "primary:Download/MyMods" is
     * /storage/emulated/0/Download/MyMods (this user's), "home:" its Documents folder, and
     * "1234-ABCD:Mods" an SD card, or "0123456789ABCDEF:Mods" a USB drive, /storage/<id>/Mods. Null for
     * another provider, and for a document id that climbs out of its volume ("primary:../../data").
     */
    static String pathOf(Uri tree) {
        if (!STORAGE.equals(tree.getAuthority()))
            return null;
        String id;
        try {
            id = DocumentsContract.getTreeDocumentId(tree);
        } catch (RuntimeException e) {
            return null;
        }
        int colon = id.indexOf(':');
        if (colon <= 0)
            return null;
        String volume = id.substring(0, colon), inside = id.substring(colon + 1);
        File base;
        if (volume.equals("primary"))
            base = Environment.getExternalStorageDirectory();
        else if (volume.equals("home"))
            base = new File(Environment.getExternalStorageDirectory(), "Documents");
        else if (volume.matches("[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}|[0-9A-Fa-f]{16}"))
            base = new File("/storage/" + volume);
        else
            return null;
        if (inside.isEmpty())
            return base.getPath();
        try {
            String root = base.getCanonicalPath(), path = new File(base, inside).getCanonicalPath();
            return path.equals(root) || path.startsWith(root + "/") ? path : null;
        } catch (IOException e) {
            return null;
        }
    }
}
