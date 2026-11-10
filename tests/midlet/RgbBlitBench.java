import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;

/** Time full-screen drawRGB separately from frame pacing and presentation. */
public final class RgbBlitBench extends MIDlet {
    protected void startApp() {
        Screen screen = new Screen(this);
        Display.getDisplay(this).setCurrent(screen);
        new Thread(screen).start();
    }
    protected void pauseApp() {}
    protected void destroyApp(boolean unconditional) {}
    static final class Screen extends Canvas implements Runnable {
        final RgbBlitBench owner;
        final int[] pixels = new int[240 * 320];
        boolean measured, alpha;
        long blitMillis;
        int draws, frame;
        Screen(RgbBlitBench app) {
            owner = app;
            setFullScreenMode(true);
            for (int i = 0; i < pixels.length; ++i)
                pixels[i] = 0xff000000 | ((i * 987657) & 0xffffff);
        }
        public void run() {
            try {
                for (int mode = 0; mode < 2; ++mode) {
                    alpha = mode != 0;
                    for (int warm = 0; warm < 8; ++warm) {
                        repaint(); serviceRepaints(); Thread.sleep(1);
                    }
                    blitMillis = 0; draws = 0; measured = true;
                    for (frame = 0; frame < 80; ++frame) {
                        repaint(); serviceRepaints(); Thread.sleep(1);
                    }
                    measured = false;
                    System.out.println("RGB_BLIT_BENCH alpha=" + mode + " draws=" + draws
                        + " blit_ms=" + blitMillis + " pixels_per_draw=76800");
                }
            } catch (Exception e) {
                System.out.println("RGB_BLIT_ERROR " + e);
            }
            owner.notifyDestroyed();
        }
        protected void paint(Graphics g) {
            pixels[0] = 0xff000000 | (frame * 13107);
            long start = System.currentTimeMillis();
            for (int i = 0; i < 8; ++i)
                g.drawRGB(pixels, 0, 240, 0, 0, 240, 320, alpha);
            if (measured) {
                blitMillis += System.currentTimeMillis() - start;
                draws += 8;
            }
        }
    }
}
