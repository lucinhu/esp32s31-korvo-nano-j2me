import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.*;
public final class FormScrollDemo extends MIDlet implements ItemCommandListener {
    protected void startApp() {
        Form form=new Form("Scroll test");
        for(int i=0;i<24;i++) form.append(new StringItem("ROW "+i,"Line one\nLine two"));
        StringItem end=new StringItem("SCROLL END","Press FIRE to verify last item");
        end.setDefaultCommand(new Command("Verify",Command.ITEM,1));
        end.setItemCommandListener(this);form.append(end);
        Display.getDisplay(this).setCurrent(form);
        System.out.println("FORM_SCROLL_READY");
    }
    protected void pauseApp() {}
    protected void destroyApp(boolean unconditional) {}
    public void commandAction(Command command,Item item) {
        System.out.println("FORM_SCROLL_LAST_ACTIVATED");notifyDestroyed();
    }
}
