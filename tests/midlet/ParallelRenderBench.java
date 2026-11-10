import javax.microedition.midlet.MIDlet;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;
import javax.microedition.lcdui.game.GameCanvas;
import javax.microedition.m3g.*;

/** Fixed work and pixels, independent of wall-clock animation or display FPS. */
public final class ParallelRenderBench extends MIDlet {
    protected void startApp() {
        Screen screen = new Screen(this);
        Display.getDisplay(this).setCurrent(screen);
        new Thread(screen).start();
    }
    protected void pauseApp() {}
    protected void destroyApp(boolean unconditional) {}
    static final class Screen extends GameCanvas implements Runnable {
        final ParallelRenderBench owner;
        Screen(ParallelRenderBench owner) { super(false); this.owner=owner; setFullScreenMode(true); }
        public void run() {
            try { measure(); }
            catch (Throwable error) { error.printStackTrace(); }
            owner.notifyDestroyed();
        }
        void measure() {
            final int width=240, height=320, frames=36, draws=4;
            Image image=Image.createImage(width,height);
            Graphics target=image.getGraphics();
            Graphics3D renderer=Graphics3D.getInstance();
            Camera camera=new Camera();
            camera.setPerspective(60, (float)width/height, 1, 1000);
            renderer.setCamera(camera,new Transform());
            VertexArray positions=new VertexArray(3,3,2);
            positions.set(0,3,new short[]{-150,-150,-220,150,-150,-240,0,150,-200});
            VertexArray uv=new VertexArray(3,2,2);
            uv.set(0,3,new short[]{0,0,32,0,16,32});
            VertexArray colours=new VertexArray(3,4,1);
            VertexBuffer vertices=new VertexBuffer();
            vertices.setPositions(positions,1,null);
            vertices.setTexCoords(0,uv,1.0f/32,null);
            vertices.setDefaultColor(0xff96d470);
            TriangleStripArray indices=new TriangleStripArray(0,new int[]{3});
            PolygonMode polygon=new PolygonMode();
            polygon.setCulling(PolygonMode.CULL_NONE);
            polygon.setShading(PolygonMode.SHADE_SMOOTH);
            CompositingMode compositing=new CompositingMode();
            compositing.setBlending(CompositingMode.ALPHA);
            Appearance appearance=new Appearance();
            appearance.setPolygonMode(polygon);
            appearance.setCompositingMode(compositing);
            byte[] pixels=new byte[32*32*4];
            for(int y=0;y<32;y++) for(int x=0;x<32;x++) {
                int at=(y*32+x)*4;
                pixels[at]=(byte)(x*7); pixels[at+1]=(byte)(y*7);
                pixels[at+2]=(byte)((x^y)*7); pixels[at+3]=(byte)255;
            }
            Texture2D texture=new Texture2D(new Image2D(Image2D.RGBA,32,32,pixels));
            texture.setWrapping(Texture2D.WRAP_CLAMP,Texture2D.WRAP_CLAMP);
            texture.setBlending(Texture2D.FUNC_MODULATE);
            Background background=new Background();
            int[] readback=new int[width*height];
            String[] names={"flat","gouraud","nearest","linear-alpha"};
            for(int phase=0;phase<4;phase++) {
                appearance.setTexture(0,phase>=2?texture:null);
                texture.setFiltering(Texture2D.FILTER_BASE_LEVEL,
                    phase==3?Texture2D.FILTER_LINEAR:Texture2D.FILTER_NEAREST);
                colours.set(0,3,phase==3?
                    new byte[]{(byte)255,40,80,80,40,(byte)255,80,(byte)180,80,40,(byte)255,(byte)255}:
                    new byte[]{(byte)255,40,80,(byte)255,40,(byte)255,80,(byte)255,80,40,(byte)255,(byte)255});
                vertices.setColors(phase==1||phase==3?colours:null);
                long start=System.currentTimeMillis();
                for(int frame=0;frame<frames;frame++) {
                    background.setColor(0xff102030+frame*1031);
                    renderer.bindTarget(target,true,0);
                    renderer.setViewport(0,0,width,height);
                    renderer.clear(background);
                    for(int draw=0;draw<draws;draw++)
                        renderer.render(vertices,indices,appearance,null);
                    renderer.releaseTarget();
                }
                long elapsed=System.currentTimeMillis()-start;
                image.getRGB(readback,0,width,0,0,width,height);
                int hash=0;
                for(int i=0;i<readback.length;i++) hash=hash*31+readback[i];
                System.out.println("PARALLEL_BENCH phase="+names[phase]+" frames="+frames+
                    " draws="+(frames*draws)+" checksum="+Integer.toHexString(hash)+" elapsed_ms="+elapsed);
            }
            Graphics screen=getGraphics();
            screen.drawImage(image,0,0,Graphics.TOP|Graphics.LEFT);
            flushGraphics();
            System.out.println("PARALLEL_BENCH_DONE");
        }
    }
}
