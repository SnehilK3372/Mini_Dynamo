package com.minidynamo.gateway.support;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.function.Function;

/**
 * A tiny in-JVM TCP server that speaks the cluster's length-prefixed framing, so
 * tests can exercise {@link com.minidynamo.gateway.cluster.ClusterClient} and the
 * whole gateway without a real C++ cluster. A supplied handler turns each request
 * payload into a response payload; framing is handled here.
 */
public class FakeClusterServer implements AutoCloseable {

    private final ServerSocket serverSocket;
    private final Thread acceptThread;
    private volatile boolean running = true;

    public FakeClusterServer(Function<String, String> handler) throws IOException {
        this.serverSocket = new ServerSocket(0);  // ephemeral port
        this.acceptThread = new Thread(() -> acceptLoop(handler), "fake-cluster");
        this.acceptThread.setDaemon(true);
        this.acceptThread.start();
    }

    public int port() {
        return serverSocket.getLocalPort();
    }

    public String endpoint() {
        return "localhost:" + port();
    }

    private void acceptLoop(Function<String, String> handler) {
        while (running) {
            try (Socket s = serverSocket.accept()) {
                String request = recvFramed(s.getInputStream());
                if (request != null) {
                    sendFramed(s.getOutputStream(), handler.apply(request));
                }
            } catch (IOException e) {
                if (running) {
                    // transient; keep serving
                }
            }
        }
    }

    private static void sendFramed(OutputStream out, String payload) throws IOException {
        byte[] body = payload.getBytes(StandardCharsets.UTF_8);
        out.write((body.length + "\n").getBytes(StandardCharsets.US_ASCII));
        out.write(body);
        out.flush();
    }

    private static String recvFramed(InputStream in) throws IOException {
        StringBuilder len = new StringBuilder();
        int c;
        while ((c = in.read()) != '\n') {
            if (c < 0) return null;
            len.append((char) c);
        }
        int n = Integer.parseInt(len.toString().trim());
        byte[] body = in.readNBytes(n);
        return new String(body, StandardCharsets.UTF_8);
    }

    @Override
    public void close() throws IOException {
        running = false;
        serverSocket.close();
        acceptThread.interrupt();
    }
}
