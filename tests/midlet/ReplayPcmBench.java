import java.io.ByteArrayInputStream;
import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;
import javax.microedition.media.Manager;
import javax.microedition.media.Player;
import javax.microedition.media.PlayerListener;

/** Reuse one short, non-silent PCM player, as games reuse sound effects. */
public final class ReplayPcmBench extends MIDlet implements Runnable, PlayerListener {
    private volatile int endings;
    protected void startApp() {
        Display.getDisplay(this).setCurrent(new Screen());
        new Thread(this).start();
    }
    protected void pauseApp() {}
    protected void destroyApp(boolean unconditional) {}
    public void playerUpdate(Player player, String event, Object value) {
        if ("endOfMedia".equals(event)) endings++;
    }
    static void le(byte[] b, int pos, int value, int count) {
        for (int i = 0; i < count; ++i) b[pos + i] = (byte)(value >>> (i * 8));
    }
    public void run() {
        Player player = null;
        try {
            final int frames = 1440;
            byte[] wav = new byte[44 + frames * 2];
            byte[] header = {82,73,70,70,0,0,0,0,87,65,86,69,102,109,116,32,
                16,0,0,0,1,0,1,0,64,31,0,0,(byte)128,62,0,0,2,0,16,0,
                100,97,116,97,0,0,0,0};
            System.arraycopy(header, 0, wav, 0, header.length);
            le(wav, 4, wav.length - 8, 4); le(wav, 40, frames * 2, 4);
            for (int i = 0; i < frames; ++i) le(wav, 44 + i * 2, i % 20 < 10 ? 4000 : -4000, 2);
            player = Manager.createPlayer(new ByteArrayInputStream(wav), "audio/x-wav");
            player.addPlayerListener(this);
            for (int round = 1; round <= 6; ++round) {
                if (round > 3) player.setMediaTime(0);
                player.start();
                player.prefetch();
                if (player.getState() != 400) throw new Exception("prefetch stopped playback");
                long deadline = System.currentTimeMillis() + 3000;
                while (endings < round && System.currentTimeMillis() < deadline) Thread.sleep(20);
                if (player.getState() != 300 || endings != round)
                    throw new Exception("round=" + round + " state=" + player.getState() + " endings=" + endings);
                System.out.println("REPLAY_PCM_ROUND " + round + " frames=" + frames);
                Thread.sleep(120);
            }
            System.out.println("REPLAY_PCM_OK rounds=6 expected_frames=8640");
        } catch (Exception e) {
            System.out.println("REPLAY_PCM_ERROR " + e);
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
