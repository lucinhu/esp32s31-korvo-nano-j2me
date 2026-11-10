import java.io.ByteArrayInputStream;
import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;
import javax.microedition.media.Manager;
import javax.microedition.media.Player;
import javax.microedition.media.PlayerListener;

/** A port accepts all 32 PCM frames in start(): endOfMedia must still fire. */
public final class ShortPcmBench extends MIDlet implements Runnable, PlayerListener {
    private int endings;
    protected void startApp() {
        Display.getDisplay(this).setCurrent(new Screen());
        new Thread(this).start();
    }
    protected void pauseApp() {}
    protected void destroyApp(boolean unconditional) {}
    public void playerUpdate(Player player, String event, Object value) {
        if ("endOfMedia".equals(event)) endings++;
    }
    public void run() {
        Player player = null;
        try {
            byte[] header = {82,73,70,70,100,0,0,0,87,65,86,69,102,109,116,32,
                16,0,0,0,1,0,1,0,64,31,0,0,(byte)128,62,0,0,2,0,16,0,
                100,97,116,97,64,0,0,0};
            byte[] wav = new byte[108];
            System.arraycopy(header, 0, wav, 0, header.length);
            player = Manager.createPlayer(new ByteArrayInputStream(wav), "audio/x-wav");
            player.addPlayerListener(this);
            player.start();
            Thread.sleep(160);
            int state = player.getState();
            if (state != 300 || endings != 1) throw new Exception("state=" + state + " events=" + endings);
            System.out.println("SHORT_PCM_OK state=" + state + " events=" + endings);
        } catch (Exception e) {
            System.out.println("SHORT_PCM_ERROR " + e);
        } finally {
            if (player != null) player.close();
            notifyDestroyed();
        }
    }
    static final class Screen extends Canvas {
        protected void paint(Graphics g) {
            g.setColor(0); g.fillRect(0, 0, getWidth(), getHeight());
        }
    }
}
