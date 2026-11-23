package de.pbcplus.emulator;

import android.Manifest;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothServerSocket;
import android.bluetooth.BluetoothSocket;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;

import java.io.InputStream;
import java.io.OutputStream;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;

import org.libsdl.app.SDLActivity;

// SDL's activity does nearly everything: it loads libSDL3.so and libmain.so (the emulator with
// frontend/android.c) and runs main() there. Two things are done here:
//
// A UF2 opened with this app from a file manager. SDL would hand main() only the path part of
// the content:// address, which cannot be opened, and nothing at all once the app runs. So the
// whole address is passed on here, in both cases, as a dropped file.
//
// The Bluetooth serial port (Serial Port Profile) that frontend/android.c joins to the
// firmware's USB serial port: a PC paired with the phone opens it like a serial cable.
public class MainActivity extends SDLActivity {
    private static Uri take(Intent intent) {
        if (intent == null || !Intent.ACTION_VIEW.equals(intent.getAction()))
            return null;
        return intent.getData();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Uri uri = savedInstanceState == null ? take(getIntent()) : null;
        if (take(getIntent()) != null) {
            // keeps SDL from passing the path on; the permission to read the address stays
            Intent plain = new Intent(getIntent());
            plain.setData(null);
            plain.setFlags(getIntent().getFlags());
            setIntent(plain);
        }
        super.onCreate(savedInstanceState);
        if (uri != null && !mBrokenLibraries)
            SDLActivity.onNativeDropFile(uri.toString());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        Uri uri = take(intent);
        if (uri != null && !mBrokenLibraries)
            SDLActivity.onNativeDropFile(uri.toString());
    }

    @Override
    protected void onDestroy() {
        btStop();
        super.onDestroy();
    }

    // ------------------------------------------------------------------------------ Bluetooth

    // frontend/android.c: the states, and the bytes to and from the firmware
    private static final int BT_OFF = 0, BT_WAITING = 1, BT_CONNECTED = 2, BT_NO_PERMISSION = 3, BT_UNAVAILABLE = 4;
    private native void nativeBtState(int state);
    private native void nativeBtIn(byte[] data, int len);
    private native int nativeBtOut(byte[] buffer);

    private static final UUID SERIAL_PORT = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB");
    private static final int BT_REQUEST = 0x5042;
    private final Object btGuard = new Object();
    private boolean btWanted;
    private Thread btThread;
    private BluetoothServerSocket btServer;
    private BluetoothSocket btSocket;

    // Called from main(): offers the serial port until btStop().
    public void btStart() {
        runOnUiThread(() -> {
            if (Build.VERSION.SDK_INT >= 31
                    && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[] {Manifest.permission.BLUETOOTH_CONNECT}, BT_REQUEST);
                return;
            }
            btListen();
        });
    }

    public void btStop() {
        Thread t;
        synchronized (btGuard) {
            btWanted = false;
            t = btThread;
            btThread = null;
            btClose();
        }
        if (t != null)
            t.interrupt();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode != BT_REQUEST) {
            super.onRequestPermissionsResult(requestCode, permissions, grantResults);
            return;
        }
        if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED)
            btListen();
        else
            nativeBtState(BT_NO_PERMISSION);
    }

    private void btClose() {
        try {
            if (btServer != null)
                btServer.close();
        } catch (Exception e) {
        }
        try {
            if (btSocket != null)
                btSocket.close();
        } catch (Exception e) {
        }
        btServer = null;
        btSocket = null;
    }

    private boolean btIsWanted(Thread me) {
        synchronized (btGuard) {
            return btWanted && btThread == me;
        }
    }

    private void btListen() {
        synchronized (btGuard) {
            if (btThread != null)
                return;
            btWanted = true;
            btThread = new Thread(this::btRun, "bluetooth serial");
            btThread.start();
        }
    }

    // One PC at a time: waits for it, moves the bytes until it leaves, waits for the next.
    private void btRun() {
        Thread me = Thread.currentThread();
        BluetoothManager manager = (BluetoothManager) getSystemService(Context.BLUETOOTH_SERVICE);
        BluetoothAdapter adapter = manager != null ? manager.getAdapter() : null;
        int end = BT_OFF;
        while (btIsWanted(me)) {
            BluetoothSocket socket;
            try {
                if (adapter == null || !adapter.isEnabled()) {
                    end = BT_UNAVAILABLE;
                    break;
                }
                BluetoothServerSocket server = adapter.listenUsingRfcommWithServiceRecord("PBC+ Emulator", SERIAL_PORT);
                synchronized (btGuard) {
                    if (!btWanted || btThread != me) {
                        server.close();
                        break;
                    }
                    btServer = server;
                }
                nativeBtState(BT_WAITING);
                socket = server.accept();
                synchronized (btGuard) {
                    btServer = null;
                    btSocket = socket;
                }
                server.close();
            } catch (SecurityException e) {
                end = BT_NO_PERMISSION;
                break;
            } catch (Exception e) {
                // closed by btStop(), or Bluetooth went off: the loop's head tells which
                try {
                    Thread.sleep(300);
                } catch (InterruptedException e2) {
                }
                continue;
            }
            nativeBtState(BT_CONNECTED);
            btPump(socket);
            synchronized (btGuard) {
                if (btSocket == socket)
                    btSocket = null;
            }
        }
        if (end != BT_OFF || !btIsWanted(me)) {
            synchronized (btGuard) {
                if (btThread == me)
                    btThread = null;
            }
            nativeBtState(end);
        }
    }

    private void btPump(final BluetoothSocket socket) {
        final AtomicBoolean open = new AtomicBoolean(true);
        Thread writer = new Thread(() -> {
            byte[] buffer = new byte[4096];
            try {
                OutputStream out = socket.getOutputStream();
                while (open.get()) {
                    int n = nativeBtOut(buffer);
                    if (n > 0)
                        out.write(buffer, 0, n);
                }
            } catch (Exception e) {
            }
        }, "bluetooth serial out");
        writer.start();
        try {
            InputStream in = socket.getInputStream();
            byte[] buffer = new byte[4096];
            for (;;) {
                int n = in.read(buffer);
                if (n < 0)
                    break;
                if (n > 0)
                    nativeBtIn(buffer, n);
            }
        } catch (Exception e) {
        }
        open.set(false);
        try {
            socket.close();
        } catch (Exception e) {
        }
        try {
            writer.join(1000);
        } catch (InterruptedException e) {
        }
    }
}
