package org.yfmredecomp.game;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/**
 * What the two activities of the Mods panel's Mods folder... share
 * ({@link ModsFolderPicker}, {@link ModsFolderAccess}): each asks the system
 * one thing in a task of its own and writes the answer to a small file in the
 * internal files folder, {@link #RECORD}, as DiscPicker does for the disc
 * image, so that the answer is not lost when the system ends the game's
 * process while the system's screen is in front. The record is two lines:
 * "chosen", "cancelled" or "failed" and the request's id (the Intent's "id"
 * extra, so that the game takes only the answer to the request it waits for),
 * then the path, the word "granted", or the reason. Ended without an answer
 * (its task closed, a newer request clearing it), it answers "cancelled".
 */
abstract class ModsFolderBase extends Activity {
    static final String TAG = "memories";
    /** The answer's file, in getFilesDir(); android.c reads and removes it. */
    public static final String RECORD = "mods-folder.txt";
    private static final String ASKED = "asked";

    boolean asked;
    private boolean answered;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        // Made again after the system ended the process, or for a
        // configuration change it did not take: the question is up already
        // and its answer is on its way.
        asked = state != null && state.getBoolean(ASKED);
        if (!asked)
            ask();
    }

    /** Puts the question to the system; sets {@link #asked} once it is up. */
    abstract void ask();

    @Override
    protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state);
        state.putBoolean(ASKED, asked);
    }

    @Override
    protected void onDestroy() {
        if (!answered && isFinishing() && !isChangingConfigurations()) {
            Log.i(TAG, "memories-pc: mods folder: closed without an answer");
            write("cancelled", "closed without an answer");
        }
        super.onDestroy();
    }

    /** Writes the record and ends. */
    void answer(String what, String text) {
        write(what, text);
        finish();
    }

    /** Writes the record through a temporary file, renamed only once whole. */
    private void write(String what, String text) {
        String id = getIntent() != null ? getIntent().getStringExtra("id") : null;
        File record = new File(getFilesDir(), RECORD);
        File temporary = new File(getFilesDir(), RECORD + ".tmp");
        answered = true;
        try (FileOutputStream out = new FileOutputStream(temporary)) {
            out.write((what + " " + (id != null ? id : "") + "\n" + text + "\n").getBytes(StandardCharsets.UTF_8));
            out.getFD().sync();
        } catch (IOException e) {
            Log.e(TAG, "memories-pc: mods folder: could not write " + temporary + ": " + e);
            if (!temporary.delete() && temporary.exists())
                Log.e(TAG, "memories-pc: mods folder: could not remove " + temporary);
            return;
        }
        if (!temporary.renameTo(record))
            Log.e(TAG, "memories-pc: mods folder: could not write " + record);
    }
}
