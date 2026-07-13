// sprout_face.h — GC9A01 (240x240 round) renderer for the "Sprout" character.
// Ports the same face shown in the web dashboard so the physical display matches.
// Include ONCE (from main.cpp). Pins per the project pinout:
//   SCL=GPIO7(D8)  SDA=GPIO9(D10)  DC=GPIO43(D6)  CS=GPIO44(D7)  RST=GPIO8(D9)
#pragma once
#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <math.h>

// ---- Display pins ----
#define DISP_SCLK 7
#define DISP_MOSI 9
#define DISP_DC   43
#define DISP_CS   44
#define DISP_RST  8

class LGFX_Sprout : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel;
  lgfx::Bus_SPI      _bus;
public:
  LGFX_Sprout() {
    { auto c = _bus.config();
      c.spi_host   = SPI2_HOST;
      c.spi_mode   = 0;
      c.freq_write = 40000000;
      c.freq_read  = 16000000;
      c.spi_3wire  = false;
      c.use_lock   = true;
      c.dma_channel= SPI_DMA_CH_AUTO;
      c.pin_sclk   = DISP_SCLK;
      c.pin_mosi   = DISP_MOSI;
      c.pin_miso   = -1;
      c.pin_dc     = DISP_DC;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config();
      c.pin_cs   = DISP_CS;
      c.pin_rst  = DISP_RST;
      c.pin_busy = -1;
      c.panel_width  = 240;
      c.panel_height = 240;
      c.offset_x = 0; c.offset_y = 0;
      c.readable = false;
      c.invert   = true;      // GC9A01 usually needs inversion ON
      c.rgb_order= false;     // BGR; flip to true if R/B look swapped
      c.dlen_16bit = false;
      c.bus_shared = false;
      _panel.config(c); }
    setPanel(&_panel);
  }
};

static LGFX_Sprout      lcd;
static lgfx::LGFX_Sprite spr(&lcd);
static bool   dispReady = false;

// ---- color helpers ----
static inline uint16_t C(uint32_t h){ return spr.color565((h>>16)&0xFF,(h>>8)&0xFF,h&0xFF); }
static inline uint32_t mix(uint32_t h,int d){
  int r=((h>>16)&255)+d, g=((h>>8)&255)+d, b=(h&255)+d;
  r=r<0?0:r>255?255:r; g=g<0?0:g>255?255:g; b=b<0?0:b>255?255:b;
  return (r<<16)|(g<<8)|b;
}
static inline float frnd(int i){ float x=sinf(i*127.1f+311.7f)*43758.5f; return x-floorf(x); }

// ---- emotion config (mirror of the JS FACE{} map) ----
enum {EYE_SPARK,EYE_HAPPY,EYE_HEART,EYE_SLEEPY,EYE_WORRY,EYE_WIDE,EYE_TIRED,EYE_SAD};
enum {MO_SMILE,MO_GRIN,MO_TINY,MO_PANT,MO_CHATTER,MO_FROWN};
enum {LF_UP,LF_DROOP,LF_WILT,LF_SHIVER};
enum {FX_NONE,FX_SPARK,FX_HEARTS,FX_STARS,FX_SNOW,FX_RAIN,FX_HEAT};
struct FaceCfg{ uint32_t body,glow,edge; int eye,mouth,leaf,blush,fx;
  bool zzz,sweat,tear,shiver; float dim; };

static FaceCfg cfgFor(const String& e){
  if(e=="happy")   return {0x5fe39a,0x1e5234,0x0a2417,EYE_HAPPY,MO_GRIN,LF_UP,1,FX_SPARK,0,0,0,0,0};
  if(e=="love")    return {0x74dba2,0x3c1626,0x180810,EYE_HEART,MO_SMILE,LF_UP,2,FX_HEARTS,0,0,0,0,0};
  if(e=="sleepy")  return {0x4d9c87,0x13243d,0x060c18,EYE_SLEEPY,MO_TINY,LF_DROOP,0,FX_STARS,1,0,0,0,0.55};
  if(e=="thirsty") return {0xc4cf73,0x2c2510,0x161203,EYE_WORRY,MO_PANT,LF_WILT,0,FX_NONE,0,1,0,0,0};
  if(e=="cold")    return {0x82c6db,0x132f49,0x07182a,EYE_WIDE,MO_CHATTER,LF_SHIVER,1,FX_SNOW,0,0,0,1,0};
  if(e=="hot")     return {0xe8a06f,0x3c1808,0x1c0a02,EYE_TIRED,MO_PANT,LF_DROOP,0,FX_HEAT,0,1,0,0,0};
  if(e=="sad")     return {0x90a7b9,0x1b1828,0x0c0a14,EYE_SAD,MO_FROWN,LF_WILT,0,FX_RAIN,0,0,1,0,0};
  if(e=="offline") return {0x8a9bad,0x1a2230,0x090d15,EYE_WORRY,MO_TINY,LF_DROOP,0,FX_NONE,0,0,0,0,0};
  return {0x57c98a,0x173a28,0x081711,EYE_SPARK,MO_SMILE,LF_UP,0,FX_NONE,0,0,0,0,0}; // neutral
}

