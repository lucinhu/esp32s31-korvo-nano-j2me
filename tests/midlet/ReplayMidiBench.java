import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;
import javax.microedition.media.Manager;
import javax.microedition.media.Player;

/** The fixture supplies three MIDI effects; the source contains no game data. */
public final class ReplayMidiBench extends MIDlet implements Runnable {
    protected void startApp() {
        Display.getDisplay(this).setCurrent(new Screen());
        new Thread(this).start();
    }
    protected void pauseApp() {}
    protected void destroyApp(boolean unconditional) {}
    static void awaitEnd(Player p) throws Exception {
        long end = System.currentTimeMillis() + 5000;
        while (p.getState() == 400 && System.currentTimeMillis() < end) Thread.sleep(20);
        if (p.getState() != 300) throw new Exception("stuck state=" + p.getState());
    }
    static void resumeRound(Player p, int round) throws Exception {
        System.out.println("REPLAY_MIDI_RESUME_BEGIN round=" + round);
        p.start();
        long deadline = System.currentTimeMillis() + 2000;
        while (p.getMediaTime() <= 0 && p.getState() == 400 &&
               System.currentTimeMillis() < deadline) Thread.sleep(20);
        Thread.sleep(40);
        if (p.getState() != 400 || p.getMediaTime() <= 0)
            throw new Exception("MIDI did not reach a playing pause point");
        p.stop();
        if (p.getState() != 300) throw new Exception("MIDI stop state=" + p.getState());
        long stopped = p.getMediaTime();
        Thread.sleep(120);
        if (p.getState() != 300 || p.getMediaTime() != stopped)
            throw new Exception("stopped MIDI clock advanced");
        p.start();
        if (p.getState() != 400 || p.getMediaTime() < stopped)
            throw new Exception("MIDI resume reset the position");
        awaitEnd(p);
        long finished = p.getMediaTime();
        if (finished <= stopped) throw new Exception("MIDI resume made no progress");
        System.out.println("REPLAY_MIDI_RESUME_OK round=" + round +
                           " stopped_us=" + stopped + " finished_us=" + finished);
        Thread.sleep(150);
    }
    static void pausedHandoffRound(Player paused, Player other, int round) throws Exception {
        System.out.println("REPLAY_MIDI_PAUSED_HANDOFF_BEGIN round=" + round);
        paused.start();
        long deadline = System.currentTimeMillis() + 2000;
        while (paused.getMediaTime() <= 0 && paused.getState() == 400 &&
               System.currentTimeMillis() < deadline) Thread.sleep(20);
        Thread.sleep(40);
        if (paused.getState() != 400 || paused.getMediaTime() <= 0)
            throw new Exception("handoff MIDI did not reach a playing pause point");
        /* Explicitly stop before starting the other Player: this must keep
         * the position, unlike a Player displaced while still playing. */
        paused.stop();
        if (paused.getState() != 300) throw new Exception("handoff stop state=" + paused.getState());
        long stopped = paused.getMediaTime();
        other.start();
        awaitEnd(other);
        if (paused.getState() != 300 || paused.getMediaTime() != stopped)
            throw new Exception("another MIDI changed the paused position");
        paused.start();
        if (paused.getState() != 400 || paused.getMediaTime() < stopped)
            throw new Exception("handoff resume reset the position");
        awaitEnd(paused);
        long finished = paused.getMediaTime();
        if (finished <= stopped) throw new Exception("handoff resume made no progress");
        System.out.println("REPLAY_MIDI_PAUSED_HANDOFF_OK round=" + round +
                           " stopped_us=" + stopped + " finished_us=" + finished);
        Thread.sleep(150);
    }
    public void run() {
        Player a = null, b = null;
        try {
            for (int sound = 0; sound < 3; ++sound) {
                a = Manager.createPlayer(getClass().getResourceAsStream("/replay-midi-" + sound + ".mid"), "audio/midi");
                for (int round = 1; round <= 3; ++round) {
                    a.start(); awaitEnd(a);
                    System.out.println("REPLAY_MIDI_OK sound=" + sound + " round=" + round);
                    Thread.sleep(150);
                }
                a.close(); a = null;
            }
            a = Manager.createPlayer(getClass().getResourceAsStream("/replay-midi-1.mid"), "audio/midi");
            b = Manager.createPlayer(getClass().getResourceAsStream("/replay-midi-2.mid"), "audio/midi");
            a.setLoopCount(3); a.start(); Thread.sleep(40);
            b.start(); Thread.sleep(80);
            if (a.getState() != 300) throw new Exception("preempted MIDI remains STARTED: " + a.getState());
            awaitEnd(b);
            a.setLoopCount(1); a.start(); awaitEnd(a);
            System.out.println("REPLAY_MIDI_PREEMPT_OK");
            /* The completed Player starts from its beginning for each round;
             * its explicit stop/start pair must preserve the paused position. */
            for (int round = 1; round <= 2; ++round) resumeRound(a, round);
            for (int round = 1; round <= 2; ++round) pausedHandoffRound(a, b, round);
        } catch (Exception e) {
            System.out.println("REPLAY_MIDI_ERROR " + e);
        } finally {
            if (a != null) a.close();
            if (b != null) b.close();
            notifyDestroyed();
        }
    }
    static final class Screen extends Canvas {
        protected void paint(Graphics g) {
            g.setColor(0); g.fillRect(0, 0, getWidth(), getHeight());
        }
    }
}
