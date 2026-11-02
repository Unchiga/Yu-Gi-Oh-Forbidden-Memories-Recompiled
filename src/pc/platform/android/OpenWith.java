package org.yfmredecomp.game;

import android.app.Activity;
import android.content.ClipData;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/**
 * "Open with" for a mod's .zip: what the system starts when the player
 * chooses the game for a .zip in a browser, Discord or a file manager
 * (ACTION_VIEW), or shares one to it (ACTION_SEND); the manifest's filters
 * are in tools/pc/package_android.py.
 *
 * <p>The game's own activity is SDL's SDLActivity, which is singleInstance
 * and has no onNewIntent, and which takes the path of an intent's data for a
 * file dropped on the window (a content:// URI's path is nothing the game
 * could open). So this activity takes the intent instead: it writes the
 * document's URI to {@link #RECORD} in the internal files folder, in
 * DiscPicker's format, brings the game forward and ends. The game reads the
 * record while it runs, or at its next start (android.c,
 * Platform_TakeOpenedMod), and opens its Mods panel on the same import Import
 * mod... runs (mods_window.c, ModsWindow_ImportOpened), which asks before it
 * imports. Nothing here reads the document.
 *
 * <p>It runs in a process of its own (":open", package_android.py), as
 * Restart does, and everything it does is caught: an intent from any app,
 * however malformed, can end this process, never the game's. It is shown to
 * nobody (translucent, no history); whether it sits in the sender's task or
 * a new one is the sender's choice (FLAG_ACTIVITY_NEW_TASK), and the game
 * (singleInstance) always comes up in its own.
 *
 * <p>The read right the sender gave this activity does not outlast it, so it
 * is handed on: the intent that brings the game forward carries the URI in
 * its ClipData (not its data, which SDLActivity would take for a dropped
 * file) with FLAG_GRANT_READ_URI_PERMISSION, and the right then lasts as
 * long as that activity of the game does. A game that is started again in
 * between (Apply and restart, the system ending its process) no longer has
 * it, and the record's file cannot be read (notes/mods-window.md, "Open
 * with"). The document is not copied here: a .zip can be a gigabyte, and
 * this is the main thread.
 *
 * <p>A VIEW's document is the intent's data, a SEND's the EXTRA_STREAM (or,
 * with the extra missing, the ClipData's first item). Only content: URIs are
 * taken: another app's file: path is unreadable to the game, and it would
 * let any app aim the game at its own private files. Anything else is
 * answered "failed", with a reason the Mods panel shows. SEND_MULTIPLE is not
 * offered: one mod at a time.
 */
public class OpenWith extends Activity {
    private static final String TAG = "memories";
    /** The record's file, in getFilesDir(); android.c reads and removes it. */
    public static final String RECORD = "open-with.txt";
    private static final String UNREADABLE = "The game cannot read that file; open it from a file manager or the Downloads app.";

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            Intent intent = getIntent();
            Uri uri = uriOf(intent);
            if (uri == null) {
                Log.w(TAG, "memories-pc: open with: no document in the intent");
                write("failed", "The app that sent this file did not give the game a file to read.");
            } else if (!"content".equals(uri.getScheme())) {
                Log.w(TAG, "memories-pc: open with: not a content URI (" + uri.getScheme() + ")");
                write("failed", UNREADABLE);
                uri = null;
            } else {
                Log.i(TAG, "memories-pc: open with: content URI of " + uri.toString().length() + " characters");
                write("chosen", uri.toString());
            }
            bringGame(uri);
        } catch (Throwable e) {
            Log.e(TAG, "memories-pc: open with: " + e);
        }
        finish();
    }

    /** The intent's document, or null. */
    private static Uri uriOf(Intent intent) {
        if (intent == null)
            return null;
        Uri uri = null;
        if (Intent.ACTION_SEND.equals(intent.getAction())) {
            Object stream = intent.getParcelableExtra(Intent.EXTRA_STREAM);
            if (stream instanceof Uri)
                uri = (Uri) stream;
        } else {
            uri = intent.getData();
        }
        ClipData clip = intent.getClipData();
        if (uri == null && clip != null && clip.getItemCount() > 0)
            uri = clip.getItemAt(0).getUri();
        return uri;
    }

    /**
     * Brings the game forward, with the read right for `uri` (null: none).
     * When the system refuses the right (a SecurityException), the game is
     * brought forward without it and the record says it could not be read:
     * the player is told, not left looking at a game that did nothing.
     */
    private void bringGame(Uri uri) {
        Intent game = new Intent();
        game.setClassName(this, "org.libsdl.app.SDLActivity");
        if (uri != null) {
            game.setClipData(ClipData.newRawUri("mod", uri));
            game.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        }
        try {
            startActivity(game);
            return;
        } catch (RuntimeException e) {
            Log.e(TAG, "memories-pc: open with: could not bring the game forward: " + e);
        }
        if (uri == null)
            return;
        write("failed", "The game could not be given access to that file; open it from a file manager or the Downloads app.");
        try {
            startActivity(new Intent().setClassName(this, "org.libsdl.app.SDLActivity"));
        } catch (RuntimeException e) {
            Log.e(TAG, "memories-pc: open with: could not bring the game forward without the file either: " + e);
        }
    }

    /**
     * Writes the record through a temporary file, renamed only once whole.
     * Any earlier record goes first: one that could not be written must not
     * leave the file opened before it to be imported in its place. Two
     * lines, as DiscPicker's: "chosen" or "failed" and a blank id, then the
     * URI or the reason.
     */
    private void write(String what, String text) {
        File record = new File(getFilesDir(), RECORD);
        File temporary = new File(getFilesDir(), RECORD + ".tmp");
        if (record.exists() && !record.delete())
            Log.e(TAG, "memories-pc: open with: could not remove " + record);
        try (FileOutputStream out = new FileOutputStream(temporary)) {
            out.write((what + " \n" + text + "\n").getBytes(StandardCharsets.UTF_8));
            out.getFD().sync();
        } catch (IOException e) {
            Log.e(TAG, "memories-pc: open with: could not write " + temporary + ": " + e);
            if (!temporary.delete() && temporary.exists())
                Log.e(TAG, "memories-pc: open with: could not remove " + temporary);
            return;
        }
        if (!temporary.renameTo(record))
            Log.e(TAG, "memories-pc: open with: could not write " + record);
    }
}
