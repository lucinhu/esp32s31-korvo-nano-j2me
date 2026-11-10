import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;
import javax.microedition.lcdui.game.GameCanvas;

public final class FillBench extends MIDlet {
    protected void startApp() {
        Screen screen=new Screen(this);
        Display.getDisplay(this).setCurrent(screen);
        new Thread(screen).start();
    }
    protected void pauseApp() {}
    protected void destroyApp(boolean unconditional) {}
    static final class Screen extends GameCanvas implements Runnable {
        final FillBench owner;
        Screen(FillBench owner) { super(false); this.owner=owner; setFullScreenMode(true); }
        public void run() {
            try {
                Image image=Image.createImage(240,320);
                Graphics target=image.getGraphics();
                for(int phase=0;phase<2;phase++) {
                    long start=System.currentTimeMillis();
                    for(int frame=0;frame<240;frame++) for(int draw=0;draw<6;draw++) {
                        target.setColor((frame*1237+draw*70001)&0xffffff);
                        int edge=draw&1;
                        target.fillRect(edge*8,edge*10,240-edge*16,320-edge*20);
                    }
                    long elapsed=System.currentTimeMillis()-start;
                    int[] pixels=new int[240*320];
                    image.getRGB(pixels,0,240,0,0,240,320);
                    int hash=0;
                    for(int i=0;i<pixels.length;i++) hash=hash*31+pixels[i];
                    System.out.println("FILL_BENCH phase="+(phase==0?"full":"clipped")+
                        " frames=240 draws=1440 checksum="+Integer.toHexString(hash)+" elapsed_ms="+elapsed);
                    target.setClip(3,5,231,307);
                    target.translate(7,-2);
                }
                Graphics screen=getGraphics();
                screen.drawImage(image,0,0,Graphics.TOP|Graphics.LEFT);
                flushGraphics();
                System.out.println("FILL_BENCH_DONE");
            } catch(Throwable error) { error.printStackTrace(); }
            owner.notifyDestroyed();
        }
    }
}