// stroke an arc as short wide-line segments (matches canvas arc angles exactly)
static void arcStroke(float cx,float cy,float r,float a0,float a1,float w,uint16_t col){
  int n=(int)(fabsf(a1-a0)*r/5.0f); if(n<2)n=2;
  float px=cx+cosf(a0)*r, py=cy+sinf(a0)*r;
  for(int i=1;i<=n;i++){ float a=a0+(a1-a0)*i/n, x=cx+cosf(a)*r, y=cy+sinf(a)*r;
    spr.drawWideLine(px,py,x,y,w,col); px=x; py=y; }
}
static void heartF(float x,float y,float s,uint16_t col){
  spr.fillCircle(x-s*0.5f,y-s*0.35f,s*0.52f,col);
  spr.fillCircle(x+s*0.5f,y-s*0.35f,s*0.52f,col);
  spr.fillTriangle(x-s*0.95f,y-s*0.15f, x+s*0.95f,y-s*0.15f, x,y+s*0.95f, col);
}
static void teardropF(float x,float y,float s,uint16_t col){
  spr.fillTriangle(x,y-s*1.5f, x-s*0.75f,y+s*0.1f, x+s*0.75f,y+s*0.1f, col);
  spr.fillCircle(x,y+s*0.25f,s*0.72f,col);
}

// ---- caption (tiny subtitle while talking) ----
static String _caption=""; static unsigned long _captionUntil=0;
void faceSetCaption(const String& t, unsigned long durMs){ _caption=t; _captionUntil=millis()+durMs; }

// ---- animation state ----
static String _shown="x"; static unsigned long _changeAt=0;
static unsigned long _blinkAt=0,_nextBlink=900,_nextLook=1500; static bool _dbl=false;
static float _lookX=0,_lookY=0,_lookTX=0,_lookTY=0;

