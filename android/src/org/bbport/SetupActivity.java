// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
package org.bbport;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.view.Gravity;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * The launcher: starts the game when its files are in place, else copies them in from a folder
 * the user picks (Android's folder picker; no storage permission, no adb).
 *
 * The folder is what tools/android_prepare.sh makes on a PC: boot-linked.bin, content.bin and
 * patches.bin (run.sh's, for this build of the port) and app0/ (the game dump). Everything goes
 * into this app's external files folder, where BbportActivity reads it; files the app writes
 * itself it can always read (adb-pushed folders could not be entered). A file already there
 * with the same size is skipped, so an interrupted copy resumes.
 */
public class SetupActivity extends Activity {
    private static final int PICK_FOLDER = 1;
    private static final String[] PREPARED = {"boot-linked.bin", "content.bin", "patches.bin"};

    private TextView status;
    private ProgressBar progress;
    private Button pick;

    /** One file of the picked folder: its document, where it goes, its size. */
    private static final class Item {
        final Uri uri;
        final File target;
        final long size;

        Item(Uri uri, File target, long size) {
            this.uri = uri;
            this.target = target;
            this.size = size;
        }
    }

    static boolean ready(File files) {
        for (String name : PREPARED) {
            if (!new File(files, name).isFile()) return false;
        }
        return new File(files, "app0/eboot.bin").isFile();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (ready(getExternalFilesDir(null))) {
            startGame();
            return;
        }
        final int pad = (int) (24 * getResources().getDisplayMetrics().density);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(pad, pad, pad, pad);
        layout.setGravity(Gravity.CENTER_HORIZONTAL);
        TextView title = new TextView(this);
        title.setTextSize(24);
        title.setText("bbport");
        title.setPadding(0, 0, 0, pad / 2);
        layout.addView(title);
        TextView intro = new TextView(this);
        intro.setTextSize(16);
        intro.setText("bbport needs the game's files.\n\n"
                + "On a PC, tools/android_prepare.sh makes a folder with boot-linked.bin, content.bin, "
                + "patches.bin and app0/ (your game dump's contents go into app0/). Copy that folder to "
                + "this device (USB file transfer or a card), then choose it here: it is copied into "
                + "bbport's own storage (about 30 GB).");
        layout.addView(intro);
        pick = new Button(this);
        pick.setText("Choose folder");
        pick.setOnClickListener(v -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            startActivityForResult(intent, PICK_FOLDER);
        });
        layout.addView(pick);
        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setVisibility(ProgressBar.GONE);
        layout.addView(progress);
        status = new TextView(this);
        status.setTextSize(14);
        layout.addView(status);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(layout);
        // Android 15 draws apps edge to edge: keep clear of the status and navigation bars.
        scroll.setOnApplyWindowInsetsListener((v, insets) -> {
            android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars()
                    | WindowInsets.Type.displayCutout());
            v.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            return insets;
        });
        setContentView(scroll);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_FOLDER || result != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        final Uri tree = data.getData();
        pick.setEnabled(false);
        progress.setVisibility(ProgressBar.VISIBLE);
        status.setText("Reading the folder...");
        new Thread(() -> copy(tree), "bbport setup").start();
    }

    private void say(String text) {
        runOnUiThread(() -> status.setText(text));
    }

    private void fail(String text) {
        runOnUiThread(() -> {
            status.setText(text);
            pick.setEnabled(true);
            progress.setVisibility(ProgressBar.GONE);
        });
    }

    /** The files under `doc` (a directory of the picked tree), with where each one goes. */
    private void list(Uri tree, String doc, File target, List<Item> out) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, doc);
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_SIZE};
        try (Cursor c = getContentResolver().query(children, columns, null, null, null)) {
            while (c != null && c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                File to = new File(target, name);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    list(tree, id, to, out);
                } else {
                    out.add(new Item(DocumentsContract.buildDocumentUriUsingTree(tree, id), to, c.getLong(3)));
                }
            }
        }
    }

    private void copy(Uri tree) {
        try {
            final File files = getExternalFilesDir(null);
            List<Item> items = new ArrayList<>();
            list(tree, DocumentsContract.getTreeDocumentId(tree), files, items);
            // the folder must be the prepared one
            boolean[] found = new boolean[PREPARED.length + 1];
            long total = 0, needed = 0;
            for (Item it : items) {
                for (int i = 0; i < PREPARED.length; ++i) {
                    if (it.target.equals(new File(files, PREPARED[i]))) found[i] = true;
                }
                if (it.target.equals(new File(files, "app0/eboot.bin"))) found[PREPARED.length] = true;
                total += it.size;
                if (!(it.target.isFile() && it.target.length() == it.size)) needed += it.size;
            }
            for (int i = 0; i < found.length; ++i) {
                if (!found[i]) {
                    fail("This folder lacks " + (i < PREPARED.length ? PREPARED[i] : "app0/eboot.bin")
                            + ": choose the folder tools/android_prepare.sh made (with app0/ inside).");
                    return;
                }
            }
            // room for it, with 2 GB left over for the rest of the device
            final long free = new StatFs(files.getPath()).getAvailableBytes();
            if (needed + (2L << 30) > free) {
                fail(String.format("Not enough space: %.1f GB to copy, %.1f GB free (2 GB must stay free).",
                        needed / 1e9, free / 1e9));
                return;
            }
            long done = 0;
            byte[] buffer = new byte[1 << 20];
            for (Item it : items) {
                if (it.target.isFile() && it.target.length() == it.size) {
                    done += it.size;
                    continue;
                }
                File parent = it.target.getParentFile();
                if (parent != null) parent.mkdirs();
                File partial = new File(it.target.getPath() + ".part");
                try (InputStream in = getContentResolver().openInputStream(it.uri);
                     OutputStream out = new FileOutputStream(partial)) {
                    int n;
                    while ((n = in.read(buffer)) > 0) {
                        out.write(buffer, 0, n);
                        done += n;
                        final int permille = (int) (done * 1000 / Math.max(total, 1));
                        final long d = done, t = total;
                        runOnUiThread(() -> {
                            progress.setProgress(permille);
                            status.setText(String.format("Copying %s\n%.1f of %.1f GB", it.target.getName(),
                                    d / 1e9, t / 1e9));
                        });
                    }
                }
                if (!partial.renameTo(it.target)) {
                    fail("Could not finish " + it.target.getName());
                    return;
                }
            }
            new File(files, "user").mkdirs();
            say("Done.");
            runOnUiThread(this::startGame);
        } catch (Exception e) {
            fail("Copying failed: " + e);
        }
    }

    private void startGame() {
        startActivity(new Intent(this, BbportActivity.class));
        finish();
    }
}
