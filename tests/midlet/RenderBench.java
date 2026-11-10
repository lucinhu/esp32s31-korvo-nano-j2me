import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;

/** One paint request per Java scheduling yield, with changing full frames. */
public final class RenderBench extends MIDlet {
    protected void startApp() {
        Screen screen = new Screen(this);
        Display.getDisplay(this).setCurrent(screen);
        new Thread(screen).start();
    }
    protected void pauseApp() {}
    protected void destroyApp(boolean unconditional) {}
    static final class Screen extends Canvas implements Runnable {
        final RenderBench owner;
        int frame;
        Screen(RenderBench app) { owner=app; setFullScreenMode(true); }
        public void run() {
            for (frame=0;frame<240;frame++) {
                repaint(); serviceRepaints();
                try { Thread.sleep(1); } catch (Exception e) {}
            }
            System.out.println("RENDER_BENCH frames=240 final=" + frame);
            owner.notifyDestroyed();
        }
        protected void paint(Graphics g) {
            g.setColor((frame*13107)&0xffffff); g.fillRect(0,0,getWidth(),getHeight());
            for (int i=0;i<64;i++) {
                g.setColor((i*987657+frame*321)&0xffffff);
                g.fillRect((i*37+frame*3)%(getWidth()-16),(i*19+frame)%(getHeight()-16),16,16);
            }
            g.setColor(0xffffff); g.drawString("RENDER " + frame,4,4,20);
        }
    }
}
