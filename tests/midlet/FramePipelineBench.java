import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;

/** Fixed Java work between changing frames; no time-based animation. */
public final class FramePipelineBench extends MIDlet {
    protected void startApp() {
        Screen screen=new Screen(this);
        Display.getDisplay(this).setCurrent(screen);
        new Thread(screen).start();
    }
    protected void pauseApp() {}
    protected void destroyApp(boolean unconditional) {}
    static final class Screen extends Canvas implements Runnable {
        final FramePipelineBench owner;
        int frame=-1, lastPaint=-1, checksum=0x1234abcd;
        final int[] painted=new int[4];
        Screen(FramePipelineBench owner) { this.owner=owner;setFullScreenMode(true); }
        public void run() {
            final int[] work={0,2000,6000,12000};
            for(int phase=0;phase<work.length;phase++) {
                long start=System.currentTimeMillis();
                for(int n=0;n<48;n++) {
                    for(int i=0;i<work[phase];i++) {
                        checksum=checksum*1664525+1013904223;
                        checksum^=checksum>>>13;
                    }
                    frame=phase*48+n;
                    repaint();serviceRepaints();
                    try { Thread.sleep(1); } catch(Exception ignored) {}
                }
                System.out.println("PIPELINE_BENCH work="+work[phase]+" frames=48 paints="+painted[phase]+" checksum="+
                    Integer.toHexString(checksum)+" elapsed_ms="+(System.currentTimeMillis()-start));
            }
            System.out.println("PIPELINE_BENCH_DONE");owner.notifyDestroyed();
        }
        protected void paint(Graphics g) {
            if(frame>=0 && frame!=lastPaint) { painted[frame/48]++;lastPaint=frame; }
            g.setColor((frame*13107)&0xffffff);g.fillRect(0,0,getWidth(),getHeight());
            for(int i=0;i<32;i++) {
                g.setColor((i*987657+frame*321)&0xffffff);
                g.fillRect((i*37+frame*3)%(getWidth()-16),(i*19+frame)%(getHeight()-16),16,16);
            }
            g.setColor(0xffffff);g.drawString("FRAME "+frame,4,4,20);
        }
    }
}
