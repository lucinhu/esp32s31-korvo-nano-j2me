import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;
import javax.microedition.media.Manager;
import javax.microedition.media.Player;

/** Sustained 3 s WAV and MIDI; catches short writes and truncated tails. */
public final class AudioBench extends MIDlet implements Runnable {
    private int phase;
    private Screen screen;
    protected void startApp() {
        screen = new Screen(this);
        Display.getDisplay(this).setCurrent(screen);
        new Thread(this).start();
    }
    protected void pauseApp() {}
    protected void destroyApp(boolean unconditional) {}
    public void run() {
        try {
            test("WAV", "/tone.wav", "audio/x-wav");
            test("MIDI", "/notes.mid", "audio/midi");
        } catch (Exception e) { System.out.println("AUDIO_BENCH_ERROR " + e); }
        notifyDestroyed();
    }
    private void test(String name, String path, String type) throws Exception {
        phase++;
        Player p = Manager.createPlayer(getClass().getResourceAsStream(path), type);
        p.realize(); p.prefetch();
        System.out.println("AUDIO_BENCH_BEGIN " + name + " duration=" + p.getDuration());
        p.start();
        for (int i = 0; i < 80; i++) {
            screen.repaint();
            Thread.sleep(50);
        }
        System.out.println("AUDIO_BENCH_END " + name + " time=" + p.getMediaTime() + " state=" + p.getState());
        p.close();
        Thread.sleep(150);
    }
    static final class Screen extends Canvas {
        final AudioBench owner;
        Screen(AudioBench app) { owner = app; setFullScreenMode(true); }
        protected void paint(Graphics g) {
            g.setColor(0x102030); g.fillRect(0, 0, getWidth(), getHeight());
            g.setColor(0xffffff); g.drawString("AUDIO TEST " + owner.phase, 10, 40, 20);
        }
    }
}
