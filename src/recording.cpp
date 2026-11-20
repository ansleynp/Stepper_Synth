#include "recording.h"
#include <string.h>

namespace recording {
static Event events[MAX_EVENTS];
static size_t used = 0, cursor = 0;
static uint32_t epoch = 0, duration = 0;
static Mode current = IDLE;
static uint8_t midiFile[MAX_FILE_BYTES];
static bool activeNotes[128];

void start(uint32_t now) { used=0; cursor=0; duration=0; epoch=now; memset(activeNotes,0,sizeof(activeNotes)); current=RECORDING; }
void stop(uint32_t now) {
  if(current==RECORDING) {
    if(now) duration=now-epoch;
    for(int i=0;i<128 && used<MAX_EVENTS;i++) if(activeNotes[i])
      events[used++]={duration,uint8_t(i),0,false};
  }
  current=IDLE; cursor=0;
}
void reset() { stop(); used=0; duration=0; }
void play(uint32_t now) { if(current!=IDLE || !used) return; cursor=0; epoch=now; current=PLAYING; }
Mode mode() { return current; }
size_t count() { return used; }
void capture(uint32_t now, uint8_t note, uint8_t velocity, bool on) {
  if(current!=RECORDING || used>=MAX_EVENTS || note>127) return;
  events[used++]={uint32_t(now-epoch),note,velocity,on};
  activeNotes[note]=on;
  duration=events[used-1].ms;
}
bool next(uint32_t now, Event &out) {
  if(current!=PLAYING || !used) return false;
  if(cursor>=used) {
    uint32_t period=duration+250;
    if(uint32_t(now-epoch)<period) return false;
    epoch+=period; cursor=0;
  }
  if(uint32_t(now-epoch)<events[cursor].ms) return false;
  out=events[cursor++];
  return true;
}
static bool put(size_t &p, uint8_t v) { if(p>=MAX_FILE_BYTES) return false; midiFile[p++]=v; return true; }
static bool vlq(size_t &p, uint32_t n) {
  uint8_t bytes[5]; int len=0;
  do { bytes[len++]=uint8_t(n&127); n>>=7; } while(n && len<5);
  if(n) return false;
  while(len) { --len; if(!put(p,bytes[len] | (len?0x80:0))) return false; }
  return true;
}
bool exportMidi(const uint8_t *&data, size_t &size) {
  if(current!=IDLE || used==0) return false;
  size_t p=0;
  const uint8_t header[]={ 'M','T','h','d',0,0,0,6,0,0,0,1,1,0xE0,'M','T','r','k',0,0,0,0 };
  for(uint8_t b:header) if(!put(p,b))return false;
  size_t trackStart=p;
  // At 120 BPM, 480 ticks / 500 ms. Round absolute timestamps to avoid drift.
  uint32_t previousTick=0;
  for(size_t i=0;i<used;i++) {
    uint32_t tick=uint32_t((uint64_t(events[i].ms)*480+250)/500);
    if(!vlq(p,tick-previousTick) || !put(p,events[i].on?0x90:0x80) ||
       !put(p,events[i].note) || !put(p,events[i].on?events[i].velocity:0))return false;
    previousTick=tick;
  }
  if(!vlq(p,0) || !put(p,0xFF) || !put(p,0x2F) || !put(p,0))return false;
  uint32_t len=p-trackStart;
  for(int i=0;i<4;i++) midiFile[18+i]=uint8_t(len>>(24-8*i));
  data=midiFile; size=p; return true;
}
static uint32_t read32(const uint8_t *p) { return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3]; }
static bool readVlq(const uint8_t *src,size_t end,size_t &p,uint32_t &out) {
  out=0;
  for(int i=0;i<4;i++) { if(p>=end)return false; uint8_t b=src[p++]; out=(out<<7)|(b&127); if(!(b&128))return true; }
  return false;
}
bool importMidi(const uint8_t *src, size_t size) {
  if(current!=IDLE || size<26 || size>MAX_FILE_BYTES || memcmp(src,"MThd",4) ||
     read32(src+4)!=6 || src[8]!=0 || src[9]!=0 || src[10]!=0 || src[11]!=1 ||
     src[12]!=1 || src[13]!=0xE0 || memcmp(src+14,"MTrk",4))return false;
  if(read32(src+18)>size-22)return false;
  size_t end=22+read32(src+18), p=22, n=0;
  static Event staged[MAX_EVENTS]; // staging lives in static RAM, not task stack
  uint32_t ticks=0, tempo=500000; uint8_t status=0;
  bool sawEnd=false;
  while(p<end) {
    uint32_t delta; if(!readVlq(src,end,p,delta) || UINT32_MAX-ticks<delta)return false;
    ticks+=delta;
    if(p>=end)return false;
    if(src[p]&0x80) status=src[p++];
    if(status==0xFF) {
      if(p>=end)return false;
      uint8_t type=src[p++]; uint32_t length;
      if(!readVlq(src,end,p,length) || length>end-p)return false;
      if(type==0x51) { if(length!=3)return false; tempo=(uint32_t(src[p])<<16)|(uint32_t(src[p+1])<<8)|src[p+2]; if(tempo!=500000)return false; }
      if(type==0x2F) sawEnd=true;
      p+=length;
      if(sawEnd) break;
      status=0;
      continue;
    }
    if(status==0xF0 || status==0xF7) {uint32_t len; if(!readVlq(src,end,p,len)||len>end-p)return false; p+=len; status=0; continue;}
    if(status<0x80 || status>=0xF0)return false;
    uint8_t kind=status&0xF0;
    int bytes=(kind==0xC0 || kind==0xD0)?1:2;
    if(size_t(bytes)>end-p)return false;
    uint8_t a=src[p++],b=bytes==2?src[p++]:0;
    if(a>127 || b>127)return false;
    if(kind==0x90 || kind==0x80) {
      if(n>=MAX_EVENTS)return false;
      uint64_t ms=(uint64_t(ticks)*500+240)/480;
      if(ms>UINT32_MAX)return false;
      staged[n++]={uint32_t(ms),a,b,kind==0x90 && b>0};
    }
  }
  if(!sawEnd || !n)return false;
  memcpy(events,staged,n*sizeof(Event)); used=n; cursor=0; duration=events[n-1].ms;
  return true;
}
}
