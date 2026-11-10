import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;
import javax.microedition.media.Manager;
import javax.microedition.media.Player;

/** An audible WAV reference followed by a repeating C4/E4/G4 MIDI pattern. */
public final class MidiAudition extends MIDlet implements Runnable {
    private String phase = "Starting";
    private Screen screen;
    private Player player;
    protected void startApp() {
        screen = new Screen();
        Display.getDisplay(this).setCurrent(screen);
        new Thread(this).start();
    }
    protected void pauseApp() {}
    protected void destroyApp(boolean unconditional) { if (player != null) player.close(); }
    public void run() {
        try {
            phase = "WAV reference: 440 Hz"; screen.repaint();
            player = Manager.createPlayer(getClass().getResourceAsStream("/tone.wav"), "audio/x-wav");
            player.start(); Thread.sleep(1600); player.close();
            phase = "MIDI loop: C4 E4 G4"; screen.repaint();
            player = Manager.createPlayer(getClass().getResourceAsStream("/notes.mid"), "audio/midi");
            player.setLoopCount(-1); player.start();
            System.out.println("MIDI_AUDITION_BEGIN");
            Thread.sleep(60000);
            if (player.getState() != 400) throw new Exception("loop ended unexpectedly");
            player.close(); player = null;
            phase = "MIDI loop completed"; screen.repaint();
            System.out.println("MIDI_AUDITION_OK");
        } catch (Exception e) {
            System.out.println("MIDI_AUDITION_ERROR " + e);
            phase = "Error: " + e; screen.repaint();
        }
    }
    final class Screen extends Canvas {
        Screen() { setFullScreenMode(true); }
        protected void paint(Graphics g) {
            g.setColor(0x102030); g.fillRect(0, 0, getWidth(), getHeight());
            g.setColor(0xffffff); g.drawString("MIDI speaker test", 8, 30, 20);
            g.drawString(phase, 8, 75, 20);
            g.drawString("WAV then MIDI for 60 sec", 8, 115, 20);
        }
    }
}
