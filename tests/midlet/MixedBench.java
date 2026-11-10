import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;
import jitperf.JitPerf;

/** Exercise the unmodified upstream field/array loop without timed rendering. */
public final class MixedBench extends MIDlet implements Runnable {
    private Screen screen;
    protected void startApp() {
        JitPerf.mixed(0);
        long start = System.currentTimeMillis();
        int value = JitPerf.mixed(50000);
        long elapsed = System.currentTimeMillis() - start;
        System.out.println("MIXED iterations=50000 checksum=" + Integer.toHexString(value) + " elapsed_ms=" + elapsed);
        screen = new Screen();
        Display.getDisplay(this).setCurrent(screen);
        new Thread(this).start();
    }
    public void run() {
        screen.repaint(); screen.serviceRepaints();
        try { Thread.sleep(50); } catch (Exception e) {}
        notifyDestroyed();
    }
    protected void pauseApp() {}
    protected void destroyApp(boolean unconditional) {}
    static final class Screen extends Canvas {
        protected void paint(Graphics g) {
            g.setColor(0); g.fillRect(0,0,getWidth(),getHeight());
        }
    }
}
