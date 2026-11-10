#!/usr/bin/env python3
"""Check the actual port scaler against an independent rectangle oracle.

Extract just the production function to compile on the host with an in-memory
LCD and cache-flush stub. No ESP-IDF headers or duplicate scaler are needed.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / "main/korvo_port.c").read_text()
start = source.index("static void present_rect(")
end = source.index("\n#endif\nstatic void display_present", start)
production = source[start:end]
harness = r'''
#include "korvo_port.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LCD_W 800
#define LCD_H 480
static uint16_t actual[LCD_W*LCD_H], expected[LCD_W*LCD_H];
static uint16_t *korvo_lcd(void) { return actual; }
static void korvo_flush_rect(int x, int y, int w, int h) {
    assert(x>=0 && y>=0 && w>=0 && h>=0 && x+w<=LCD_W && y+h<=LCD_H);
}
'''
test = r'''
static uint32_t rng=0x12345678;
static unsigned random_u(void) { return rng=rng*1664525+1013904223; }
int main(void) {
    /* Every accepted launcher resolution must remain centered, integer-scaled
     * and inside the control-free viewport. Check the first excluded scale. */
    for (int w=64;w<=320;++w) for (int h=64;h<=320;++h) {
        launcher_rect v=launcher_game_rect(w,h,true);
        assert(v.w%w==0 && v.h%h==0 && v.w/w==v.h/h);
        int scale=v.w/w;
        assert(scale>=1 && ((scale+1)*w>LAUNCHER_VIEW_W ||
                            (scale+1)*h>LAUNCHER_VIEW_H));
        assert(v.x>=204 && v.x+v.w<=596);
        assert(v.y>=LAUNCHER_VIEW_TOP && v.y+v.h<=476);
        assert(v.x==(LCD_W-v.w)/2);
        assert(v.y==LAUNCHER_VIEW_TOP+(LAUNCHER_VIEW_H-v.h)/2);
        v=launcher_game_rect(w,h,false);
        assert(v.w==LAUNCHER_VIEW_W || v.h==LAUNCHER_VIEW_H);
        assert(v.x>=204 && v.x+v.w<=596 && v.y>=44 && v.y+v.h<=476);
        assert(abs(v.w*h-v.h*w)<(w>h?w:h));
    }
    const int sizes[][2]={{240,320},{176,208},{128,160},{320,240},{64,320},{320,64}};
    const int scales[]={1,2,2,1,1,1};
    unsigned checks=0;
    for (int pixel=0;pixel<=1;++pixel)
    for (unsigned size=0;size<sizeof(sizes)/sizeof(sizes[0]);++size) {
        korvo_port_state s={ .width=sizes[size][0], .height=sizes[size][1] };
        s.pixels=calloc(s.width*s.height,2);
        s.previous_pixels=malloc(s.width*s.height*2);
        assert(s.pixels && s.previous_pixels);
        launcher_rect v=launcher_game_rect(s.width,s.height,pixel);
        if (pixel) assert(v.w==s.width*scales[size] && v.h==s.height*scales[size]);
        s.view_x=v.x; s.view_y=v.y; s.view_w=v.w; s.view_h=v.h;
        for (int i=0;i<s.view_w;++i) s.xmap[i]=i*s.width/s.view_w;
        for (int i=0;i<s.view_h;++i) s.ymap[i]=i*s.height/s.view_h;
        memset(actual,0xa5,sizeof(actual)); memcpy(expected,actual,sizeof(actual));
        for (int frame=0;frame<300;++frame) {
            int x=0,y=0,w=s.width,h=s.height;
            if (frame<10 || (frame>=30 && frame%5==0)) {
                uint16_t color=random_u();
                for (int i=0;i<s.width*s.height;++i) s.pixels[i]=color;
            } else if (frame>=20) {
                for (int i=0;i<50;++i) s.pixels[random_u()%(s.width*s.height)]=random_u();
            }
            if (frame>=30 && frame%3) {
                x=(int)(random_u()%(s.width*2))-s.width/2;
                y=(int)(random_u()%(s.height*2))-s.height/2;
                w=random_u()%(s.width+1); h=random_u()%(s.height+1);
            }
            int x0=x<0?0:x, y0=y<0?0:y;
            int x1=x+w>s.width?s.width:x+w, y1=y+h>s.height?s.height:y+h;
            if (!s.probes_valid || w<=0 || h<=0) { x0=0;y0=0;x1=s.width;y1=s.height; }
            /* Independently commit only destination pixels whose sampled
             * source lies inside the flushed rectangle. Native pixels outside
             * it may intentionally differ from the last displayed frame. */
            for (int dy=0;dy<s.view_h;++dy) for (int dx=0;dx<s.view_w;++dx) {
                int sx=dx*s.width/s.view_w, sy=dy*s.height/s.view_h;
                if (sx>=x0 && sx<x1 && sy>=y0 && sy<y1)
                    expected[(s.view_y+dy)*LCD_W+s.view_x+dx]=s.pixels[sy*s.width+sx];
            }
            present_rect(&s,x,y,w,h);
            if (memcmp(actual,expected,sizeof(actual))) {
                fprintf(stderr,"scaler mismatch %dx%d frame=%d rect=%d,%d,%d,%d\n",s.width,s.height,frame,x,y,w,h);
                return 1;
            }
            ++checks;
        }
        assert(s.unchanged_frames>0 && s.updated_frames>0);
        free(s.pixels); free(s.previous_pixels);
    }
    printf("scaler: %u frames match the rectangle oracle, including unflushed pixels and unchanged frames\n",checks);
}
'''
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    c = directory / "scaler_test.c"
    c.write_text(harness + production + test)
    executable = directory / "scaler_test"
    subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", "-I" + str(root / "main"),
                    "-I" + str(root / "third_party/nano_j2me/include"),
                    str(c), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
