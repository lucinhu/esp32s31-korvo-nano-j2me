import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;
import javax.microedition.media.Manager;
import javax.microedition.rms.RecordStore;

/** MIT licensed, board integration demo; no external game data required. */
public final class KorvoDemo extends MIDlet {
    private DemoCanvas canvas;
    protected void startApp() {
        canvas = new DemoCanvas();
        Display.getDisplay(this).setCurrent(canvas);
        new Thread(canvas).start();
    }
    protected void pauseApp() {}
    protected void destroyApp(boolean unconditional) { if (canvas != null) canvas.alive = false; }

    static final class DemoCanvas extends Canvas implements Runnable {
        volatile boolean alive = true;
        int frame, key, px = 80, py = 100, boots;
        DemoCanvas() {
            setFullScreenMode(true);
            try {
                RecordStore store = RecordStore.openRecordStore("korvo", true);
                if (store.getNumRecords() > 0) boots = store.getRecord(1)[0] & 255;
                byte[] value = { (byte) (++boots) };
                if (store.getNumRecords() == 0) store.addRecord(value, 0, 1);
                else store.setRecord(1, value, 0, 1);
                store.closeRecordStore();
                System.out.println("DEMO_RMS_BOOT=" + boots);
            } catch (Exception e) { System.out.println("DEMO_RMS_UNAVAILABLE " + e); }
        }
        public void run() {
            while (alive) {
                frame++;
                repaint();
                try { Thread.sleep(40); } catch (Exception e) {}
            }
        }
        protected void keyPressed(int code) {
            key = code;
            System.out.println("DEMO_KEY_DOWN=" + code);
            if (code == -5 || code == 53) {
                try { Manager.playTone(69, 180, 60); } catch (Exception e) {}
            }
            repaint();
        }
        protected void keyReleased(int code) { System.out.println("DEMO_KEY_UP=" + code); }
        protected void pointerPressed(int x, int y) {
            px = x; py = y;
            System.out.println("DEMO_POINTER=" + x + "," + y);
            repaint();
        }
        protected void pointerDragged(int x, int y) { px = x; py = y; repaint(); }
        protected void paint(Graphics g) {
            int w = getWidth(), h = getHeight();
            g.setColor(0x102030); g.fillRect(0, 0, w, h);
            g.setColor(0xff3030); g.fillRect(0, 0, w / 3, 12);
            g.setColor(0x30ff30); g.fillRect(w / 3, 0, w / 3, 12);
            g.setColor(0x3030ff); g.fillRect(w * 2 / 3, 0, w - w * 2 / 3, 12);
            g.setColor(0xffffff);
            g.drawString("Korvo J2ME", 6, 22, 20);
            g.drawString("中文测试", 6, 44, 20);
            g.drawString("Frame " + frame, 6, 70, 20);
            g.drawString("Key " + key, 6, 92, 20);
            g.drawString("RMS boot " + boots, 6, 114, 20);
            g.setColor(0xffcc22); g.fillRect(frame % (w - 20), h - 30, 20, 20);
            g.setColor(0x33ddff); g.drawRect(px - 8, py - 8, 16, 16);
        }
    }
}
