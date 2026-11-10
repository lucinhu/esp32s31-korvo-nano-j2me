import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;
import javax.microedition.media.Manager;
import javax.microedition.media.Player;
import java.io.InputStream;

/** The same audible speech as MP3, AMR-NB and AMR-WB, repeated for listening. */
public final class AmrAudition extends MIDlet implements Runnable {
    private String phase = "Starting";
    private Screen screen;
    private Player current;
    private int passed, failed;
    protected void startApp() {
        screen = new Screen();
        Display.getDisplay(this).setCurrent(screen);
        new Thread(this).start();
    }
    protected void pauseApp() {}
    protected void destroyApp(boolean unconditional) { if (current != null) current.close(); }
    public void run() {
        System.out.println("AMR_AUDITION_BEGIN");
        for (int round = 1; round <= 4; round++) {
            play("MP3", "/speech.mp3", "audio/mpeg", round);
            play("AMR-NB", "/speech.amr", "audio/amr", round);
            play("AMR-WB", "/speech.awb", "audio/amr-wb", round);
        }
        phase = "PASS " + passed + " / FAIL " + failed;
        screen.repaint();
        System.out.println("AMR_AUDITION_DONE pass=" + passed + " fail=" + failed);
    }
    private void play(String name, String path, String type, int round) {
        InputStream stream = null;
        try {
            stream = getClass().getResourceAsStream(path);
            current = Manager.createPlayer(stream, type);
            current.realize(); current.prefetch();
            long duration = current.getDuration();
            if (duration < 1000000 || duration > 10000000)
                throw new Exception("invalid speech duration " + duration);
            phase = name + "  " + round + "/4"; screen.repaint();
            System.out.println("AMR_AUDITION_PLAY " + name + " round=" + round + " duration=" + duration);
            current.start();
            Thread.sleep(duration / 1000 + 600);
            if (current.getState() != 300) throw new Exception("state=" + current.getState());
            if (current.getMediaTime() < duration - 20000) throw new Exception("speech ended early");
            passed++;
            System.out.println("AMR_AUDITION_OK " + name + " time=" + current.getMediaTime());
        } catch (Exception e) {
            failed++;
            System.out.println("AMR_AUDITION_ERROR " + name + " " + e);
        } finally {
            if (current != null) current.close();
            current = null;
            if (stream != null) try { stream.close(); } catch (Exception e) {}
        }
    }
    final class Screen extends Canvas {
        Screen() { setFullScreenMode(true); }
        protected void paint(Graphics g) {
            g.setColor(0x102030); g.fillRect(0, 0, getWidth(), getHeight());
            g.setColor(0xffffff); g.drawString("AMR speaker test", 8, 30, 20);
            g.drawString(phase, 8, 75, 20);
            g.drawString("Audio test.", 8, 115, 20);
            g.drawString("One, two, three.", 8, 145, 20);
            g.drawString("MP3 / AMR-NB / AMR-WB", 8, 200, 20);
        }
    }
}
