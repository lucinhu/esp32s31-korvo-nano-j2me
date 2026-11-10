import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;
import javax.microedition.media.Manager;
import javax.microedition.media.Player;
import java.io.InputStream;

/** On-board verification of the Espressif adapters. Leaves its result visible. */
public final class MediaBench extends MIDlet implements Runnable {
    private Screen screen;
    private Image[] images = new Image[4];
    private String phase = "Starting";
    private int passed, failed;

    protected void startApp() {
        screen = new Screen();
        Display.getDisplay(this).setCurrent(screen);
        new Thread(this).start();
    }
    protected void pauseApp() {}
    protected void destroyApp(boolean unconditional) {}

    public void run() {
        image(0, "/rgba.png", 48, 48);
        image(1, "/palette.png", 48, 48);
        image(2, "/odd.jpg", 51, 37);
        image(3, "/cmyk.jpg", 51, 37);
        audio("WAV", "/tone.wav", "audio/x-wav");
        audio("MP3", "/tone.mp3", "audio/mpeg");
        audio("AAC", "/tone.aac", "audio/aac");
        audio("AMR-NB", "/tone.amr", "audio/amr");
        audio("AMR-WB", "/tone.awb", "audio/amr-wb");
        audio("MIDI", "/notes.mid", "audio/midi");
        midiControls();
        phase = "PASS " + passed + " / FAIL " + failed;
        screen.repaint();
        System.out.println("MEDIA_BENCH_DONE pass=" + passed + " fail=" + failed);
    }

    private void image(int index, String path, int width, int height) {
        try {
            Image image = Image.createImage(path);
            if (image.getWidth() != width || image.getHeight() != height)
                throw new Exception("wrong image dimensions");
            int[] pixel = new int[1];
            image.getRGB(pixel, 0, 1, 0, 0, 1, 1);
            if (index == 0 && (pixel[0] >>> 24) != 0)
                throw new Exception("PNG lost alpha");
            if (index == 1 && pixel[0] != 0x8000ff00)
                throw new Exception("palette PNG wrong ARGB");
            if (index >= 2 && (pixel[0] >>> 24) != 255)
                throw new Exception("JPEG must return opaque ARGB pixels");
            if (index >= 2 && ((pixel[0] & 255) < 70 || ((pixel[0] >>> 16) & 255) > 40))
                throw new Exception("JPEG channel order or pixel conversion is wrong");
            images[index] = image;
            passed++;
            System.out.println("MEDIA_IMAGE_OK " + path + " " + width + "x" + height + " argb=" + pixel[0]);
        } catch (Exception e) {
            failed++;
            System.out.println("MEDIA_IMAGE_ERROR " + path + " " + e);
        }
    }

    private void audio(String name, String path, String type) {
        Player player = null;
        InputStream stream = null;
        try {
            phase = name;
            screen.repaint();
            stream = getClass().getResourceAsStream(path);
            player = Manager.createPlayer(stream, type);
            player.realize(); player.prefetch();
            System.out.println("MEDIA_AUDIO_BEGIN " + name + " duration=" + player.getDuration());
            player.start();
            Thread.sleep(1400);
            long time = player.getMediaTime();
            int state = player.getState();
            if (time < 400000 || state != 300) throw new Exception("time=" + time + " state=" + state);
            passed++;
            System.out.println("MEDIA_AUDIO_OK " + name + " time=" + time + " state=" + state);
        } catch (Exception e) {
            failed++;
            System.out.println("MEDIA_AUDIO_ERROR " + name + " " + e);
        } finally {
            if (player != null) player.close();
            if (stream != null) try { stream.close(); } catch (Exception e) {}
        }
    }

    private void midiControls() {
        Player player = null;
        try {
            phase = "MIDI controls";
            screen.repaint();
            player = Manager.createPlayer(getClass().getResourceAsStream("/notes.mid"), "audio/midi");
            player.realize(); player.prefetch(); player.setLoopCount(-1);
            player.start(); Thread.sleep(350);
            player.stop();
            System.out.println("MEDIA_MIDI_STOP time=" + player.getMediaTime());
            player.setMediaTime(200000);
            player.start(); Thread.sleep(400);
            player.stop();
            passed++;
            System.out.println("MEDIA_MIDI_CONTROLS_OK time=" + player.getMediaTime());
        } catch (Exception e) {
            failed++;
            System.out.println("MEDIA_MIDI_CONTROLS_ERROR " + e);
        } finally {
            if (player != null) player.close();
        }
    }

    final class Screen extends Canvas {
        Screen() { setFullScreenMode(true); }
        protected void paint(Graphics g) {
            g.setColor(0x102030); g.fillRect(0, 0, getWidth(), getHeight());
            g.setColor(0xffffff);
            g.drawString("Espressif media", 8, 10, 20);
            g.drawString("中文字体 测试 ABC123", 8, 40, 20);
            g.drawString(phase, 8, 70, 20);
            for (int i = 0; i < images.length; i++)
                if (images[i] != null) g.drawImage(images[i], 8 + (i % 2) * 95, 110 + (i / 2) * 65, 20);
            g.setColor(0xffcc66);
            g.drawString("PNG / JPEG / MIDI", 8, 265, 20);
        }
    }
}
