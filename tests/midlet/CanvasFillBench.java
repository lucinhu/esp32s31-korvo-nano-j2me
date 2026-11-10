import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;

/** Measures Canvas painting; the port checks the final native/LCD pixels. */
public final class CanvasFillBench extends MIDlet {
    protected void startApp() { Display.getDisplay(this).setCurrent(new Screen(this)); }
    protected void pauseApp() {}
    protected void destroyApp(boolean unconditional) {}
    static final class Screen extends Canvas {
        final CanvasFillBench owner;
        boolean ran;
        Screen(CanvasFillBench owner) { this.owner=owner; setFullScreenMode(true); }
        protected void paint(Graphics target) {
            if(ran) return;
            ran=true;
            for(int phase=0;phase<2;phase++) {
                long start=System.currentTimeMillis();
                for(int frame=0;frame<240;frame++) for(int draw=0;draw<6;draw++) {
                    target.setColor((frame*1237+draw*70001)&0xffffff);
                    int edge=draw&1;
                    target.fillRect(edge*8,edge*10,240-edge*16,320-edge*20);
                }
                long elapsed=System.currentTimeMillis()-start;
                System.out.println("FILL_BENCH phase="+(phase==0?"full":"clipped")+
                    " frames=240 draws=1440 checksum=not-read elapsed_ms="+elapsed);
                target.setClip(3,5,231,307);
                target.translate(7,-2);
            }
            System.out.println("FILL_BENCH_DONE");
            owner.notifyDestroyed();
        }
    }
}
