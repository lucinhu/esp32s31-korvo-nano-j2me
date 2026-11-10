import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;

/** Fixed work and final pixels: compare runtimes without changing game time. */
public final class PerfBench extends MIDlet {
    protected void startApp() {
        Screen screen = new Screen(this, Integer.parseInt(getAppProperty("Perf-Work")),
                                   Integer.parseInt(getAppProperty("Perf-Frames")));
        Display.getDisplay(this).setCurrent(screen);
        new Thread(screen).start();
    }
    protected void pauseApp() {}
    protected void destroyApp(boolean unconditional) {}
    static final class Screen extends Canvas implements Runnable {
        final PerfBench owner;
        final int work, count;
        int frame, checksum = 0x1234abcd;
        Screen(PerfBench app, int iterations, int frames) {
            owner = app; work = iterations; count = frames;
            setFullScreenMode(true);
        }
        public void run() {
            for (int n = 0; n < count; n++) {
                for (int i = 0; i < work; i++) {
                    checksum = checksum * 1664525 + 1013904223;
                    checksum ^= checksum >>> 13;
                }
                frame = n;
                repaint(); serviceRepaints();
            }
            System.out.println("PERF_BENCH frames=" + count + " work=" + work + " checksum=" + checksum);
            owner.notifyDestroyed();
        }
        protected void paint(Graphics g) {
            int w = getWidth(), h = getHeight();
            g.setColor((frame * 13107) & 0xffffff); g.fillRect(0, 0, w, h);
            for (int i = 0; i < 64; i++) {
                g.setColor((i * 987657 + frame * 321) & 0xffffff);
                g.fillRect((i * 37 + frame * 3) % (w - 16), (i * 19 + frame) % (h - 16), 16, 16);
            }
            g.setColor(0xffffff);
            g.drawString("PERF " + frame, 4, 4, 20);
            g.drawString("SUM " + checksum, 4, 28, 20);
        }
    }
}