static void drawFaceHW(const String& emoIn, bool offline, unsigned long t){
  if(!dispReady) return;
  String emo = offline ? String("offline") : emoIn;
  FaceCfg f=cfgFor(emo);
  if(emo!=_shown){_shown=emo; _changeAt=t;}

  // schedule blink + glance
  if(t>_nextBlink && _blinkAt==0){_blinkAt=t; _dbl=frnd(t)<0.28f; _nextBlink=t+1800+(unsigned)(frnd(t+1)*3200);}
  if(t>_nextLook){_nextLook=t+1100+(unsigned)(frnd(t+2)*2600);
    if(frnd(t+3)<0.45f){_lookTX=0;_lookTY=0;} else {_lookTX=(frnd(t+4)*2-1)*6; _lookTY=(frnd(t+5)*2-1)*4;} }
  _lookX+=(_lookTX-_lookX)*0.12f; _lookY+=(_lookTY-_lookY)*0.12f;
  float open=1;
  if(_blinkAt>0){ float d=t-_blinkAt; auto bw=[](float x){return x<90?1-x/90:(x<180?(x-90)/90:1);};
    if(d<180)open=bw(d); else if(_dbl&&d>=240&&d<420)open=bw(d-240);
    if(d>(_dbl?420:180))_blinkAt=0; }
  if(f.eye==EYE_SLEEPY)open=fminf(open,0.30f);
  if(f.eye==EYE_TIRED) open=fminf(open,0.55f);
  if(f.eye==EYE_WIDE)  open=1;

  // ---- background: glow disk on darker edge ----
  spr.fillScreen(C(f.edge));
  spr.fillCircle(120,120,108,C(f.glow));

  // ---- particles (screen space) ----
  if(f.fx==FX_STARS) for(int i=0;i<20;i++){ float x=frnd(i)*240,y=frnd(i+7)*150+12,
      a=0.3f+0.6f*(0.5f+0.5f*sinf(t/500.0f+i*2)); int v=(int)(180*a+40);
      spr.fillCircle(x,y,1+2*frnd(i+3), spr.color565(v,v+20,255)); }
  if(f.fx==FX_SNOW) for(int i=0;i<24;i++){ float x=fmodf(frnd(i)*240+sinf(t/700.0f+i)*12+240,240),
      y=fmodf(t*0.03f*(0.5f+frnd(i+2))+frnd(i+5)*260,260);
      spr.fillCircle(x,y,1+2*frnd(i+1), C(0xeaf6ff)); }
  if(f.fx==FX_HEARTS) for(int i=0;i<10;i++){ float prog=fmodf(t*0.045f*(0.5f+frnd(i+1))+frnd(i+4)*260,270),
      y=240-prog, x=frnd(i)*200+20+sinf(t/450.0f+i)*9; if(prog>20&&prog<260) heartF(x,y,4+4*frnd(i+2),C(0xff86ad)); }
  if(f.fx==FX_SPARK) for(int i=0;i<12;i++){ float ph=fmodf(t/650.0f+frnd(i)*7,4), sc=sinf(ph*M_PI/2);
      if(sc<=0)continue; float x=frnd(i+1)*220+10,y=frnd(i+6)*200+15,r=3+5*sc;
      spr.fillTriangle(x,y-r,x-r*0.3f,y,x+r*0.3f,y,C(0xfff3b4)); spr.fillTriangle(x,y+r,x-r*0.3f,y,x+r*0.3f,y,C(0xfff3b4)); }
  if(f.fx==FX_RAIN) for(int i=0;i<20;i++){ float x=frnd(i)*240, y=fmodf(t*0.2f*(0.6f+frnd(i+1))+frnd(i+3)*240,260);
      spr.drawWideLine(x,y,x-2,y+9,2,C(0x9fc8e6)); }
  if(f.fx==FX_HEAT){ float a=t/2600.0f; for(int j=0;j<8;j++){ float an=a+j*M_PI/4;
      spr.fillTriangle(196+cosf(an)*14,46+sinf(an)*14, 196+cosf(an+0.25f)*22,46+sinf(an+0.25f)*22,
        196+cosf(an-0.25f)*22,46+sinf(an-0.25f)*22, C(0xffc45a)); }
      spr.fillCircle(196,46,11,C(0xffd278)); }

  // ---- character transform (breathe + pop + shiver) ----
  float breath=sinf(t/850.0f), sxk=1-0.03f*breath, syk=1+0.04f*breath;
  float dt=t-_changeAt, pop=1+0.17f*expf(-dt/210.0f)*cosf(dt/72.0f);
  float SX=sxk*pop, SY=syk*pop, shv=f.shiver?sinf(t/40.0f)*1.7f:0;
  float BX=120+shv, BY=142;
  auto TX=[&](float lx){return BX+lx*SX;}; auto TY=[&](float ly){return BY+ly*SY;};
  auto SC=[&](float v){return v*((SX+SY)*0.5f);};
  const float BW=74,BH=66; const uint16_t DK=C(0x1e2b25), WH=C(0xffffff);

  // ground shadow
  spr.fillEllipse(TX(0),TY(BH-2),SC(BW*0.7f),SC(9),C(0x000000));
  // leaves + stem
  float bend=0,droop=0,jit=0;
  if(f.leaf==LF_DROOP)bend=0.5f; else if(f.leaf==LF_WILT){bend=1.0f;droop=8;} else if(f.leaf==LF_SHIVER)jit=sinf(t/45.0f)*0.12f;
  float sway=sinf(t/700.0f)*0.1f+jit;
  spr.drawWideLine(TX(0),TY(-BH+8),TX(0),TY(-BH-6+droop),SC(5),C(0x3f8f5e));
  for(int k=0;k<2;k++){ float ang=(k?(0.6f-bend):(-0.6f+bend))+sway, len=SC(26);
    float lx=TX(0),ly=TY(-BH-4+droop);
    // leaf as a small filled ellipse oriented by angle (approx of the teardrop leaf)
    float mx=lx+sinf(ang)*len*0.5f, my=ly-cosf(ang)*len*0.5f;
    spr.fillEllipse(mx,my,len*0.22f,len*0.5f,k?C(0x46a86b):C(0x52b878)); }

  // body + shading + rim
  spr.fillEllipse(TX(0),TY(0),SC(BW),SC(BH),C(f.body));
  spr.fillEllipse(TX(-18),TY(-22),SC(BW*0.5f),SC(BH*0.4f),C(mix(f.body,28)));   // highlight
  spr.fillEllipse(TX(14),TY(26),SC(BW*0.55f),SC(BH*0.35f),C(mix(f.body,-26)));  // shade
  arcStroke(TX(0),TY(0),SC(BW-2),M_PI*1.15f,M_PI*1.85f,2,C(mix(f.body,50)));    // rim light

  // cheeks
  if(f.blush){ uint16_t bl=C(0xff8296);
    spr.fillCircle(TX(-40),TY(12),SC(f.blush>1?10:8),bl);
    spr.fillCircle(TX(40),TY(12),SC(f.blush>1?10:8),bl); }

  // ---- eyes ----
  const float EYX=27,EYY=-12; float lx=_lookX*0.4f, ly=_lookY*0.4f;
  auto ball=[&](float ex,float ey,float kx){ float rx=SC(15*kx), ry=fmaxf(SC(2.5f),SC(18*kx)*open);
    if(open<0.12f){ arcStroke(ex,ey-SC(3),SC(9),M_PI*0.18f,M_PI*0.82f,SC(4),DK); return; }
    spr.fillEllipse(ex+SC(lx),ey+SC(ly),rx,ry,DK);
    if(open>0.45f){ spr.fillCircle(ex-rx*0.32f+SC(lx),ey-ry*0.34f+SC(ly),rx*0.36f,WH);
      spr.fillCircle(ex+rx*0.34f+SC(lx),ey+ry*0.28f+SC(ly),rx*0.16f,WH);} };
  auto arcUp=[&](float ex,float ey){ arcStroke(ex,ey+SC(4),SC(12),M_PI*1.18f,M_PI*1.82f,SC(5),DK); };
  auto brow=[&](float ex,float ey,int dir){ spr.drawWideLine(ex-SC(10*dir),ey-SC(16),ex+SC(9*dir),ey-SC(9),SC(4),DK); };
  float ELx=TX(-EYX),ERx=TX(EYX),EYs=TY(EYY);
  if(f.eye==EYE_HAPPY){ arcUp(ELx,EYs); arcUp(ERx,EYs); }
  else if(f.eye==EYE_HEART){ float p=1+0.08f*sinf(t/200.0f); heartF(ELx+SC(lx),EYs+SC(ly),SC(13*p),C(0xff5d86)); heartF(ERx+SC(lx),EYs+SC(ly),SC(13*p),C(0xff5d86)); }
  else { float kx=(f.eye==EYE_WIDE)?1.12f:1.0f; ball(ELx,EYs,kx); ball(ERx,EYs,kx);
    if(f.eye==EYE_SAD){ brow(ELx,EYs-SC(3),-1); brow(ERx,EYs-SC(3),1); }
    if(f.eye==EYE_WORRY){ brow(ELx,EYs,-1); brow(ERx,EYs,1); } }

  // ---- mouth ----
  float MY=TY(22); float mcx=TX(0);
  if(f.mouth==MO_SMILE)      arcStroke(mcx,MY-SC(6),SC(15),M_PI*0.18f,M_PI*0.82f,SC(4.5f),DK);
  else if(f.mouth==MO_TINY)  arcStroke(mcx,MY-SC(4),SC(7),M_PI*0.2f,M_PI*0.8f,SC(4),DK);
  else if(f.mouth==MO_FROWN) arcStroke(mcx,MY+SC(14),SC(14),M_PI*1.2f,M_PI*1.8f,SC(4.5f),DK);
  else if(f.mouth==MO_CHATTER){ float px=mcx-SC(13),py=MY; for(int i=0;i<=4;i++){ float x=mcx-SC(13)+SC(6.5f*i),y=MY+SC(i%2?5:-5);
      if(i)spr.drawWideLine(px,py,x,y,SC(4),DK); px=x;py=y; } }
  else if(f.mouth==MO_GRIN){ spr.fillEllipse(mcx,MY,SC(15),SC(12),DK);
      spr.fillRect(mcx-SC(16),MY-SC(13),SC(32),SC(12),C(f.body));            // flatten top
      spr.fillEllipse(mcx,MY+SC(4),SC(7),SC(4),C(0xff7c98)); }               // tongue
  else if(f.mouth==MO_PANT){ spr.fillEllipse(mcx,MY,SC(9),SC(8),DK);
      spr.fillEllipse(mcx,MY+SC(5),SC(6),SC(5),C(0xff8fa3)); }

  // ---- foreground fx near the face ----
  float fy=TY(EYY);
  if(f.sweat) teardropF(TX(46),fy-SC(8)+sinf(t/200.0f),SC(5),C(0xbfe6ff));
  if(emo=="thirsty") teardropF(TX(-46),fy-SC(4)+sinf(t/240.0f),SC(5),C(0x8fd0ff));
  if(f.tear){ float fl=fmodf(t/9.0f,26); teardropF(TX(-30),fy+SC(14)+fl,SC(5),C(0x9fd8ff)); }
  if(f.zzz){ float zb=sinf(t/450.0f)*2; spr.setTextColor(C(0xcfe3ff));
    spr.setTextSize(2); spr.drawString("z",150,66+zb); spr.setTextSize(3); spr.drawString("Z",164,48-zb); }

  // ---- offline: searching-wifi icon (rings build up, then a red slash) ----
  if(offline){
    const float cx=120, cy=60; float cyc=fmodf(t/300.0f,4);
    for(int k=0;k<3;k++){ bool on=cyc>(k+1);
      arcStroke(cx,cy,8+k*9, M_PI*1.25f, M_PI*1.75f, 4, on?C(0x9fd9ff):C(0x35414d)); }
    spr.fillCircle(cx,cy,3, cyc>1?C(0x9fd9ff):C(0x4a5662));
    if(cyc>3) spr.drawWideLine(cx-20,cy-22,cx+20,cy+6,4,C(0xff6b6b));   // "no signal" slash
    spr.setTextColor(C(0xcfe3ff)); spr.setTextSize(1);
    spr.drawString("looking for internet...",120,214);
  }

  // ---- tiny subtitle while talking (word-wrap to <=2 lines at the bottom) ----
  if(_caption.length() && millis()<_captionUntil){
    spr.setTextSize(1); spr.setTextDatum(middle_center);
    const int maxc=22;                              // ~chars per line at size 1
    String l1=_caption, l2="";
    if(_caption.length()>maxc){ int sp=_caption.lastIndexOf(' ',maxc);
      if(sp<6)sp=maxc; l1=_caption.substring(0,sp); l2=_caption.substring(sp+1);
      if(l2.length()>maxc){ l2=l2.substring(0,maxc-1)+"."; } }
    int n=l2.length()?2:1, lh=14, y0=212-(n-1)*lh;
    for(int i=0;i<n;i++){ String s=i?l2:l1; int w=spr.textWidth(s);
      spr.fillRect(120-w/2-4,y0+i*lh-7,w+8,14,C(0x0a1410)); }
    spr.setTextColor(C(0xeafff2));
    spr.drawString(l1,120,y0); if(l2.length())spr.drawString(l2,120,y0+lh);
  }

  spr.pushSprite(0,0);
}

// ---- public API ----
static void faceDisplaySetup(){
  if(!lcd.init()){ Serial.println("[DISP] init returned false (check wiring)"); }
  lcd.setRotation(0);
  lcd.fillScreen(TFT_BLACK);
  // boot self-test so first plug-in confirms wiring/colors:
  lcd.fillScreen(TFT_RED);   delay(250);
  lcd.fillScreen(TFT_GREEN); delay(250);
  lcd.fillScreen(TFT_BLUE);  delay(250);
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_WHITE); lcd.setTextDatum(middle_center);
  lcd.setTextSize(2); lcd.drawString("PetPlant", 120, 120);
  delay(500);
  // sprite buffer in PSRAM (plenty of room) for flicker-free animation
  spr.setColorDepth(16); spr.setPsram(true);
  if(!spr.createSprite(240,240)){ Serial.println("[DISP] sprite alloc failed"); return; }
  spr.setTextDatum(middle_center);
  dispReady = true;
  Serial.println("[DISP] GC9A01 ready");
}
static inline void faceDisplayRender(const String& emo, bool offline, unsigned long t){ drawFaceHW(emo,offline,t); }
