import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;
import jitperf.JitPerf;

/** Run the unmodified upstream pure kernels; host checks independent checksums. */
public final class KernelBench extends MIDlet {
    protected void startApp() {
        int n = 50000;
        String[] names = {"locals", "constants", "fields", "arrays", "branches"};
        for (int k=0;k<names.length;k++) {
            work(k,0); // Resolve and initialize outside the measured call.
            long start = System.currentTimeMillis();
            int value = work(k,n);
            long elapsed = System.currentTimeMillis()-start;
            System.out.println("KERNEL name="+names[k]+" iterations="+n+" checksum="+Integer.toHexString(value)+" elapsed_ms="+elapsed);
        }
        System.out.println("KERNELS_DONE");
        Screen screen = new Screen();
        Display.getDisplay(this).setCurrent(screen);
        new Thread(new Finish(this,screen)).start();
    }
    private static int work(int k,int n) {
        switch(k) {
            case 0:return JitPerf.locals(n);
            case 1:return JitPerf.constants(n);
            case 2:return JitPerf.fields(n);
            case 3:return JitPerf.arrays(n);
            default:return JitPerf.branches(n);
        }
    }
    protected void pauseApp() {}
    protected void destroyApp(boolean unconditional) {}
    static final class Screen extends Canvas {
        protected void paint(Graphics g) {
            g.setColor(0); g.fillRect(0,0,getWidth(),getHeight());
        }
    }
    static final class Finish implements Runnable {
        final KernelBench owner;
        final Screen screen;
        Finish(KernelBench app,Screen canvas) { owner=app;screen=canvas; }
        public void run() {
            screen.repaint();screen.serviceRepaints();
            try { Thread.sleep(50); } catch (Exception e) {}
            owner.notifyDestroyed();
        }
    }
}
